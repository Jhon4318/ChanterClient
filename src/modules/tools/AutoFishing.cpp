#include "modules/tools/AutoFishing.h"

#include "events/actor_entity_tick_event.h"
#include "events/event_bus.h"
#include "events/fishing_packet_event.h"
#include "minecraft/hooks/fishing_packet_hook.h"
#include "minecraft/sdk/actor/actor.h"
#include "minecraft/sdk/actor/local_player.h"
#include "minecraft/sdk/client/client_instance.h"
#include "minecraft/sdk/components/ActorTypeComponent.h"
#include "ui/notifications.h"
#include "utils/logger.h"

#include <Windows.h>
#include <imgui.h>

#include <cmath>
#include <atomic>

namespace {
constexpr std::uint64_t kBobberSpawnTimeoutMs = 5000;
constexpr std::uint64_t kBobberReturnTimeoutMs = 2000;
constexpr std::uint64_t kStaleBobberTimeoutMs = 30000;
constexpr float kBobberSearchRangeSq = 48.0f * 48.0f;
constexpr float kStationaryThreshold = 0.001f;
std::atomic_bool g_traceLocalTick{false};
std::atomic_bool g_traceActorBobberTick{false};
std::atomic_bool g_traceBobberMove{false};
std::atomic_bool g_traceBobberMotion{false};
}

AutoFishing::AutoFishing()
    : Module("Tools", "AutoFishing", "Reel in and recast when a fish bites.") {
    m_recastDelayMs = add_setting<config::IntSetting>(
        "Recast delay", "Delay before casting again.", 400, 100, 1500);
    m_biteThreshold = add_setting<config::FloatSetting>(
        "Bite sensitivity", "Downward bobber movement threshold.", 0.20f, 0.10f, 0.80f, "%.2f");
    m_detectionWarmupMs = add_setting<config::IntSetting>(
        "Detection warmup", "Ignore bobber movement just after casting.", 1000, 500, 3000);
}

void AutoFishing::on_enable() {
    g_traceLocalTick.store(false, std::memory_order_relaxed);
    g_traceActorBobberTick.store(false, std::memory_order_relaxed);
    g_traceBobberMove.store(false, std::memory_order_relaxed);
    g_traceBobberMotion.store(false, std::memory_order_relaxed);
    m_packetHooksReady = minecraft::fishing::initialize();
    if (!m_packetHooksReady) {
        LOG_WARN("[AutoFishing] Packet tracking unavailable; actor-tick fallback remains active.");
    }

    {
        std::lock_guard lock(m_mutex);
        m_state = State::WaitingForWorld;
        m_stateSince = GetTickCount64();
        m_bobberRuntimeId = 0;
        m_pendingReel = false;
        m_havePlayerPosition = false;
        m_previousBobberIds.clear();
        m_bobbers.clear();
    }

    m_tickListener = EventBus::instance().subscribe<ActorEntityTickEvent>(
        [this](ActorEntityTickEvent& event) { on_actor_entity_tick(event); });
    m_packetListener = EventBus::instance().subscribe<FishingPacketEvent>(
        [this](FishingPacketEvent& event) { on_fishing_packet(event); });

    LOG_INFO("[AutoFishing] Enabled. Hold a fishing rod and aim at water.");
}

void AutoFishing::on_disable() {
    if (m_tickListener != 0) {
        EventBus::instance().unsubscribe(m_tickListener);
        m_tickListener = 0;
    }
    if (m_packetListener != 0) {
        EventBus::instance().unsubscribe(m_packetListener);
        m_packetListener = 0;
    }

    {
        std::lock_guard lock(m_mutex);
        m_state = State::Disabled;
        m_stateSince = 0;
        m_bobberRuntimeId = 0;
        m_pendingReel = false;
        m_havePlayerPosition = false;
        m_previousBobberIds.clear();
        m_bobbers.clear();
    }

    if (m_packetHooksReady) {
        minecraft::fishing::shutdown();
        m_packetHooksReady = false;
    }
}

