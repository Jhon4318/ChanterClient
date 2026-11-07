#include "modules/tools/ContainerTransfer.h"

#include "events/block_use_event.h"
#include "utils/logger.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

std::atomic<ContainerTransfer*> ContainerTransfer::s_instance{nullptr};

namespace {
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
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        return result;
    }
}

ContainerTransfer::ContainerTransfer()
    : Module("Tools", "ContainerTransfer",
        "取り出し元・収納先とアイテムの設定。現在はクリック検出段階で、まだアイテムの移動は行いません。") {
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
}

ContainerTransfer::~ContainerTransfer() {
    ContainerTransfer* expected = this;
    s_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

ContainerTransfer* ContainerTransfer::instance() {
    return s_instance.load(std::memory_order_acquire);
}

bool ContainerTransfer::matches_block_list(
    const config::StringSetting* setting,
    const std::string& blockId) const {
    if (setting == nullptr || blockId.empty()) {
        return false;
    }

    const std::string normalizedBlock = normalize_id(blockId);
    const auto ids = parse_id_list(setting->value());
    return std::find(ids.begin(), ids.end(), normalizedBlock) != ids.end();
}

void ContainerTransfer::on_block_use(const BlockUseEvent& event) {
    if (!enabled() || !event.firstEvent) {
        return;
    }

    const bool source = matches_block_list(m_sourceBlocks, event.blockType);
    const bool destination = matches_block_list(m_destinationBlocks, event.blockType);
    if (!source && !destination) {
        return;
    }
    if (source && destination) {
        LOG_WARN("[ContainerTransfer] Block {} is configured as both source and destination; ignoring click.", event.blockType);
        return;
    }

    const auto itemIds = m_itemIds != nullptr ? parse_id_list(m_itemIds->value())
                                               : std::vector<std::string>{};
    if (itemIds.empty()) {
        LOG_WARN("[ContainerTransfer] Matched {} block {} at ({}, {}, {}), but no item IDs are configured.",
            source ? "source" : "destination", event.blockType, event.x, event.y, event.z);
        return;
    }

    // Click recognition is intentionally separated from slot movement and UI suppression.
    // The next implementation stage will only start a transfer after a verified hidden
    // container controller has been captured.
    LOG_INFO("[ContainerTransfer] Matched {} click: {} at ({}, {}, {}), {} item filter(s).",
        source ? "source" : "destination", event.blockType,
        event.x, event.y, event.z, itemIds.size());
}
