#include "modules/tools/AutoFishing.h"

#include "events/actor_entity_tick_event.h"
#include "events/event_bus.h"
#include "minecraft/sdk/actor/actor.h"
#include "minecraft/sdk/actor/local_player.h"
#include "minecraft/sdk/client/client_instance.h"
#include "minecraft/sdk/components/ActorTypeComponent.h"
#include "ui/notifications.h"
#include "utils/logger.h"

#include <Windows.h>
#include <imgui.h>

#include <cmath>

namespace {
constexpr std::uint64_t kBobberSpawnTimeoutMs = 5000;
constexpr std::uint64_t kBobberMissingGraceMs = 300;
constexpr float kBobberSearchRangeSq = 48.0f * 48.0f;
constexpr float kHorizontalMotionLimitSq = 0.30f * 0.30f;
}

AutoFishing::AutoFishing()
    : Module("Tools", "AutoFishing", "Reel in and recast when a fish bites.") {
    m_recastDelayMs = add_setting<config::IntSetting>(
        "Recast delay", "Delay before casting again.", 400, 100, 1500);
    m_biteThreshold = add_setting<config::FloatSetting>(
        "Bite sensitivity", "Downward bobber speed threshold.", 0.20f, 0.10f, 0.80f, "%.2f");
    m_detectionWarmupMs = add_setting<config::IntSetting>(
        "Detection warmup", "Ignore bobber movement just after casting.", 1000, 500, 3000);
}

void AutoFishing::on_enable() {
    {
        std::lock_guard lock(m_mutex);
        m_state = State::WaitingForWorld;
        m_stateSince = GetTickCount64();
        m_lastBobberTickAt = 0;
        m_bobberId = 0;
        m_previousHookIds.clear();
    }

    m_tickListener = EventBus::instance().subscribe<ActorEntityTickEvent>(
        [this](ActorEntityTickEvent& event) { on_actor_entity_tick(event); });
    LOG_INFO("[AutoFishing] Enabled. Hold a fishing rod and aim at water.");
}

void AutoFishing::on_disable() {
    if (m_tickListener != 0) {
        EventBus::instance().unsubscribe(m_tickListener);
        m_tickListener = 0;
    }

    std::lock_guard lock(m_mutex);
    m_state = State::Disabled;
    m_stateSince = 0;
    m_lastBobberTickAt = 0;
    m_bobberId = 0;
    m_previousHookIds.clear();
}

bool AutoFishing::game_is_focused() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