bool AutoFishing::game_is_focused() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

bool AutoFishing::send_rod_use() const {
    HWND window = GetForegroundWindow();
    if (!window || !game_is_focused() ||
        ClientInstance::isClientUiOpen() ||
        ClientInstance::isGameUiOpen()) {
        return false;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(window, &cursor)) {
        cursor.x = 0;
        cursor.y = 0;
    }
    const LPARAM mousePosition = MAKELPARAM(
        static_cast<SHORT>(cursor.x), static_cast<SHORT>(cursor.y));

    // Queue input for the window message loop. Do not synchronously re-enter
    // Minecraft's window procedure from inside Actor::normalTick.
    if (!PostMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, mousePosition)) {
        LOG_WARN("[AutoFishing] Failed to queue right-button down (error={}).", GetLastError());
        return false;
    }
    if (!PostMessageW(window, WM_RBUTTONUP, 0, mousePosition)) {
        LOG_WARN("[AutoFishing] Failed to queue right-button up (error={}).", GetLastError());
        return false;
    }

    LOG_INFO("[AutoFishing] Right-click messages queued for Minecraft.");
    return true;
}

void AutoFishing::prepare_cast(std::uint64_t now) {
    m_previousBobberIds.clear();
    for (const auto& [id, data] : m_bobbers) {
        if (now >= data.lastUpdate && now - data.lastUpdate <= kStaleBobberTimeoutMs) {
            m_previousBobberIds.insert(id);
        }
    }

    if (!send_rod_use()) {
        m_state = State::SearchingForBobber;
        m_stateSince = now;
        ui::notifications::warning(
            "AutoFishing", "Automatic cast failed. Manual casts are still monitored.");
        LOG_WARN("[AutoFishing] Could not dispatch cast input; monitoring for a manual bobber.");
        return;
    }

    m_state = State::WaitingForBobber;
    m_stateSince = now;
    m_bobberRuntimeId = 0;
    m_pendingReel = false;
}

void AutoFishing::try_adopt_bobber(std::uint64_t now) {
    if (m_bobberRuntimeId != 0 || !m_havePlayerPosition) return;

    std::uint64_t bestId = 0;
    float bestDistanceSq = kBobberSearchRangeSq;
    for (const auto& [id, data] : m_bobbers) {
        if (m_previousBobberIds.contains(id)) continue;
        if (now < data.lastUpdate || now - data.lastUpdate > kStaleBobberTimeoutMs) continue;
        const float distanceSq = data.position.distance_sq(m_localPlayerPosition);
        if (distanceSq < bestDistanceSq) {
            bestDistanceSq = distanceSq;
            bestId = id;
        }
    }
    if (bestId == 0) return;

    const auto found = m_bobbers.find(bestId);
    if (found == m_bobbers.end()) return;
    m_bobberRuntimeId = bestId;
    m_lastBobberPosition = found->second.position;
    m_lastBobberVelocity = found->second.velocity;
    m_state = State::Fishing;
    m_stateSince = now;
    LOG_INFO("[AutoFishing] Fishing bobber detected (runtime ID={}, distance={:.2f}).",
        bestId, std::sqrt(bestDistanceSq));
}

