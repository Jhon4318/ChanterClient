#pragma once

#include "modules/module.h"
#include "config/setting.h"

#include <atomic>
#include <string>

struct BlockUseEvent;

class ContainerTransfer final : public Module {
public:
    ContainerTransfer();
    ~ContainerTransfer() override;

    static ContainerTransfer* instance();

    // Called by the block-use event listener. At this stage this only validates
    // the configured source/destination identifiers and reports matched clicks.
    void on_block_use(const BlockUseEvent& event);

private:
    static std::atomic<ContainerTransfer*> s_instance;

    config::StringSetting* m_sourceBlocks = nullptr;
    config::StringSetting* m_destinationBlocks = nullptr;
    config::StringSetting* m_itemIds = nullptr;

    bool matches_block_list(const config::StringSetting* setting,
                            const std::string& blockId) const;
};