bool AutoFishing::send_rod_use() const {
    if (!game_is_focused() ||
        ClientInstance::isClientUiOpen() ||
        ClientInstance::isGameUiOpen()) {
        return false;
    }

    INPUT clicks[2]{};
    clicks[0].type = INPUT_MOUSE;
    clicks[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    clicks[1].type = INPUT_MOUSE;
    clicks[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;

    const UINT sent = SendInput(2, clicks, sizeof(INPUT));
    if (sent != 2) {
        INPUT release{};
        release.type = INPUT_MOUSE;
        release.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(1, &release, sizeof(INPUT));
        LOG_WARN("[AutoFishing] SendInput failed: inserted {} of 2 mouse events.", sent);
        return false;
    }

    LOG_INFO("[AutoFishing] Right-click input queued.");
    return true;
}

std::uint64_t AutoFishing::actor_identity(const Actor* actor) {
    if (!actor) return 0;
    const auto uniqueId = actor->getUniqueId();
    return uniqueId != 0 ? uniqueId : actor->getRuntimeId();
}

std::vector<Actor*> AutoFishing::fishing_hooks(LocalPlayer* player) const {
    std::vector<Actor*> hooks;
    if (!player) return hooks;

    const auto actors = Actor::get_all(player);
    hooks.reserve(actors.size());
    for (Actor* actor : actors) {
        // Do not use Actor::isAlive() here: FishingHook is a transient projectile,
        // and its dead-state marker is not a reliable actor-list filter.
        if (!actor || actor->getActorType() != ActorType::FishingHook)
            continue;
        hooks.push_back(actor);
    }
    return hooks;
}

void AutoFishing::prepare_cast(const std::vector<Actor*>& hooks, std::uint64_t now) {
    m_previousHookIds.clear();
    for (Actor* hook : hooks) {
        const auto id = actor_identity(hook);
        if (id != 0) m_previousHookIds.insert(id);
    }

    if (!send_rod_use()) {
        m_state = State::InputUnavailable;
        ui::notifications::warning("AutoFishing", "Game input unavailable. Focus Minecraft and try again.");
        return;
    }

    m_state = State::WaitingForBobber;
    m_stateSince = now;
    m_lastBobberTickAt = 0;
    m_bobberId = 0;
}

void AutoFishing::on_actor_entity_tick(ActorEntityTickEvent& event) {
    if (!enabled() || !event.localPlayer || !event.clientInstance || !event.tickingActor)
        return;
    if (ClientInstance::isClientUiOpen() || ClientInstance::isGameUiOpen() || !game_is_focused())
        return;

    std::lock_guard lock(m_mutex);
    if (!enabled()) return;

    const std::uint64_t now = GetTickCount64();
    Actor* actor = event.tickingActor;

    // Run timers and casting only on the local player's own tick.
    if (actor == event.localPlayer) {
        switch (m_state) {
        case State::Disabled:
        case State::NoBobber:
        case State::InputUnavailable:
        case State::Fishing:
        case State::WaitingForBobberReturn:
            if (m_state == State::WaitingForBobberReturn &&
                m_lastBobberTickAt != 0 &&
                now - m_lastBobberTickAt >= kBobberMissingGraceMs) {
                m_state = State::RecastDelay;
                m_stateSince = now;
            }
            return;

        case State::WaitingForWorld:
            prepare_cast(fishing_hooks(event.localPlayer), now);
            return;

        case State::WaitingForBobber:
            if (now - m_stateSince >= kBobberSpawnTimeoutMs) {
                m_state = State::NoBobber;
                ui::notifications::warning("AutoFishing", "No bobber tick detected. Check the cast and rod.");
                LOG_WARN("[AutoFishing] No FishingHook actor tick was received after casting.");
            }
            return;

        case State::RecastDelay: {
            const int delay = m_recastDelayMs ? m_recastDelayMs->value() : 400;
            if (now - m_stateSince < static_cast<std::uint64_t>(delay)) return;
            prepare_cast(fishing_hooks(event.localPlayer), now);
            return;
        }
        }
    }

    if (actor->getActorType() != ActorType::FishingHook)
        return;

    const std::uint64_t id = actor_identity(actor);
    if (id == 0) {
        LOG_WARN("[AutoFishing] FishingHook tick received without a usable entity ID.");
        return;
    }

    if (m_state == State::WaitingForBobber) {
        if (m_previousHookIds.contains(id)) return;

        const float distanceSq =
            actor->getPosition().distance_sq(event.localPlayer->getPosition());
        if (distanceSq > kBobberSearchRangeSq) return;

        m_bobberId = id;
        m_lastBobberPosition = actor->getPosition();
        m_lastBobberTickAt = now;
        m_state = State::Fishing;
        m_stateSince = now;
        LOG_INFO("[AutoFishing] FishingHook tick detected (id={}, distance={:.2f}).",
            id, std::sqrt(distanceSq));
        return;
    }

    if (id != m_bobberId)
        return;

    m_lastBobberTickAt = now;

    if (m_state == State::WaitingForBobberReturn)
        return;
    if (m_state != State::Fishing)
        return;

    const math::Vec3 position = actor->getPosition();
    const math::Vec3 velocity = actor->getVelocity();
    const math::Vec3 delta = position - m_lastBobberPosition;
    const bool warmedUp = now - m_stateSince >=
        static_cast<std::uint64_t>(m_detectionWarmupMs ? m_detectionWarmupMs->value() : 1000);
    const float threshold = m_biteThreshold ? m_biteThreshold->value() : 0.20f;
    const bool velocityBite = velocity.y <= -threshold;
    const bool movementBite = delta.y <= -threshold &&
        (delta.x * delta.x + delta.z * delta.z) <= kHorizontalMotionLimitSq;

    m_lastBobberPosition = position;
    if (!warmedUp || (!velocityBite && !movementBite)) return;

    if (!send_rod_use()) {
        m_state = State::InputUnavailable;
        ui::notifications::warning("AutoFishing", "Could not reel in. Focus Minecraft and restart AutoFishing.");
        return;
    }

    m_state = State::WaitingForBobberReturn;
    m_stateSince = now;
    m_lastBobberTickAt = now;
    LOG_INFO("[AutoFishing] Bite detected; reeling in.");
}

void AutoFishing::on_settings_render() {
    Module::on_settings_render();

    std::lock_guard lock(m_mutex);
    const char* status = "Stopped";
    switch (m_state) {
    case State::Disabled: status = "Stopped"; break;
    case State::WaitingForWorld: status = "Waiting for world"; break;
    case State::WaitingForBobber: status = "Waiting for bobber"; break;
    case State::Fishing: status = "Waiting for bite"; break;
    case State::WaitingForBobberReturn: status = "Reeling in"; break;
    case State::RecastDelay: status = "Recasting"; break;
    case State::NoBobber: status = "No bobber detected"; break;
    case State::InputUnavailable: status = "Input unavailable"; break;
    }
    ImGui::TextDisabled("Status: %s", status);
    ImGui::TextDisabled("Hold a rod and aim at water before enabling.");
}
