#include "modules/tools/ContainerTransfer.h"

#include "minecraft/sdk/ui/container_ui.h"
#include "utils/logger.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <string>
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
    if (m_phase == Phase::Opening) {
        LOG_WARN("[ContainerTransfer] The clicked block did not produce a capturable container screen; no items were moved.");
        reset_transfer();
    }
    minecraft::container_ui::cancel_pending_open();
}

void ContainerTransfer::on_container_opened(void* controller) {
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
    if (m_phase != Phase::Idle && m_phase != Phase::Closing) {
        LOG_WARN("[ContainerTransfer] The hidden container closed unexpectedly; the transfer was stopped.");
    } else if (m_phase == Phase::Closing && m_moved) {
        LOG_INFO("[ContainerTransfer] Container transfer session ended.");
    }
    reset_transfer();
}

void ContainerTransfer::on_disable() {
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
