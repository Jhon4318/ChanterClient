#include "modules/tools/ContainerTransfer.h"
#include "config/config_manager.h"

#include "minecraft/sdk/ui/container_ui.h"
#include "modules/tools/vanilla_item_ids.h"
#include "utils/logger.h"

#include <imgui.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

std::atomic<ContainerTransfer*> ContainerTransfer::s_instance{nullptr};

namespace {
    constexpr int kClicksPerTick = 16;
    constexpr std::uint64_t kContentWaitMs = 1000;
    constexpr std::uint64_t kSettleMs = 500;
    constexpr std::uint64_t kConfirmMs = 2000;
    constexpr std::uint64_t kCloseWaitMs = 1500;
    constexpr std::uint64_t kPhaseGuardMs = 10000;

    std::string normalize_id(std::string value) {
        const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());

        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (!value.empty() && value.find(':') == std::string::npos) {
            value.insert(0, "minecraft:");
        }
        return value;
    }

    std::vector<std::string> parse_id_list(const std::string& csv) {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;

        std::size_t start = 0;
        while (start <= csv.size()) {
            const std::size_t comma = csv.find(',', start);
            const std::size_t end = comma == std::string::npos ? csv.size() : comma;
            std::string id = normalize_id(csv.substr(start, end - start));
            if (!id.empty() && seen.insert(id).second) {
                result.push_back(std::move(id));
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return result;
    }
}

ContainerTransfer::ContainerTransfer()
    : Module("Tools", "ContainerTransfer",
        "設定したコンテナを自分でクリックすると、指定アイテムを一括で回収または収納します。画面は表示しません。") {
    m_sourceBlocks = add_setting<config::StringSetting>(
        "取り出し元ブロック ID",
        "カンマ区切りで複数指定できます。例: minecraft:chest,minecraft:barrel",
        "");
    m_destinationBlocks = add_setting<config::StringSetting>(
        "詰め込み先ブロック ID",
        "カンマ区切りで複数指定できます。例: minecraft:shulker_box,minecraft:white_shulker_box",
        "");
    m_itemIds = add_setting<config::StringSetting>(
        "移動アイテム ID",
        "カンマ区切りで複数指定できます。例: minecraft:iron_ingot,minecraft:gold_ingot",
        "");

    s_instance.store(this, std::memory_order_release);
    minecraft::container_ui::set_listener(this);
}

ContainerTransfer::~ContainerTransfer() {
    minecraft::container_ui::set_listener(nullptr);
    ContainerTransfer* expected = this;
    s_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

ContainerTransfer* ContainerTransfer::instance() {
    return s_instance.load(std::memory_order_acquire);
}

bool ContainerTransfer::matches_block_list(
    const config::StringSetting* setting,
    const std::string& blockId) const {
    if (setting == nullptr || blockId.empty()) return false;

    const std::string normalizedBlock = normalize_id(blockId);
    const auto ids = parse_id_list(setting->value());
    return std::find(ids.begin(), ids.end(), normalizedBlock) != ids.end();
}

bool ContainerTransfer::matches_item(const std::string& itemId) const {
    return std::find(m_itemFilter.begin(), m_itemFilter.end(), itemId) != m_itemFilter.end();
}

bool ContainerTransfer::prepare_block_use(
    const std::string& blockId, std::int32_t x,
    std::int32_t y, std::int32_t z, bool firstEvent) {
    std::lock_guard lock(m_mutex);
    if (!firstEvent || !enabled()) return false;

    if (!minecraft::container_ui::available()) {
        LOG_WARN("[ContainerTransfer] Hidden container support is unavailable for this Bedrock build.");
        return false;
    }
    const bool source = matches_block_list(m_sourceBlocks, blockId);
    const bool destination = matches_block_list(m_destinationBlocks, blockId);

    // A second real block click supersedes an earlier click whose container UI
    // has not appeared yet. This prevents a later unrelated container from being
    // mistaken for the first click's screen.
    if (m_phase == Phase::Opening) {
        minecraft::container_ui::cancel_pending_open();
        reset_transfer();
    } else if (m_phase != Phase::Idle || minecraft::container_ui::has_hidden_screen()) {
        return false;
    }
    if (!source && !destination) return false;

    if (source && destination) {
        LOG_WARN("[ContainerTransfer] {} is configured as both source and destination; ignoring click.", blockId);
        return false;
    }

    auto itemIds = m_itemIds != nullptr ? parse_id_list(m_itemIds->value())
                                        : std::vector<std::string>{};
    if (itemIds.empty()) {
        LOG_WARN("[ContainerTransfer] A container was clicked, but no item IDs are configured.");
        return false;
    }

    m_direction = source ? Direction::WithdrawFromContainer : Direction::DepositIntoContainer;
    m_itemFilter = std::move(itemIds);
    m_phase = Phase::Opening;
    m_phaseSince = GetTickCount64();
    m_lastClickAt = 0;
    m_lastTotal = -1;
    m_inPass = false;
    m_moved = false;
    m_reportedNoProgress = false;
    m_passClicked.clear();

    minecraft::container_ui::arm_next_open();
    LOG_INFO("[ContainerTransfer] Matched {} container click: {} at ({}, {}, {}).",
        source ? "source" : "destination", blockId, x, y, z);
    return true;
}

void ContainerTransfer::on_open_attempt_failed() {
    std::lock_guard lock(m_mutex);
    if (m_phase == Phase::Opening) {
        LOG_WARN("[ContainerTransfer] The clicked block did not produce a capturable container screen; no items were moved.");
        reset_transfer();
    }
    minecraft::container_ui::cancel_pending_open();
}

void ContainerTransfer::on_container_opened(void* controller) {
    std::lock_guard lock(m_mutex);
    if (m_phase != Phase::Opening || controller == nullptr) {
        LOG_WARN("[ContainerTransfer] A hidden container screen arrived without a matching click transaction; closing it.");
        m_phase = Phase::Closing;
        m_phaseSince = GetTickCount64();
        return;
    }

    m_phase = Phase::Moving;
    m_phaseSince = GetTickCount64();
    m_lastClickAt = 0;
    m_lastTotal = -1;
    m_inPass = false;
    m_passClicked.clear();

    // Report the real model capacities once per opened container. This makes it
    // possible to distinguish a missing collection from an empty collection.
    const int containerSlots = minecraft::container_ui::collection_size("container_items");
    const int inventorySlots = minecraft::container_ui::collection_size("inventory_items");
    const int hotbarSlots = minecraft::container_ui::collection_size("hotbar_items");
    const int combinedSlots =
        minecraft::container_ui::collection_size("combined_hotbar_and_inventory_items");
    LOG_INFO("[ContainerTransfer] Collections ready: container_items={}, inventory_items={}, hotbar_items={}, combined={}.",
        containerSlots, inventorySlots, hotbarSlots, combinedSlots);
    LOG_INFO("[ContainerTransfer] Hidden container is ready; starting the filtered transfer.");
}

std::vector<std::pair<std::string, int>> ContainerTransfer::player_slots() const {
    using minecraft::container_ui::collection_size;

    std::vector<std::pair<std::string, int>> slots;
    const int inventory = collection_size("inventory_items");
    const int hotbar = collection_size("hotbar_items");

    const auto append = [&slots](const char* collection, int count) {
        for (int index = 0; index < count; ++index) {
            slots.emplace_back(collection, index);
        }
    };

    if (inventory > 0 || hotbar > 0) {
        append("inventory_items", inventory);
        append("hotbar_items", hotbar);
    } else {
        append("combined_hotbar_and_inventory_items",
               collection_size("combined_hotbar_and_inventory_items"));
    }
    return slots;
}

std::vector<std::pair<std::string, int>> ContainerTransfer::collect_candidates(int& total) const {
    using namespace minecraft::container_ui;

    total = 0;
    std::vector<std::pair<std::string, int>> candidates;

    if (m_direction == Direction::WithdrawFromContainer) {
        const int size = collection_size("container_items");
        for (int index = 0; index < size; ++index) {
            const void* stack = stack_at("container_items", index);
            if (is_empty(stack)) continue;

            const std::string item = item_name(stack);
            if (!matches_item(item)) continue;

            total += count_of(stack);
            candidates.emplace_back("container_items", index);
        }
    } else if (m_direction == Direction::DepositIntoContainer) {
        for (const auto& [collection, index] : player_slots()) {
            const void* stack = stack_at(collection, index);
            if (is_empty(stack)) continue;

            const std::string item = item_name(stack);
            if (!matches_item(item)) continue;

            total += count_of(stack);
            candidates.emplace_back(collection, index);
        }
    }

    return candidates;
}

bool ContainerTransfer::destination_has_room(const std::string& itemId) const {
    using namespace minecraft::container_ui;
    const int size = collection_size("container_items");
    for (int index = 0; index < size; ++index) {
        const void* stack = stack_at("container_items", index);
        if (is_empty(stack)) return true;
        if (item_name(stack) == itemId && count_of(stack) < max_stack_of(stack)) return true;
    }
    return false;
}

void ContainerTransfer::begin_closing(const char* reason) {
    if (m_phase == Phase::Closing || m_phase == Phase::Idle) return;
    m_phase = Phase::Closing;
    m_phaseSince = GetTickCount64();
    m_inPass = false;
    m_passClicked.clear();
    if (reason != nullptr && *reason != '\0') {
        LOG_INFO("[ContainerTransfer] {}", reason);
    }
}

void ContainerTransfer::on_container_tick() {
    std::lock_guard lock(m_mutex);
    using namespace minecraft::container_ui;
    const std::uint64_t now = GetTickCount64();

    if (m_phase == Phase::Idle || m_phase == Phase::Opening) return;

    if (m_phase == Phase::Closing) {
        if (!pending_predictions() || now - m_phaseSince >= kCloseWaitMs) {
            release_offstack_screen();
        }
        return;
    }

    if (!enabled()) {
        begin_closing("Module disabled; closing the hidden container after pending moves settle.");
        return;
    }

    if (now - m_phaseSince >= kPhaseGuardMs) {
        LOG_WARN("[ContainerTransfer] Transfer exceeded its safety time limit; stopping.");
        begin_closing("Closing timed-out container.");
        return;
    }

    const int containerSlots = collection_size("container_items");
    if (containerSlots <= 0) {
        if (now - m_phaseSince >= kContentWaitMs) {
            LOG_WARN("[ContainerTransfer] Container contents did not arrive; stopping without moving items.");
            begin_closing("Closing container whose contents were unavailable.");
        }
        return;
    }

    const void* cursor = stack_at("cursor_items", 0);
    if (!is_empty(cursor)) {
        LOG_WARN("[ContainerTransfer] Cursor already holds an item; leaving the container untouched.");
        begin_closing("Closing because the cursor was occupied.");
        return;
    }

    int total = 0;
    const auto candidates = collect_candidates(total);

    if (m_inPass) {
        int clicks = 0;
        for (const auto& candidate : candidates) {
            if (clicks >= kClicksPerTick) break;
            if (std::find(m_passClicked.begin(), m_passClicked.end(), candidate) != m_passClicked.end()) {
                continue;
            }

            if (!shift_click(candidate.first, candidate.second)) {
                LOG_WARN("[ContainerTransfer] A shift-click could not be sent; stopping this transfer.");
                begin_closing("Closing after a failed slot request.");
                return;
            }

            m_passClicked.push_back(candidate);
            ++clicks;
            m_lastClickAt = now;
            m_moved = true;
        }
        if (clicks > 0) return;

        if (m_lastClickAt == 0 || now - m_lastClickAt < kSettleMs) return;
        if (pending_predictions()) {
            if (now - m_lastClickAt >= kConfirmMs) {
                LOG_WARN("[ContainerTransfer] The server did not settle the previous item move in time.");
                begin_closing("Closing after an unconfirmed item move.");
            }
            return;
        }

        m_inPass = false;
        if (candidates.empty()) {
            begin_closing("All matching item stacks have been processed.");
            return;
        }
        if (m_lastTotal >= 0 && total >= m_lastTotal) {
            if (!m_reportedNoProgress) {
                LOG_WARN("[ContainerTransfer] No further items moved; the destination or player inventory may be full.");
                m_reportedNoProgress = true;
            }
            begin_closing("Closing because the transfer made no further progress.");
            return;
        }
    }

    if (candidates.empty()) {
        begin_closing(m_moved ? "Filtered transfer completed." : "No matching items were found.");
        return;
    }

    if (m_direction == Direction::DepositIntoContainer) {
        bool anyRoom = false;
        for (const auto& candidate : candidates) {
            const void* stack = stack_at(candidate.first, candidate.second);
            if (!is_empty(stack) && destination_has_room(item_name(stack))) {
                anyRoom = true;
                break;
            }
        }
        if (!anyRoom) {
            LOG_WARN("[ContainerTransfer] The destination container has no room for the selected items.");
            begin_closing("Closing because the destination is full.");
            return;
        }
    }

    m_lastTotal = total;
    m_passClicked.clear();
    m_inPass = true;

    int clicks = 0;
    for (const auto& candidate : candidates) {
        if (clicks >= kClicksPerTick) break;

        if (!shift_click(candidate.first, candidate.second)) {
            LOG_WARN("[ContainerTransfer] A shift-click could not be sent; stopping this transfer.");
            begin_closing("Closing after a failed slot request.");
            return;
        }

        m_passClicked.push_back(candidate);
        ++clicks;
        m_lastClickAt = now;
        m_moved = true;
    }
}

void ContainerTransfer::on_container_lost() {
    std::lock_guard lock(m_mutex);
    if (m_phase == Phase::Opening) {
        LOG_WARN("[ContainerTransfer] The clicked block did not produce a capturable container screen; no items were moved.");
    } else if (m_phase != Phase::Idle && m_phase != Phase::Closing) {
        LOG_WARN("[ContainerTransfer] The hidden container closed unexpectedly; the transfer was stopped.");
    } else if (m_phase == Phase::Closing && m_moved) {
        LOG_INFO("[ContainerTransfer] Container transfer session ended.");
    }
    reset_transfer();
}

void ContainerTransfer::on_enable() {
    // Install version-specific container hooks only when the user explicitly
    // enables this module. The bridge must not intercept global scene changes
    // during ordinary world loading while ContainerTransfer is inactive.
    if (minecraft::container_ui::available()) {
        return;
    }

    if (!minecraft::container_ui::initialize()) {
        LOG_ERROR("[ContainerTransfer] Could not initialize the hidden-container bridge; disabling the module.");
        set_enabled(false);
    }
}

void ContainerTransfer::on_disable() {
    std::lock_guard lock(m_mutex);
    if (m_phase == Phase::Opening) {
        minecraft::container_ui::cancel_pending_open();
        reset_transfer();
    } else if (m_phase == Phase::Moving) {
        begin_closing("Module disabled; finishing safely.");
    }
}

void ContainerTransfer::reset_transfer() {
    m_direction = Direction::None;
    m_phase = Phase::Idle;
    m_itemFilter.clear();
    m_passClicked.clear();
    m_phaseSince = 0;
    m_lastClickAt = 0;
    m_lastTotal = -1;
    m_inPass = false;
    m_moved = false;
    m_reportedNoProgress = false;
}


namespace {
    struct ContainerOption {
        std::string_view id;
        std::string_view label;
        std::string_view group;
    };

    constexpr ContainerOption kContainerOptions[] = {
        {"minecraft:chest", "Chest", "Storage"},
        {"minecraft:trapped_chest", "Trapped Chest", "Storage"},
        {"minecraft:barrel", "Barrel", "Storage"},
        {"minecraft:ender_chest", "Ender Chest", "Storage"},
        {"minecraft:copper_chest", "Copper Chest", "Copper Chests"},
        {"minecraft:exposed_copper_chest", "Exposed Copper Chest", "Copper Chests"},
        {"minecraft:weathered_copper_chest", "Weathered Copper Chest", "Copper Chests"},
        {"minecraft:oxidized_copper_chest", "Oxidized Copper Chest", "Copper Chests"},
        {"minecraft:waxed_copper_chest", "Waxed Copper Chest", "Copper Chests"},
        {"minecraft:waxed_exposed_copper_chest", "Waxed Exposed Copper Chest", "Copper Chests"},
        {"minecraft:waxed_weathered_copper_chest", "Waxed Weathered Copper Chest", "Copper Chests"},
        {"minecraft:waxed_oxidized_copper_chest", "Waxed Oxidized Copper Chest", "Copper Chests"},
        {"minecraft:hopper", "Hopper", "Redstone"},
        {"minecraft:dispenser", "Dispenser", "Redstone"},
        {"minecraft:dropper", "Dropper", "Redstone"},
        {"minecraft:crafter", "Crafter", "Redstone"},
        {"minecraft:furnace", "Furnace", "Furnaces & Brewing"},
        {"minecraft:lit_furnace", "Lit Furnace", "Furnaces & Brewing"},
        {"minecraft:blast_furnace", "Blast Furnace", "Furnaces & Brewing"},
        {"minecraft:lit_blast_furnace", "Lit Blast Furnace", "Furnaces & Brewing"},
        {"minecraft:smoker", "Smoker", "Furnaces & Brewing"},
        {"minecraft:lit_smoker", "Lit Smoker", "Furnaces & Brewing"},
        {"minecraft:brewing_stand", "Brewing Stand", "Furnaces & Brewing"},
        {"minecraft:undyed_shulker_box", "Undyed Shulker Box", "Shulker Boxes"},
        {"minecraft:white_shulker_box", "White Shulker Box", "Shulker Boxes"},
        {"minecraft:orange_shulker_box", "Orange Shulker Box", "Shulker Boxes"},
        {"minecraft:magenta_shulker_box", "Magenta Shulker Box", "Shulker Boxes"},
        {"minecraft:light_blue_shulker_box", "Light Blue Shulker Box", "Shulker Boxes"},
        {"minecraft:yellow_shulker_box", "Yellow Shulker Box", "Shulker Boxes"},
        {"minecraft:lime_shulker_box", "Lime Shulker Box", "Shulker Boxes"},
        {"minecraft:pink_shulker_box", "Pink Shulker Box", "Shulker Boxes"},
        {"minecraft:gray_shulker_box", "Gray Shulker Box", "Shulker Boxes"},
        {"minecraft:light_gray_shulker_box", "Light Gray Shulker Box", "Shulker Boxes"},
        {"minecraft:cyan_shulker_box", "Cyan Shulker Box", "Shulker Boxes"},
        {"minecraft:purple_shulker_box", "Purple Shulker Box", "Shulker Boxes"},
        {"minecraft:blue_shulker_box", "Blue Shulker Box", "Shulker Boxes"},
        {"minecraft:brown_shulker_box", "Brown Shulker Box", "Shulker Boxes"},
        {"minecraft:green_shulker_box", "Green Shulker Box", "Shulker Boxes"},
        {"minecraft:red_shulker_box", "Red Shulker Box", "Shulker Boxes"},
        {"minecraft:black_shulker_box", "Black Shulker Box", "Shulker Boxes"}
    };

    bool contains_id(const std::vector<std::string>& values, std::string_view id) {
        return std::find_if(values.begin(), values.end(),
            [id](const std::string& value) { return value == id; }) != values.end();
    }

    void remove_id(std::vector<std::string>& values, std::string_view id) {
        values.erase(std::remove_if(values.begin(), values.end(),
            [id](const std::string& value) { return value == id; }), values.end());
    }

    std::string join_ids(const std::vector<std::string>& values) {
        std::string result;
        for (const auto& value : values) {
            if (!result.empty()) result += ',';
            result += value;
        }
        return result;
    }

    bool is_known_container(std::string_view id) {
        return std::any_of(std::begin(kContainerOptions), std::end(kContainerOptions),
            [id](const ContainerOption& option) { return option.id == id; });
    }

    bool valid_resource_id(std::string_view id) {
        const std::size_t colon = id.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            colon + 1 >= id.size() || id.find(':', colon + 1) != std::string_view::npos) {
            return false;
        }
        for (const unsigned char ch : id) {
            if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' ||
                  ch == '/' || ch == ':')) {
                return false;
            }
        }
        return true;
    }

    std::string lower_copy(std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    bool persist_default_profile() {
        return ConfigManager::instance().save_config("default");
    }

    std::vector<const ContainerOption*> container_suggestions(std::string_view query, std::size_t limit) {
        const std::string needle = lower_copy(query);
        std::vector<const ContainerOption*> matches;
        if (needle.empty()) return matches;

        for (const auto& option : kContainerOptions) {
            const std::string id = lower_copy(option.id);
            const std::string label = lower_copy(option.label);
            const std::string shortId = id.starts_with("minecraft:") ? id.substr(10) : id;
            if (id.find(needle) != std::string::npos ||
                shortId.find(needle) != std::string::npos ||
                label.find(needle) != std::string::npos) {
                matches.push_back(&option);
                if (limit != 0 && matches.size() >= limit) break;
            }
        }
        return matches;
    }

    std::string container_label(std::string_view id) {
        const auto found = std::find_if(std::begin(kContainerOptions), std::end(kContainerOptions),
            [id](const ContainerOption& option) { return option.id == id; });
        return found != std::end(kContainerOptions) ? std::string(found->label) : std::string(id);
    }

    bool add_container_id(config::StringSetting* setting,
                          config::StringSetting* oppositeSetting,
                          const char* input) {
        if (setting == nullptr || input == nullptr || *input == '\0') return false;

        const std::string id = normalize_id(input);
        if (!valid_resource_id(id)) return false;

        auto selected = parse_id_list(setting->value());
        if (contains_id(selected, id)) return false;
        selected.push_back(id);

        if (oppositeSetting != nullptr) {
            auto opposite = parse_id_list(oppositeSetting->value());
            const auto before = opposite.size();
            remove_id(opposite, id);
            if (opposite.size() != before) {
                oppositeSetting->set_value(join_ids(opposite));
            }
        }

        setting->set_value(join_ids(selected));
        persist_default_profile();
        return true;
    }

    void remove_container_id(config::StringSetting* setting, std::string_view id) {
        if (setting == nullptr) return;
        auto values = parse_id_list(setting->value());
        const auto before = values.size();
        remove_id(values, id);
        if (values.size() != before) {
            setting->set_value(join_ids(values));
            persist_default_profile();
        }
    }

    void render_container_selector(const char* title,
                                   const char* description,
                                   const char* widgetId,
                                   config::StringSetting* setting,
                                   config::StringSetting* oppositeSetting) {
        if (setting == nullptr) return;

        static char sourceSearch[160]{};
        static char destinationSearch[160]{};
        char* searchBuffer = std::string_view(widgetId) == "source"
            ? sourceSearch : destinationSearch;
        const auto initialMatches = container_suggestions(searchBuffer, 1);
        const float cardHeight = (searchBuffer[0] != '\0' && !initialMatches.empty())
            ? 292.0f : 218.0f;

        ImGui::PushID(widgetId);
        ImGui::BeginChild("##container_card", ImVec2(0.0f, cardHeight), true);
        ImGui::TextColored(ImVec4(0.18f, 0.40f, 0.66f, 1.0f), "%s", title);
        ImGui::TextWrapped("%s", description);
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-92.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##container_search", "Search by name or block ID...",
            searchBuffer, sizeof(sourceSearch), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Add ID", ImVec2(82.0f, 0.0f)) || submitted) &&
            add_container_id(setting, oppositeSetting, searchBuffer)) {
            searchBuffer[0] = '\0';
        }

        const auto matches = container_suggestions(searchBuffer, 3);
        if (searchBuffer[0] != '\0' && !matches.empty()) {
            ImGui::TextDisabled("Matching container types");
            ImGui::BeginChild("##container_suggestions", ImVec2(0.0f, 82.0f), true);
            for (const auto* option : matches) {
                ImGui::PushID(option->id.data());
                const std::string label(option->label);
                if (ImGui::Selectable(label.c_str(), false)) {
                    if (add_container_id(setting, oppositeSetting, option->id.data())) {
                        searchBuffer[0] = '\0';
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", option->id.data());
                ImGui::PopID();
            }
            ImGui::EndChild();
        } else if (searchBuffer[0] != '\0') {
            ImGui::TextDisabled("No built-in match. Use Add ID for a custom namespace ID.");
        } else {
            ImGui::TextDisabled("Type a name or ID to see suggestions.");
        }

        ImGui::Spacing();
        const auto selected = parse_id_list(setting->value());
        ImGui::TextDisabled("Selected containers (%zu)", selected.size());
        ImGui::BeginChild("##selected_containers", ImVec2(0.0f, 58.0f), true);
        if (selected.empty()) {
            ImGui::TextDisabled("No container types selected yet.");
        } else {
            for (const auto& id : selected) {
                ImGui::PushID(id.c_str());
                const std::string label = container_label(id);
                ImGui::Text("%s", label.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) remove_container_id(setting, id);
                ImGui::TextDisabled("%s", id.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::PopID();
    }

    std::vector<std::string_view> item_suggestions(std::string_view query, std::size_t limit) {
        std::string needle(query);
        std::transform(needle.begin(), needle.end(), needle.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        needle.erase(needle.begin(), std::find_if(needle.begin(), needle.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        while (!needle.empty() && std::isspace(static_cast<unsigned char>(needle.back()))) {
            needle.pop_back();
        }

        std::vector<std::string_view> matches;
        if (needle.empty()) return matches;

        const bool qualified = needle.find(':') != std::string::npos;
        for (const auto id : container_transfer_data::kVanillaItemIds) {
            const std::string_view candidate = qualified ? id : id.substr(std::string_view("minecraft:").size());
            if (candidate.find(needle) != std::string::npos) {
                matches.push_back(id);
                if (limit != 0 && matches.size() >= limit) break;
            }
        }
        return matches;
    }

    struct ItemCompletionCycle {
        std::string baseQuery;
        std::string lastCompletion;
        std::size_t nextIndex = 0;
    };

    int complete_item_on_tab(ImGuiInputTextCallbackData* data) {
        auto* cycle = static_cast<ItemCompletionCycle*>(data->UserData);
        if (cycle == nullptr) return 0;

        const std::string current(data->Buf, static_cast<std::size_t>(data->BufTextLen));
        if (current != cycle->lastCompletion) {
            cycle->baseQuery = current;
            cycle->lastCompletion.clear();
            cycle->nextIndex = 0;
        }

        const auto matches = item_suggestions(cycle->baseQuery, 0);
        if (matches.empty()) return 0;

        const std::string completed(matches[cycle->nextIndex % matches.size()]);
        ++cycle->nextIndex;
        cycle->lastCompletion = completed;
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, completed.c_str());
        return 0;
    }

    void set_item_search(char* buffer, std::size_t capacity, const std::string& value,
                         ItemCompletionCycle& cycle) {
        strncpy_s(buffer, capacity, value.c_str(), _TRUNCATE);
        cycle = ItemCompletionCycle{};
    }

    bool add_item_from_search(config::StringSetting* setting, const char* input) {
        if (setting == nullptr || input == nullptr || *input == '\0') return false;

        std::string id = normalize_id(input);
        const std::string typed = lower_copy(input);
        for (const auto candidate : container_transfer_data::kVanillaItemIds) {
            const std::string full = lower_copy(candidate);
            const std::string shortId = full.starts_with("minecraft:")
                ? full.substr(10) : full;
            if (typed == full || typed == shortId) {
                id.assign(candidate);
                break;
            }
        }
        if (!valid_resource_id(id)) return false;

        auto selected = parse_id_list(setting->value());
        if (contains_id(selected, id)) return false;
        selected.push_back(std::move(id));
        setting->set_value(join_ids(selected));
        persist_default_profile();
        return true;
    }

    void remove_item_id(config::StringSetting* setting, std::string_view id) {
        if (setting == nullptr) return;
        auto values = parse_id_list(setting->value());
        const auto before = values.size();
        remove_id(values, id);
        if (values.size() != before) {
            setting->set_value(join_ids(values));
            persist_default_profile();
        }
    }

    void render_item_picker(config::StringSetting* setting) {
        if (setting == nullptr) return;

        static char searchBuffer[160]{};
        static ItemCompletionCycle completionCycle{};
        const auto initialMatches = item_suggestions(searchBuffer, 1);
        const float cardHeight = (searchBuffer[0] != '\0' && !initialMatches.empty())
            ? 286.0f : 218.0f;

        ImGui::BeginChild("##item_filter_card", ImVec2(0.0f, cardHeight), true);
        ImGui::TextColored(ImVec4(0.18f, 0.40f, 0.66f, 1.0f), "ITEM FILTER");
        ImGui::TextWrapped("Only selected item IDs are transferred.");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-92.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##item_id_search", "Search item ID... (Tab completes)",
            searchBuffer, sizeof(searchBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion,
            complete_item_on_tab, &completionCycle);
        ImGui::SameLine();
        if ((ImGui::Button("Add ID", ImVec2(82.0f, 0.0f)) || submitted) &&
            add_item_from_search(setting, searchBuffer)) {
            set_item_search(searchBuffer, sizeof(searchBuffer), "", completionCycle);
        }

        const auto suggestions = item_suggestions(searchBuffer, 3);
        if (searchBuffer[0] != '\0' && !suggestions.empty()) {
            ImGui::TextDisabled("Matching items");
            ImGui::BeginChild("##item_suggestions", ImVec2(0.0f, 78.0f), true);
            for (const auto suggestion : suggestions) {
                const std::string text(suggestion);
                ImGui::PushID(text.c_str());
                if (ImGui::Selectable(text.c_str(), false)) {
                    if (add_item_from_search(setting, text.c_str())) {
                        set_item_search(searchBuffer, sizeof(searchBuffer), "", completionCycle);
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        } else if (searchBuffer[0] != '\0') {
            ImGui::TextDisabled("No built-in match. Custom namespace IDs are supported.");
        } else {
            ImGui::TextDisabled("Enter part of an item ID to see suggestions.");
        }

        const auto selected = parse_id_list(setting->value());
        ImGui::Spacing();
        ImGui::TextDisabled("Selected items (%zu)", selected.size());
        ImGui::BeginChild("##selected_item_ids", ImVec2(0.0f, 58.0f), true);
        if (selected.empty()) {
            ImGui::TextDisabled("No item IDs selected.");
        } else {
            for (const auto& id : selected) {
                ImGui::PushID(id.c_str());
                ImGui::Text("%s", id.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) remove_item_id(setting, id);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void push_container_transfer_style() {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.15f, 0.18f, 0.23f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.43f, 0.48f, 0.55f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.86f, 0.89f, 0.93f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.97f, 0.98f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.93f, 0.96f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.89f, 0.94f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.91f, 0.95f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.92f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.88f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.89f, 0.94f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.84f, 0.91f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.77f, 0.87f, 0.97f, 1.0f));
    }

    void pop_container_transfer_style() {
        ImGui::PopStyleColor(13);
        ImGui::PopStyleVar(5);
    }

}

void ContainerTransfer::on_settings_render() {
    push_container_transfer_style();
    ImGui::BeginChild("##container_transfer_settings", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextColored(ImVec4(0.18f, 0.40f, 0.66f, 1.0f), "CONTAINER TRANSFER");
    ImGui::TextWrapped("Set the source, destination, and item IDs. Click a configured container in the world to run the transfer without opening its screen.");
    ImGui::Spacing();

    render_container_selector(
        "SOURCE CONTAINERS",
        "Withdraw matching items into your inventory.",
        "source",
        m_sourceBlocks,
        m_destinationBlocks);
    ImGui::Spacing();
    render_container_selector(
        "DESTINATION CONTAINERS",
        "Deposit matching items into this container.",
        "destination",
        m_destinationBlocks,
        m_sourceBlocks);
    ImGui::Spacing();
    render_item_picker(m_itemIds);
    ImGui::Spacing();
    ImGui::TextDisabled("A container type cannot be selected as both source and destination.");
    ImGui::TextDisabled("Settings are saved automatically to the default profile.");
    ImGui::EndChild();
    pop_container_transfer_style();
}
