#include "modules/tools/ContainerTransfer.h"
#include "config/config_manager.h"

#include "minecraft/sdk/ui/container_ui.h"
#include "modules/tools/vanilla_item_ids.h"
#include "utils/logger.h"
#include "ui/notifications.h"

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
        "Move selected items between containers.") {
    m_sourceBlocks = add_setting<config::StringSetting>(
        "Source containers",
        "Container IDs.",
        "");
    m_destinationBlocks = add_setting<config::StringSetting>(
        "Destination containers",
        "Container IDs, comma-separated.",
        "");
    m_itemIds = add_setting<config::StringSetting>(
        "Item filter",
        "Item IDs.",
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
        ui::notifications::warning("ContainerTransfer", "This container is selected as both source and destination.");
        return false;
    }

    auto itemIds = m_itemIds != nullptr ? parse_id_list(m_itemIds->value())
                                        : std::vector<std::string>{};
    if (itemIds.empty()) {
        LOG_WARN("[ContainerTransfer] A container was clicked, but no item IDs are configured.");
        ui::notifications::warning("ContainerTransfer", "Choose at least one item type in the Item Filter first.");
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
        ui::notifications::warning("ContainerTransfer", "The clicked block did not open a supported container. No items were moved.");
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
    const std::string_view message = reason != nullptr ? reason : "";
    if (!message.empty()) LOG_INFO("[ContainerTransfer] {}", message);

    if (message.find("completed") != std::string_view::npos ||
        message.find("processed") != std::string_view::npos) {
        ui::notifications::success("ContainerTransfer", "Filtered item transfer completed.");
    } else if (message.find("No matching items") != std::string_view::npos) {
        ui::notifications::info("ContainerTransfer", "No matching items were found in this container or inventory.");
    } else if (m_moved) {
        ui::notifications::warning("ContainerTransfer", "Transfer stopped after sending item moves. Check the container and inventory contents.");
    } else {
        ui::notifications::warning("ContainerTransfer", "Transfer stopped before any items were moved. Check the settings and game log.");
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
        ui::notifications::warning("ContainerTransfer", "The container did not open. No items were moved.");
    } else if (m_phase != Phase::Idle && m_phase != Phase::Closing) {
        LOG_WARN("[ContainerTransfer] The hidden container closed unexpectedly; the transfer was stopped.");
        ui::notifications::warning("ContainerTransfer", "The container closed unexpectedly. Transfer was stopped.");
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


    void draw_container_icon(std::string_view id, const ImVec2& pos, float size) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (draw == nullptr) return;

        const float scale = size / 20.0f;
        const auto px = [scale, &pos](float x, float y) {
            return ImVec2(pos.x + x * scale, pos.y + y * scale);
        };
        const auto rect = [&](float x1, float y1, float x2, float y2, ImU32 color, float rounding = 0.0f) {
            draw->AddRectFilled(px(x1, y1), px(x2, y2), color, rounding * scale);
        };
        const ImU32 outline = IM_COL32(48, 54, 64, 255);
        const bool shulker = id.find("shulker_box") != std::string_view::npos;
        const bool copper = id.find("copper_chest") != std::string_view::npos;
        const bool barrel = id.find("barrel") != std::string_view::npos;
        const bool hopper = id.find("hopper") != std::string_view::npos;
        const bool furnace = id.find("furnace") != std::string_view::npos ||
            id.find("smoker") != std::string_view::npos;
        const bool brewing = id.find("brewing_stand") != std::string_view::npos;
        const bool machine = id.find("dispenser") != std::string_view::npos ||
            id.find("dropper") != std::string_view::npos ||
            id.find("crafter") != std::string_view::npos;

        ImU32 body = IM_COL32(151, 91, 48, 255);
        ImU32 lid = IM_COL32(190, 128, 72, 255);
        if (copper) {
            body = IM_COL32(174, 103, 64, 255);
            lid = IM_COL32(207, 140, 91, 255);
            if (id.find("oxidized") != std::string_view::npos) {
                body = IM_COL32(71, 143, 127, 255);
                lid = IM_COL32(113, 184, 159, 255);
            } else if (id.find("weathered") != std::string_view::npos) {
                body = IM_COL32(99, 151, 123, 255);
                lid = IM_COL32(139, 186, 148, 255);
            } else if (id.find("exposed") != std::string_view::npos) {
                body = IM_COL32(135, 148, 99, 255);
                lid = IM_COL32(178, 182, 119, 255);
            }
        }

        if (shulker) {
            body = IM_COL32(143, 88, 171, 255);
            const auto colorFor = [&](std::string_view color) {
                if (id.find(color) == std::string_view::npos) return;
                if (color == "white") body = IM_COL32(225, 225, 222, 255);
                else if (color == "orange") body = IM_COL32(226, 119, 42, 255);
                else if (color == "magenta") body = IM_COL32(173, 62, 169, 255);
                else if (color == "light_blue") body = IM_COL32(81, 154, 207, 255);
                else if (color == "yellow") body = IM_COL32(220, 190, 54, 255);
                else if (color == "lime") body = IM_COL32(111, 174, 48, 255);
                else if (color == "pink") body = IM_COL32(212, 126, 151, 255);
                else if (color == "gray") body = IM_COL32(83, 91, 100, 255);
                else if (color == "light_gray") body = IM_COL32(155, 158, 153, 255);
                else if (color == "cyan") body = IM_COL32(51, 145, 145, 255);
                else if (color == "purple") body = IM_COL32(119, 65, 170, 255);
                else if (color == "blue") body = IM_COL32(57, 83, 168, 255);
                else if (color == "brown") body = IM_COL32(112, 77, 52, 255);
                else if (color == "green") body = IM_COL32(83, 126, 55, 255);
                else if (color == "red") body = IM_COL32(166, 57, 53, 255);
                else if (color == "black") body = IM_COL32(49, 49, 56, 255);
            };
            colorFor("white"); colorFor("orange"); colorFor("magenta"); colorFor("light_blue");
            colorFor("yellow"); colorFor("lime"); colorFor("pink"); colorFor("light_gray");
            colorFor("gray"); colorFor("cyan"); colorFor("purple"); colorFor("blue");
            colorFor("brown"); colorFor("green"); colorFor("red"); colorFor("black");
            rect(3, 3, 17, 17, outline, 3);
            rect(4, 4, 16, 16, body, 2);
            rect(4, 4, 16, 7, IM_COL32(255, 255, 255, 48), 2);
            rect(6, 9, 14, 11, IM_COL32(35, 39, 45, 190), 1);
            rect(8, 12, 12, 15, IM_COL32(255, 255, 255, 75), 1);
            return;
        }

        if (barrel) {
            rect(3, 2, 17, 18, outline, 4);
            rect(4, 3, 16, 17, IM_COL32(143, 91, 48, 255), 3);
            rect(4, 5, 16, 7, IM_COL32(77, 83, 91, 255), 1);
            rect(4, 13, 16, 15, IM_COL32(77, 83, 91, 255), 1);
            rect(7, 8, 13, 12, IM_COL32(192, 137, 78, 255), 1);
            return;
        }

        if (hopper) {
            const ImVec2 hopperPoints[5]{
                px(2, 4), px(18, 4), px(14, 12), px(12, 18), px(8, 18)
            };
            draw->AddConvexPolyFilled(hopperPoints, 5, IM_COL32(93, 101, 112, 255));
            draw->AddPolyline(hopperPoints, 5, outline, ImDrawFlags_Closed, 1.0f * scale);
            rect(8, 8, 12, 10, IM_COL32(180, 186, 194, 255), 1);
            return;
        }

        if (brewing) {
            rect(3, 15, 17, 18, outline, 1);
            rect(4, 15, 16, 17, IM_COL32(161, 111, 61, 255), 1);
            rect(8, 4, 12, 15, IM_COL32(104, 111, 124, 255), 1);
            rect(6, 3, 14, 5, IM_COL32(181, 191, 205, 255), 1);
            rect(6, 9, 14, 11, IM_COL32(72, 164, 193, 255), 1);
            return;
        }

        if (furnace || machine) {
            rect(2, 2, 18, 18, outline, 2);
            rect(3, 3, 17, 17, IM_COL32(127, 133, 143, 255), 1);
            rect(6, 6, 14, 14, IM_COL32(52, 58, 67, 255), 1);
            if (furnace) {
                rect(8, 8, 12, 13, IM_COL32(232, 126, 44, 255), 1);
                rect(9, 7, 11, 10, IM_COL32(255, 202, 78, 255), 1);
            } else if (id.find("crafter") != std::string_view::npos) {
                rect(5, 5, 7, 7, IM_COL32(220, 220, 224, 255), 0);
                rect(13, 5, 15, 7, IM_COL32(220, 220, 224, 255), 0);
                rect(5, 13, 7, 15, IM_COL32(220, 220, 224, 255), 0);
                rect(13, 13, 15, 15, IM_COL32(220, 220, 224, 255), 0);
            } else {
                rect(8, 8, 12, 12, IM_COL32(177, 185, 194, 255), 1);
            }
            return;
        }

        // Chest-family icons, including normal, trapped, ender and copper chests.
        if (id.find("ender_chest") != std::string_view::npos) {
            body = IM_COL32(55, 92, 88, 255);
            lid = IM_COL32(78, 133, 124, 255);
        } else if (id.find("trapped_chest") != std::string_view::npos) {
            body = IM_COL32(143, 82, 49, 255);
            lid = IM_COL32(188, 104, 65, 255);
        }
        rect(2, 5, 18, 17, outline, 2);
        rect(3, 6, 17, 10, lid, 1);
        rect(3, 10, 17, 16, body, 1);
        rect(9, 10, 11, 15, IM_COL32(65, 57, 49, 255), 0);
        rect(8, 11, 12, 13, copper ? IM_COL32(232, 207, 133, 255) :
            (id.find("ender_chest") != std::string_view::npos ? IM_COL32(105, 226, 183, 255) :
                IM_COL32(225, 186, 94, 255)), 1);
        if (id.find("trapped_chest") != std::string_view::npos) {
            rect(8, 4, 12, 6, IM_COL32(190, 56, 49, 255), 1);
        }
    }

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

    bool persist_active_profile() {
        const bool saved = ConfigManager::instance().save_active_config();
        if (!saved) LOG_WARN("[ContainerTransfer] Could not persist the current profile.");
        return saved;
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
        const std::string typed = lower_copy(input);
        std::string id;

        for (const auto& option : kContainerOptions) {
            const std::string full = lower_copy(option.id);
            const std::string shortId = full.starts_with("minecraft:") ? full.substr(10) : full;
            if (typed == full || typed == shortId || typed == lower_copy(option.label)) {
                id.assign(option.id);
                break;
            }
        }
        // Custom/add-on containers must be entered as a full namespace:id.
        // Partial names are never silently converted to the first suggestion.
        if (id.empty()) {
            if (typed.find(':') == std::string::npos) return false;
            id = normalize_id(input);
        }
        if (!valid_resource_id(id)) return false;

        auto selected = parse_id_list(setting->value());
        if (contains_id(selected, id)) return false;
        selected.push_back(id);

        if (oppositeSetting != nullptr) {
            auto opposite = parse_id_list(oppositeSetting->value());
            remove_id(opposite, id);
            oppositeSetting->set_value(join_ids(opposite));
        }
        setting->set_value(join_ids(selected));
        persist_active_profile();
        return true;
    }

    void remove_container_id(config::StringSetting* setting, std::string_view id) {
        if (setting == nullptr) return;
        auto values = parse_id_list(setting->value());
        const auto before = values.size();
        remove_id(values, id);
        if (values.size() != before) {
            setting->set_value(join_ids(values));
            persist_active_profile();
        }
    }

    std::vector<const ContainerOption*> container_suggestions(std::string_view query, std::size_t limit) {
        const std::string needle = lower_copy(query);
        std::vector<const ContainerOption*> matches;
        if (needle.empty()) return matches;

        for (const auto& option : kContainerOptions) {
            const std::string id = lower_copy(option.id);
            const std::string shortId = id.starts_with("minecraft:") ? id.substr(10) : id;
            const std::string label = lower_copy(option.label);
            if (label.find(needle) != std::string::npos ||
                shortId.find(needle) != std::string::npos ||
                id.find(needle) != std::string::npos) {
                matches.push_back(&option);
                if (limit != 0 && matches.size() >= limit) break;
            }
        }
        return matches;
    }

    void render_container_selector(const char* title, const char* widgetId,
                                   config::StringSetting* setting,
                                   config::StringSetting* oppositeSetting) {
        if (setting == nullptr) return;
        static char sourceSearch[160]{};
        static char destinationSearch[160]{};
        static std::string sourceFeedback;
        static std::string destinationFeedback;
        char* searchBuffer = std::string_view(widgetId) == "source" ? sourceSearch : destinationSearch;
        std::string& feedback = std::string_view(widgetId) == "source" ? sourceFeedback : destinationFeedback;
        ImGui::PushID(widgetId);

        const auto toggle_id = [&](std::string_view id, bool checked) {
            auto values = parse_id_list(setting->value());
            if (checked) {
                if (!contains_id(values, id)) values.emplace_back(id);
                if (oppositeSetting != nullptr) {
                    auto opposite = parse_id_list(oppositeSetting->value());
                    remove_id(opposite, id);
                    oppositeSetting->set_value(join_ids(opposite));
                }
            } else {
                remove_id(values, id);
            }
            setting->set_value(join_ids(values));
            persist_active_profile();
        };

        const auto selected = parse_id_list(setting->value());
        const std::string preview = selected.empty() ? "Select container types..." :
            std::to_string(selected.size()) + " selected";
        ImGui::TextDisabled("%s", title);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##container_types", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            std::string lastGroup;
            for (const auto& option : kContainerOptions) {
                if (lastGroup != option.group) {
                    if (!lastGroup.empty()) ImGui::Separator();
                    ImGui::TextDisabled("%s", option.group.data());
                    lastGroup.assign(option.group);
                }

                bool checked = contains_id(selected, option.id);
                ImGui::PushID(option.id.data());
                const float rowHeight = ImGui::GetFrameHeight();
                const bool clicked = ImGui::Selectable("##container_option", checked,
                    0, ImVec2(0.0f, rowHeight));
                const ImVec2 rowMin = ImGui::GetItemRectMin();
                const ImVec2 rowMax = ImGui::GetItemRectMax();
                if (clicked) {
                    checked = !checked;
                    toggle_id(option.id, checked);
                }
                draw_container_icon(option.id,
                    ImVec2(rowMin.x + 9.0f, rowMin.y + (rowHeight - 19.0f) * 0.5f), 19.0f);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddText(ImVec2(rowMin.x + 36.0f, rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), option.label.data());
                if (checked) {
                    const ImVec2 p(rowMax.x - 18.0f, rowMin.y + rowHeight * 0.5f);
                    draw->AddLine(ImVec2(p.x - 4.0f, p.y), ImVec2(p.x - 1.0f, p.y + 3.0f),
                        ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);
                    draw->AddLine(ImVec2(p.x - 1.0f, p.y + 3.0f), ImVec2(p.x + 5.0f, p.y - 4.0f),
                        ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);
                }
                ImGui::PopID();
            }

            const auto current = parse_id_list(setting->value());
            bool hasCustom = false;
            for (const auto& id : current) {
                if (is_known_container(id)) continue;
                if (!hasCustom) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Add-on");
                    hasCustom = true;
                }
                ImGui::PushID(id.c_str());
                const float rowHeight = ImGui::GetFrameHeight();
                const bool clicked = ImGui::Selectable("##custom_container_option", true,
                    0, ImVec2(0.0f, rowHeight));
                const ImVec2 rowMin = ImGui::GetItemRectMin();
                const ImVec2 rowMax = ImGui::GetItemRectMax();
                if (clicked) toggle_id(id, false);
                draw_container_icon(id,
                    ImVec2(rowMin.x + 9.0f, rowMin.y + (rowHeight - 19.0f) * 0.5f), 19.0f);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rowMin.x + 36.0f, rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), id.c_str());
                const ImVec2 p(rowMax.x - 18.0f, rowMin.y + rowHeight * 0.5f);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - 4.0f, p.y), ImVec2(p.x - 1.0f, p.y + 3.0f),
                    ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - 1.0f, p.y + 3.0f), ImVec2(p.x + 5.0f, p.y - 4.0f),
                    ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(-62.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##container_search", "Type a container name or namespace:id...",
            searchBuffer, sizeof(sourceSearch), ImGuiInputTextFlags_EnterReturnsTrue);
        const ImVec2 searchMin = ImGui::GetItemRectMin();
        const ImVec2 searchMax = ImGui::GetItemRectMax();
        const bool searchActive = ImGui::IsItemActive();
        const auto matches = container_suggestions(searchBuffer, 12);

        ImGui::SameLine();
        const bool addClicked = ImGui::Button("Add", ImVec2(54.0f, 0.0f));
        if (addClicked || submitted) {
            if (add_container_id(setting, oppositeSetting, searchBuffer)) {
                feedback = "Container type added.";
                searchBuffer[0] = '\0';
            } else {
                feedback = "Choose a suggestion or enter an exact namespace:id.";
            }
        }
        if (!feedback.empty()) ImGui::TextDisabled("%s", feedback.c_str());

        if (searchActive && searchBuffer[0] != '\0' && !matches.empty())
            ImGui::OpenPopup("##container_suggestions");
        ImGui::SetNextWindowPos(ImVec2(searchMin.x, searchMax.y + 3.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(searchMax.x - searchMin.x, 190.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("##container_suggestions",
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            for (const auto* option : matches) {
                ImGui::PushID(option->id.data());
                const ImVec2 iconStart = ImGui::GetCursorScreenPos();
                const float rowHeight = ImGui::GetFrameHeight();
                draw_container_icon(option->id,
                    ImVec2(iconStart.x, iconStart.y + (rowHeight - 20.0f) * 0.5f), 20.0f);
                ImGui::Dummy(ImVec2(20.0f, rowHeight));
                ImGui::SameLine(0.0f, 6.0f);
                const std::string label = std::string(option->label) + "  (" + std::string(option->id) + ")";
                if (ImGui::Selectable(label.c_str())) {
                    if (add_container_id(setting, oppositeSetting, option->id.data())) {
                        feedback = "Container type added.";
                        searchBuffer[0] = '\0';
                    } else {
                        feedback = "This container type is already selected.";
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
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
            const std::string_view candidate = qualified
                ? id
                : id.substr(std::string_view("minecraft:").size());
            if (candidate.find(needle) != std::string_view::npos) {
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
        const std::string typed = lower_copy(input);
        std::string id;

        for (const auto candidate : container_transfer_data::kVanillaItemIds) {
            const std::string full = lower_copy(candidate);
            const std::string shortId = full.starts_with("minecraft:") ? full.substr(10) : full;
            if (typed == full || typed == shortId) {
                id.assign(candidate);
                break;
            }
        }
        if (id.empty()) {
            if (typed.find(':') != std::string::npos) {
                id = normalize_id(input);
            } else {
                const auto matches = item_suggestions(input, 0);
                // Do not silently add the first result when a search term has
                // several possible matches; the user must choose one explicitly.
                if (matches.size() != 1) return false;
                id.assign(matches.front());
            }
        }
        if (!valid_resource_id(id)) return false;
        auto selected = parse_id_list(setting->value());
        if (contains_id(selected, id)) return false;
        selected.push_back(std::move(id));
        setting->set_value(join_ids(selected));
        persist_active_profile();
        return true;
    }

    void remove_item_id(config::StringSetting* setting, std::string_view id) {
        if (setting == nullptr) return;
        auto values = parse_id_list(setting->value());
        const auto before = values.size();
        remove_id(values, id);
        if (values.size() != before) {
            setting->set_value(join_ids(values));
            persist_active_profile();
        }
    }

    void render_item_picker(config::StringSetting* setting) {
        if (setting == nullptr) return;
        static char searchBuffer[160]{};
        static ItemCompletionCycle completionCycle{};
        static std::string feedback;
        const auto selected = parse_id_list(setting->value());
        const std::string preview = selected.empty() ? "No items selected" :
            std::to_string(selected.size()) + " selected";
        ImGui::TextDisabled("ITEM FILTER");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##selected_items", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (selected.empty()) ImGui::TextDisabled("No item IDs added.");
            for (const auto& id : selected) {
                ImGui::PushID(id.c_str());
                ImGui::TextUnformatted(id.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    remove_item_id(setting, id);
                    feedback = "Removed " + id + ".";
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(-118.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##item_id_search", "Type an item name or ID... (Tab to cycle)",
            searchBuffer, sizeof(searchBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion,
            complete_item_on_tab, &completionCycle);
        const ImVec2 searchMin = ImGui::GetItemRectMin();
        const ImVec2 searchMax = ImGui::GetItemRectMax();
        const bool searchActive = ImGui::IsItemActive();
        const auto matches = item_suggestions(searchBuffer, 12);

        ImGui::SameLine();
        const bool addClicked = ImGui::Button("Add", ImVec2(52.0f, 0.0f));
        if (addClicked || submitted) {
            if (add_item_from_search(setting, searchBuffer)) {
                feedback = "Item added.";
                set_item_search(searchBuffer, sizeof(searchBuffer), "", completionCycle);
            } else {
                const auto allMatches = item_suggestions(searchBuffer, 0);
                if (allMatches.empty())
                    feedback = "No matching item. Use a full namespace:id for custom items.";
                else if (allMatches.size() > 1)
                    feedback = "Several items match. Choose the exact item from the suggestions.";
                else
                    feedback = "This item is already selected or the ID is invalid.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(56.0f, 0.0f))) {
            setting->set_value("");
            persist_active_profile();
            feedback = "Item filter cleared.";
        }

        if (searchActive && searchBuffer[0] != '\0' && !matches.empty())
            ImGui::OpenPopup("##item_autocomplete");
        ImGui::SetNextWindowPos(ImVec2(searchMin.x, searchMax.y + 3.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(searchMax.x - searchMin.x, 210.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("##item_autocomplete",
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            for (const auto id : matches) {
                const std::string shortName(id.starts_with("minecraft:") ? id.substr(10) : id);
                std::string label = shortName;
                std::replace(label.begin(), label.end(), '_', ' ');
                ImGui::PushID(id.data());
                if (ImGui::Selectable(label.c_str())) {
                    if (add_item_from_search(setting, id.data())) {
                        feedback = "Item added.";
                        set_item_search(searchBuffer, sizeof(searchBuffer), "", completionCycle);
                    } else {
                        feedback = "This item is already selected.";
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%.*s", static_cast<int>(id.size()), id.data());
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        if (!feedback.empty()) ImGui::TextDisabled("%s", feedback.c_str());
    }



}

void ContainerTransfer::on_settings_render() {
    ImGui::TextWrapped("Set source, destination, and item filters. Click a matching container to transfer.");
    ImGui::TextDisabled("A container cannot be both source and destination.");
    const auto sourceIds = parse_id_list(m_sourceBlocks ? m_sourceBlocks->value() : "");
    const auto destinationIds = parse_id_list(m_destinationBlocks ? m_destinationBlocks->value() : "");
    const auto itemIds = parse_id_list(m_itemIds ? m_itemIds->value() : "");
    ImGui::TextDisabled("%zu source types  |  %zu destination types  |  %zu item types",
        sourceIds.size(), destinationIds.size(), itemIds.size());
    if (sourceIds.empty()) ImGui::TextColored(ImVec4(0.72f, 0.39f, 0.12f, 1.0f), "Select a source container.");
    if (destinationIds.empty()) ImGui::TextColored(ImVec4(0.72f, 0.39f, 0.12f, 1.0f), "Select a destination container.");
    if (itemIds.empty()) ImGui::TextColored(ImVec4(0.72f, 0.39f, 0.12f, 1.0f), "Select an item type.");
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
    render_container_selector("SOURCE", "source", m_sourceBlocks, m_destinationBlocks);
    ImGui::Spacing();
    render_container_selector("DESTINATION", "destination", m_destinationBlocks, m_sourceBlocks);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    render_item_picker(m_itemIds);
    ImGui::PopStyleVar();
}
