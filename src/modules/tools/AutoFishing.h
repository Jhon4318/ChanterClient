#pragma once

#include "modules/module.h"
#include "config/setting.h"
#include "utils/math.h"

#include <cstdint>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

class Actor;
struct ActorEntityTickEvent;

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
        SearchingForBobber,
        Fishing,
        WaitingForBobberReturn,
        RecastDelay,
        InputUnavailable
    };

    struct BobberData {
        math::Vec3 position{};
        math::Vec3 velocity{};
        std::uint64_t lastUpdate = 0;
    };

    void on_actor_entity_tick(ActorEntityTickEvent& event);
    bool game_is_focused() const;
    bool send_rod_use();
    void release_rod_use();
    void prepare_cast(std::uint64_t now);
    void try_adopt_bobber(std::uint64_t now);
    void evaluate_bite(const math::Vec3& newPosition,
                       const math::Vec3& newVelocity,
                       bool hasPosition, bool hasVelocity,
                       std::uint64_t now);

    config::IntSetting* m_recastDelayMs = nullptr;
    config::FloatSetting* m_biteThreshold = nullptr;
    config::IntSetting* m_detectionWarmupMs = nullptr;

    mutable std::mutex m_mutex;
    State m_state = State::Disabled;
    std::uint64_t m_tickListener = 0;
    std::uint64_t m_stateSince = 0;
    std::uint64_t m_bobberRuntimeId = 0;
    bool m_pendingReel = false;
    bool m_havePlayerPosition = false;
    std::atomic_bool m_rightClickReleasePending{false};
    math::Vec3 m_localPlayerPosition{};
    math::Vec3 m_lastBobberPosition{};
    math::Vec3 m_lastBobberVelocity{};
    std::unordered_map<std::uint64_t, BobberData> m_bobbers;
    std::unordered_set<std::uint64_t> m_previousBobberIds;
};