void AutoFishing::evaluate_bite(
    const math::Vec3& newPosition, const math::Vec3& newVelocity,
    bool hasPosition, bool hasVelocity, std::uint64_t now) {
    if (m_state != State::Fishing || m_bobberRuntimeId == 0 || m_pendingReel) return;

    const auto warmup = static_cast<std::uint64_t>(
        m_detectionWarmupMs ? m_detectionWarmupMs->value() : 1000);
    if (now - m_stateSince < warmup) return;

    const float threshold = m_biteThreshold ? m_biteThreshold->value() : 0.20f;
    const math::Vec3 delta = newPosition - m_lastBobberPosition;
    const bool movementBite = hasPosition &&
        delta.y <= -threshold &&
        std::abs(delta.x) < kStationaryThreshold &&
        std::abs(delta.z) < kStationaryThreshold;
    const bool velocityBite = hasVelocity && newVelocity.y <= -threshold;

    if (hasPosition) m_lastBobberPosition = newPosition;
    if (hasVelocity) m_lastBobberVelocity = newVelocity;
    if (!movementBite && !velocityBite) return;

    m_pendingReel = true;
    LOG_INFO("[AutoFishing] Bobber bite movement detected; reel queued.");
}

void AutoFishing::on_fishing_packet(FishingPacketEvent& event) {
    if (!enabled() || event.runtimeId == 0) return;
    std::lock_guard lock(m_mutex);
    if (!enabled()) return;

    const std::uint64_t now = GetTickCount64();
    switch (event.kind) {
    case FishingPacketEvent::Kind::Spawn: {
        LOG_INFO("[AutoFishing][Trace] FishingHook spawn event: runtimeId={}, pos=({:.3f}, {:.3f}, {:.3f}), velocity=({:.3f}, {:.3f}, {:.3f}).",
            event.runtimeId, event.position.x, event.position.y, event.position.z,
            event.velocity.x, event.velocity.y, event.velocity.z);
        BobberData data{};
        data.position = event.position;
        data.velocity = event.velocity;
        data.lastUpdate = now;
        m_bobbers.insert_or_assign(event.runtimeId, data);
        try_adopt_bobber(now);
        break;
    }
    case FishingPacketEvent::Kind::Move: {
        auto found = m_bobbers.find(event.runtimeId);
        if (found == m_bobbers.end()) return;
        found->second.lastUpdate = now;
        if (event.runtimeId == m_bobberRuntimeId) {
            if (!g_traceBobberMove.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO("[AutoFishing][Trace] First bobber position update: pos=({:.3f}, {:.3f}, {:.3f}).",
                    event.position.x, event.position.y, event.position.z);
            }
            evaluate_bite(event.position, found->second.velocity, true, false, now);
        }
        found->second.position = event.position;
        break;
    }
    case FishingPacketEvent::Kind::Motion: {
        auto found = m_bobbers.find(event.runtimeId);
        if (found == m_bobbers.end()) return;
        found->second.velocity = event.velocity;
        found->second.lastUpdate = now;
        if (event.runtimeId == m_bobberRuntimeId) {
            if (!g_traceBobberMotion.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO("[AutoFishing][Trace] First bobber velocity update: velocity=({:.3f}, {:.3f}, {:.3f}).",
                    event.velocity.x, event.velocity.y, event.velocity.z);
            }
            evaluate_bite(found->second.position, event.velocity, false, true, now);
        }
        break;
    }
    case FishingPacketEvent::Kind::Remove:
        m_bobbers.erase(event.runtimeId);
        m_previousBobberIds.erase(event.runtimeId);
        if (event.runtimeId == m_bobberRuntimeId) {
            m_bobberRuntimeId = 0;
            m_pendingReel = false;
            m_state = State::RecastDelay;
            m_stateSince = now;
            LOG_INFO("[AutoFishing] Bobber removed; recast queued.");
        }
        break;
    }
}

