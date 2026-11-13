#pragma once

#include "modules/module.h"
#include "config/setting.h"
#include "utils/math.h"

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

class Actor;
class LocalPlayer;
struct ActorTickEvent;

class AutoFishing final : public Module {
public:
    AutoFishing();

    void on_enable() override;
    void on_disable() override;
    void on_settings_render() override;

private:
    enum class State {
        Disabled,
        WaitingForWorld,
        WaitingForBobber,
        Fishing,
        WaitingForBobberReturn,
        RecastDelay,
        NoBobber,
        InputUnavailable
    };

    void on_actor_tick(ActorTickEvent& event);
    bool game_is_focused() const;
    bool send_rod_use() const;
    std::vector<Actor*> fishing_hooks(LocalPlayer* player) const;
    Actor* find_hook(const std::vector<Actor*>& hooks, std::uint64_t uniqueId) const;
    void prepare_cast(const std::vector<Actor*>& hooks, std::uint64_t now);

    config::IntSetting* m_recastDelayMs = nullptr;
    config::FloatSetting* m_biteThreshold = nullptr;
    config::IntSetting* m_detectionWarmupMs = nullptr;

    mutable std::mutex m_mutex;
    State m_state = State::Disabled;
    std::uint64_t m_tickListener = 0;
    std::uint64_t m_stateSince = 0;
    std::uint64_t m_bobberId = 0;
    math::Vec3 m_lastBobberPosition{};
    std::unordered_set<std::uint64_t> m_previousHookIds;
};
