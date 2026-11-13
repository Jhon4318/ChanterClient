#include "modules/tools/AutoFishing.h"

#include "events/actor_tick_event.h"
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
constexpr std::uint64_t kBobberReturnTimeoutMs = 2000;
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
        m_bobberId = 0;
        m_previousHookIds.clear();
    }

    m_tickListener = EventBus::instance().subscribe<ActorTickEvent>(
        [this](ActorTickEvent& event) { on_actor_tick(event); });
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
        return false;
    }
    return true;
}

std::vector<Actor*> AutoFishing::fishing_hooks(LocalPlayer* player) const {
    std::vector<Actor*> hooks;
    if (!player) return hooks;

    const auto actors = Actor::get_all(player);
    hooks.reserve(actors.size());
    for (Actor* actor : actors) {
        if (!actor || !actor->isAlive() || actor->getActorType() != ActorType::FishingHook)
            continue;
        hooks.push_back(actor);
    }
    return hooks;
}

Actor* AutoFishing::find_hook(const std::vector<Actor*>& hooks, std::uint64_t uniqueId) const {
    if (uniqueId == 0) return nullptr;
    for (Actor* hook : hooks) {
        if (hook && hook->getUniqueId() == uniqueId) return hook;
    }
    return nullptr;
}

void AutoFishing::prepare_cast(const std::vector<Actor*>& hooks, std::uint64_t now) {
    m_previousHookIds.clear();
    for (Actor* hook : hooks) {
        if (!hook) continue;
        const auto id = hook->getUniqueId();
        if (id != 0) m_previousHookIds.insert(id);
    }

    if (!send_rod_use()) {
        m_state = State::InputUnavailable;
        ui::notifications::warning("AutoFishing", "Game input unavailable. Focus Minecraft and try again.");
        return;
    }

    m_state = State::WaitingForBobber;
    m_stateSince = now;
    m_bobberId = 0;
}

void AutoFishing::on_actor_tick(ActorTickEvent& event) {
    if (!enabled() || !event.localPlayer || !event.clientInstance) return;
    if (ClientInstance::isClientUiOpen() || ClientInstance::isGameUiOpen() || !game_is_focused())
        return;

    std::lock_guard lock(m_mutex);
    if (!enabled()) return;

    const std::uint64_t now = GetTickCount64();
    const auto hooks = fishing_hooks(event.localPlayer);

    switch (m_state) {
    case State::Disabled:
    case State::NoBobber:
    case State::InputUnavailable:
        return;

    case State::WaitingForWorld:
        prepare_cast(hooks, now);
        return;

    case State::WaitingForBobber: {
        Actor* newHook = nullptr;
        float nearestDistanceSq = kBobberSearchRangeSq;
        const math::Vec3 playerPosition = event.localPlayer->getPosition();

        for (Actor* hook : hooks) {
            const auto id = hook ? hook->getUniqueId() : 0;
            if (!hook || id == 0 || m_previousHookIds.contains(id)) continue;

            const float distanceSq = hook->getPosition().distance_sq(playerPosition);
            if (distanceSq < nearestDistanceSq) {
                nearestDistanceSq = distanceSq;
                newHook = hook;
            }
        }

        if (newHook) {
            m_bobberId = newHook->getUniqueId();
            m_lastBobberPosition = newHook->getPosition();
            m_state = State::Fishing;
            m_stateSince = now;
            LOG_INFO("[AutoFishing] Fishing bobber detected.");
        } else if (now - m_stateSince >= kBobberSpawnTimeoutMs) {
            m_state = State::NoBobber;
            ui::notifications::warning("AutoFishing", "No bobber detected. Check the rod and aim.");
            LOG_WARN("[AutoFishing] No new fishing bobber was detected after casting.");
        }
        return;
    }

    case State::Fishing: {
        Actor* hook = find_hook(hooks, m_bobberId);
        if (!hook) {
            m_state = State::RecastDelay;
            m_stateSince = now;
            return;
        }

        const math::Vec3 position = hook->getPosition();
        const math::Vec3 velocity = hook->getVelocity();
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
        LOG_INFO("[AutoFishing] Bite detected; reeling in.");
        return;
    }

    case State::WaitingForBobberReturn:
        if (!find_hook(hooks, m_bobberId)) {
            m_state = State::RecastDelay;
            m_stateSince = now;
        } else if (now - m_stateSince >= kBobberReturnTimeoutMs) {
            Actor* hook = find_hook(hooks, m_bobberId);
            if (hook) m_lastBobberPosition = hook->getPosition();
            m_state = State::Fishing;
            m_stateSince = now;
            LOG_WARN("[AutoFishing] Bobber did not retract; waiting for the next bite.");
        }
        return;

    case State::RecastDelay: {
        const int delay = m_recastDelayMs ? m_recastDelayMs->value() : 400;
        if (now - m_stateSince < static_cast<std::uint64_t>(delay)) return;
        prepare_cast(hooks, now);
        return;
    }
    }
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