void AutoFishing::on_actor_entity_tick(ActorEntityTickEvent& event) {
    if (!enabled() || !event.localPlayer || !event.clientInstance || !event.tickingActor) return;
    if (ClientInstance::isClientUiOpen() || ClientInstance::isGameUiOpen() || !game_is_focused()) return;

    std::lock_guard lock(m_mutex);
    if (!enabled()) return;

    const std::uint64_t now = GetTickCount64();
    Actor* actor = event.tickingActor;

    if (actor != event.localPlayer && actor->getActorType() == ActorType::FishingHook) {
        if (!g_traceActorBobberTick.exchange(true, std::memory_order_relaxed)) {
            LOG_INFO("[AutoFishing][Trace] Actor::normalTick observed a FishingHook actor.");
        }
        const std::uint64_t runtimeId = actor->getRuntimeId();
        if (runtimeId != 0) {
            auto& data = m_bobbers[runtimeId];
            const math::Vec3 position = actor->getPosition();
            const math::Vec3 velocity = actor->getVelocity();
            if (runtimeId == m_bobberRuntimeId) {
                evaluate_bite(position, velocity, true, true, now);
            }
            data.position = position;
            data.velocity = velocity;
            data.lastUpdate = now;
            if (m_state == State::WaitingForBobber || m_state == State::SearchingForBobber ||
                m_state == State::WaitingForWorld) {
                try_adopt_bobber(now);
            }
        }
        return;
    }
    if (actor != event.localPlayer) return;

    if (!g_traceLocalTick.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO("[AutoFishing][Trace] LocalPlayer tick listener is running.");
    }
    m_localPlayerPosition = event.localPlayer->getPosition();
    m_havePlayerPosition = true;

    switch (m_state) {
    case State::Disabled:
    case State::InputUnavailable:
    case State::Fishing:
        break;
    case State::WaitingForWorld:
        try_adopt_bobber(now);
        if (m_bobberRuntimeId == 0) prepare_cast(now);
        break;
    case State::WaitingForBobber:
        try_adopt_bobber(now);
        if (m_state == State::WaitingForBobber && now - m_stateSince >= kBobberSpawnTimeoutMs) {
            m_state = State::SearchingForBobber;
            m_stateSince = now;
            ui::notifications::warning(
                "AutoFishing", "Automatic cast was not detected. Manual casting is still monitored.");
            LOG_WARN("[AutoFishing] No bobber detected after cast; continuing to monitor manual casts.");
        }
        break;
    case State::SearchingForBobber:
        try_adopt_bobber(now);
        break;
    case State::WaitingForBobberReturn:
        if (now - m_stateSince >= kBobberReturnTimeoutMs) {
            m_state = State::RecastDelay;
            m_stateSince = now;
            LOG_WARN("[AutoFishing] Bobber return timed out; recast queued.");
        }
        break;
    case State::RecastDelay: {
        const int delay = m_recastDelayMs ? m_recastDelayMs->value() : 400;
        if (now - m_stateSince >= static_cast<std::uint64_t>(delay)) prepare_cast(now);
        break;
    }
    }

    if (m_pendingReel && m_state == State::Fishing) {
        if (send_rod_use()) {
            m_pendingReel = false;
            m_state = State::WaitingForBobberReturn;
            m_stateSince = now;
            LOG_INFO("[AutoFishing] Reeling input dispatched.");
        } else {
            m_pendingReel = false;
            m_state = State::InputUnavailable;
            ui::notifications::warning("AutoFishing", "Could not reel. Focus Minecraft and close menus.");
            LOG_WARN("[AutoFishing] Could not dispatch reel input.");
        }
    }
}

void AutoFishing::on_settings_render() {
    Module::on_settings_render();
    std::lock_guard lock(m_mutex);

    const char* status = "Stopped";
    switch (m_state) {
    case State::Disabled: status = "Stopped"; break;
    case State::WaitingForWorld: status = "Preparing"; break;
    case State::WaitingForBobber: status = "Waiting for bobber"; break;
    case State::SearchingForBobber: status = "Searching for bobber"; break;
    case State::Fishing: status = "Waiting for bite"; break;
    case State::WaitingForBobberReturn: status = "Reeling in"; break;
    case State::RecastDelay: status = "Recasting"; break;
    case State::InputUnavailable: status = "Input unavailable"; break;
    }
    ImGui::TextDisabled("Status: %s", status);
    ImGui::TextDisabled("Hold a rod and aim at water before enabling.");
}
