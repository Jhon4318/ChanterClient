#pragma once

#include "modules/module.h"
#include "config/setting.h"
#include "minecraft/sdk/ui/container_ui.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class ContainerTransfer final : public Module, public minecraft::container_ui::Listener {
public:
    ContainerTransfer();
    ~ContainerTransfer() override;

    static ContainerTransfer* instance();

    // Called before vanilla GameMode::useItemOn so the exact container screen
    // created by this user click can be kept off the visible scene stack.
    bool prepare_block_use(const std::string& blockId, std::int32_t x,
                           std::int32_t y, std::int32_t z, bool firstEvent);
    void on_open_attempt_failed();

    void on_container_opened(void* controller) override;
    void on_container_tick() override;
    void on_container_lost() override;

    void on_disable() override;

private:
    enum class Direction {
        None,
        WithdrawFromContainer,
        DepositIntoContainer
    };

    enum class Phase {
        Idle,
        Opening,
        Moving,
        Closing
    };

    static std::atomic<ContainerTransfer*> s_instance;

    config::StringSetting* m_sourceBlocks = nullptr;
    config::StringSetting* m_destinationBlocks = nullptr;
    config::StringSetting* m_itemIds = nullptr;

    Direction m_direction = Direction::None;
    Phase m_phase = Phase::Idle;
    std::vector<std::string> m_itemFilter;
    std::vector<std::pair<std::string, int>> m_passClicked;

    std::uint64_t m_phaseSince = 0;
    std::uint64_t m_lastClickAt = 0;
    int m_lastTotal = -1;
    bool m_inPass = false;
    bool m_moved = false;
    bool m_reportedNoProgress = false;

    bool matches_block_list(const config::StringSetting* setting,
                            const std::string& blockId) const;
    bool matches_item(const std::string& itemId) const;

    std::vector<std::pair<std::string, int>> collect_candidates(int& total) const;
    std::vector<std::pair<std::string, int>> player_slots() const;
    bool destination_has_room(const std::string& itemId) const;

    void begin_closing(const char* reason);
    void reset_transfer();
    void finish_pass_or_continue(std::uint64_t now);
};
