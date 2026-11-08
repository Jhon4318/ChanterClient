#include "modules/tools/ContainerTransfer.h"

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
    if (!enabled() || !firstEvent) return false;

    if (!minecraft::container_ui::available()) {
        LOG_WARN("[ContainerTransfer] Hidden container support is unavailable for this Bedrock build.");
        return false;
    }
    if (m_phase != Phase::Idle || minecraft::container_ui::has_hidden_screen()) {
        return false;
    }

    const bool source = matches_block_list(m_sourceBlocks, blockId);
    const bool destination = matches_block_list(m_destinationBlocks, blockId);
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
    if (m_phase != Phase::Idle && m_phase != Phase::Closing) {
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
        {"minecraft:chest", "チェスト", "チェスト・収納箱"},
        {"minecraft:trapped_chest", "トラップチェスト", "チェスト・収納箱"},
        {"minecraft:barrel", "樽", "チェスト・収納箱"},
        {"minecraft:ender_chest", "エンダーチェスト", "チェスト・収納箱"},
        {"minecraft:copper_chest", "銅のチェスト", "銅のチェスト"},
        {"minecraft:exposed_copper_chest", "風化した銅のチェスト", "銅のチェスト"},
        {"minecraft:weathered_copper_chest", "錆びた銅のチェスト", "銅のチェスト"},
        {"minecraft:oxidized_copper_chest", "酸化した銅のチェスト", "銅のチェスト"},
        {"minecraft:waxed_copper_chest", "錆止めされた銅のチェスト", "銅のチェスト"},
        {"minecraft:waxed_exposed_copper_chest", "錆止めされた風化銅のチェスト", "銅のチェスト"},
        {"minecraft:waxed_weathered_copper_chest", "錆止めされた錆びた銅のチェスト", "銅のチェスト"},
        {"minecraft:waxed_oxidized_copper_chest", "錆止めされた酸化銅のチェスト", "銅のチェスト"},
        {"minecraft:hopper", "ホッパー", "レッドストーン系"},
        {"minecraft:dispenser", "ディスペンサー", "レッドストーン系"},
        {"minecraft:dropper", "ドロッパー", "レッドストーン系"},
        {"minecraft:crafter", "自動作業台", "レッドストーン系"},
        {"minecraft:furnace", "かまど", "かまど・醸造"},
        {"minecraft:lit_furnace", "燃焼中のかまど", "かまど・醸造"},
        {"minecraft:blast_furnace", "溶鉱炉", "かまど・醸造"},
        {"minecraft:lit_blast_furnace", "燃焼中の溶鉱炉", "かまど・醸造"},
        {"minecraft:smoker", "燻製器", "かまど・醸造"},
        {"minecraft:lit_smoker", "燃焼中の燻製器", "かまど・醸造"},
        {"minecraft:brewing_stand", "醸造台", "かまど・醸造"},
        {"minecraft:undyed_shulker_box", "無色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:white_shulker_box", "白色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:orange_shulker_box", "橙色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:magenta_shulker_box", "赤紫色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:light_blue_shulker_box", "空色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:yellow_shulker_box", "黄色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:lime_shulker_box", "黄緑色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:pink_shulker_box", "桃色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:gray_shulker_box", "灰色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:light_gray_shulker_box", "薄灰色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:cyan_shulker_box", "青緑色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:purple_shulker_box", "紫色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:blue_shulker_box", "青色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:brown_shulker_box", "茶色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:green_shulker_box", "緑色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:red_shulker_box", "赤色のシュルカーボックス", "シュルカーボックス"},
        {"minecraft:black_shulker_box", "黒色のシュルカーボックス", "シュルカーボックス"}
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

    void render_container_selector(
        const char* label,
        const char* widgetId,
        config::StringSetting* setting,
        config::StringSetting* oppositeSetting)
    {
        if (setting == nullptr) return;

        const auto selected = parse_id_list(setting->value());
        const std::string preview = selected.empty()
            ? "コンテナを選択..."
            : std::to_string(selected.size()) + " 種類を選択中";

        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::PushID(widgetId);
        if (ImGui::BeginCombo("##container_types", preview.c_str())) {
            ImGui::TextDisabled("チェックを付けた種類をすべて対象にします。");
            std::string lastGroup;

            for (const auto& option : kContainerOptions) {
                if (lastGroup != option.group) {
                    if (!lastGroup.empty()) ImGui::Separator();
                    ImGui::TextDisabled("%s", option.group.data());
                    lastGroup.assign(option.group);
                }

                bool checked = contains_id(selected, option.id);
                ImGui::PushID(option.id.data());
                if (ImGui::Checkbox(option.label.data(), &checked)) {
                    auto values = parse_id_list(setting->value());
                    if (checked) {
                        if (!contains_id(values, option.id)) values.emplace_back(option.id);
                        // One block type must not be both a source and a destination.
                        if (oppositeSetting != nullptr) {
                            auto opposite = parse_id_list(oppositeSetting->value());
                            remove_id(opposite, option.id);
                            oppositeSetting->set_value(join_ids(opposite));
                        }
                    } else {
                        remove_id(values, option.id);
                    }
                    setting->set_value(join_ids(values));
                }
                ImGui::PopID();
            }

            const auto current = parse_id_list(setting->value());
            bool hasCustom = false;
            for (const auto& id : current) {
                if (!is_known_container(id)) {
                    if (!hasCustom) {
                        ImGui::Separator();
                        ImGui::TextDisabled("保存済みのカスタムID");
                        hasCustom = true;
                    }
                    bool checked = true;
                    ImGui::PushID(id.c_str());
                    if (ImGui::Checkbox(id.c_str(), &checked) && !checked) {
                        auto values = parse_id_list(setting->value());
                        remove_id(values, id);
                        setting->set_value(join_ids(values));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
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
            if (candidate.starts_with(needle)) {
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

        const auto matches = item_suggestions(input, 1);
        std::string id = normalize_id(input);
        if (!matches.empty() && matches.front() != id) {
            id.assign(matches.front());
        }
        if (!valid_resource_id(id)) return false;

        auto selected = parse_id_list(setting->value());
        if (!contains_id(selected, id)) selected.push_back(std::move(id));
        setting->set_value(join_ids(selected));
        return true;
    }

    void render_item_picker(config::StringSetting* setting) {
        if (setting == nullptr) return;

        static char searchBuffer[160]{};
        static ItemCompletionCycle completionCycle{};

        ImGui::TextUnformatted("移動するアイテム");
        ImGui::SetNextItemWidth(-1.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##item_id_search",
            "例: iron_ingot / minecraft:diamond ... (Tabで補完)",
            searchBuffer,
            sizeof(searchBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion,
            complete_item_on_tab,
            &completionCycle);

        if (submitted && add_item_from_search(setting, searchBuffer)) {
            set_item_search(searchBuffer, sizeof(searchBuffer), "", completionCycle);
        }

        const auto suggestions = item_suggestions(searchBuffer, 9);
        if (searchBuffer[0] != '\0' && !suggestions.empty()) {
            const auto allMatches = item_suggestions(searchBuffer, 0);
            ImGui::TextDisabled("%zu 件の候補があります。Tabで順番に補完できます。", allMatches.size());
            if (ImGui::BeginChild("##item_suggestions", ImVec2(0, 126), true)) {
                for (const auto suggestion : suggestions) {
                    const std::string text(suggestion);
                    if (ImGui::Selectable(text.c_str(), false)) {
                        if (add_item_from_search(setting, text.c_str())) {
                            set_item_search(searchBuffer, sizeof(searchBuffer), "", completionCycle);
                        }
                    }
                }
            }
            ImGui::EndChild();
        } else if (searchBuffer[0] != '\0') {
            ImGui::TextDisabled("候補がありません。アドオンのIDも手入力で追加できます。");
        } else {
            ImGui::TextDisabled("IDの一部を入力すると候補が表示されます。");
        }

        if (ImGui::Button("入力したIDを追加", ImVec2(150.0f, 30.0f))) {
            if (add_item_from_search(setting, searchBuffer)) {
                set_item_search(searchBuffer, sizeof(searchBuffer), "", completionCycle);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("すべて解除", ImVec2(105.0f, 30.0f))) {
            setting->set_value("");
        }

        const auto selected = parse_id_list(setting->value());
        ImGui::Spacing();
        ImGui::Text("登録済みアイテム (%zu)", selected.size());
        if (ImGui::BeginChild("##selected_item_ids", ImVec2(0, 105), true)) {
            if (selected.empty()) {
                ImGui::TextDisabled("アイテムが登録されていません。");
            } else {
                for (const auto& id : selected) {
                    ImGui::PushID(id.c_str());
                    ImGui::TextUnformatted(id.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("削除")) {
                        auto values = parse_id_list(setting->value());
                        remove_id(values, id);
                        setting->set_value(join_ids(values));
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
    }
}

void ContainerTransfer::on_settings_render() {
    ImGui::TextWrapped("指定したコンテナを自分でクリックすると、選択アイテムを一括で回収または収納します。コンテナ画面は表示しません。");
    ImGui::Spacing();
    ImGui::Separator();

    render_container_selector("取り出し元（コンテナ → インベントリ）", "source",
                              m_sourceBlocks, m_destinationBlocks);
    ImGui::Spacing();
    render_container_selector("収納先（インベントリ → コンテナ）", "destination",
                              m_destinationBlocks, m_sourceBlocks);

    ImGui::Spacing();
    ImGui::Separator();
    render_item_picker(m_itemIds);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("同じ種類のコンテナを選び直すと、もう一方の欄から自動的に外れます。");
    ImGui::TextDisabled("設定後にContainerTransferを有効化し、対象コンテナを自分でクリックしてください。");
}
