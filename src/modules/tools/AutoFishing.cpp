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

#include <atomic>
#include <cmath>

namespace {
constexpr std::uint64_t kBobberSpawnTimeoutMs = 5000;
constexpr std::uint64_t kBobberReturnTimeoutMs = 2000;
constexpr std::uint64_t kStaleBobberTimeoutMs = 30000;
constexpr std::uint64_t kInitialCastDelayMs = 600;
constexpr float kBobberSearchRangeSq = 48.0f * 48.0f;

std::atomic_bool g_traceLocalTick{false};
std::atomic_bool g_traceActorBobberTick{false};
std::atomic_bool g_traceWorldActorScan{false};
std::atomic_bool g_traceWorldBobberScan{false};
}

AutoFishing::AutoFishing()
    : Module("Tools", "AutoFishing", "Reel in and recast when a fish bites.") {
    m_recastDelayMs = add_setting<config::IntSetting>(
        "Recast delay", "Delay before casting again.", 400, 100, 1500);
    m_initialCastDelayMs = add_setting<config::IntSetting>(
        "Initial cast delay", "Wait after enabling before the first cast.", 600, 200, 1500);
}

void AutoFishing::on_enable() {
    g_traceLocalTick.store(false, std::memory_order_relaxed);
    g_traceActorBobberTick.store(false, std::memory_order_relaxed);
    g_traceWorldActorScan.store(false, std::memory_order_relaxed);
    g_traceWorldBobberScan.store(false, std::memory_order_relaxed);

    {
        std::lock_guard lock(m_mutex);
        m_state = State::WaitingForWorld;
        m_stateSince = GetTickCount64();
        m_bobberRuntimeId = 0;
        m_castAttempts = 0;
        m_pendingReel = false;
        m_havePlayerPosition = false;
        m_previousBobberIds.clear();
        m_bobbers.clear();
    }

    m_tickListener = EventBus::instance().subscribe<ActorEntityTickEvent>(
        [this](ActorEntityTickEvent& event) { on_actor_entity_tick(event); });
    m_packetListener = EventBus::instance().subscribe<FishingPacketEvent>(
        [this](FishingPacketEvent& event) { on_fishing_packet(event); });

    const bool hooksReady = minecraft::fishing::initialize();
    m_packetHooksReady.store(hooksReady, std::memory_order_release);
    if (hooksReady) {
        LOG_INFO("[AutoFishing] Exact bite detection ready (server FishHookHooktime event).");
    } else {
        LOG_ERROR("[AutoFishing] Exact bite detection unavailable; automatic reeling is disabled.");
    }

    LOG_INFO("[AutoFishing] Enabled. First cast will be delayed briefly.");
}

void AutoFishing::on_disable() {
    release_rod_use();

    if (m_tickListener != 0) {
        EventBus::instance().unsubscribe(m_tickListener);
        m_tickListener = 0;
    }
    if (m_packetListener != 0) {
        EventBus::instance().unsubscribe(m_packetListener);
        m_packetListener = 0;
    }
    if (m_packetHooksReady.exchange(false, std::memory_order_acq_rel)) {
        minecraft::fishing::shutdown();
    }

    {
        std::lock_guard lock(m_mutex);
        if (m_state == State::Fishing) {
            const std::uint64_t stoppedAt = GetTickCount64();
            const std::uint64_t elapsed = stoppedAt >= m_stateSince ? stoppedAt - m_stateSince : 0;
            LOG_INFO("[AutoFishing] Disabled while waiting for a bite ({}s, bobber ID={}).",
                elapsed / 1000, m_bobberRuntimeId);
        }
        m_state = State::Disabled;
        m_stateSince = 0;
        m_bobberRuntimeId = 0;
        m_castAttempts = 0;
        m_pendingReel = false;
        m_havePlayerPosition = false;
        m_previousBobberIds.clear();
        m_bobbers.clear();
    }
}

bool AutoFishing::game_is_focused() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

bool AutoFishing::send_rod_use() {
    HWND window = GetForegroundWindow();
    if (!window || !game_is_focused() ||
        ClientInstance::isClientUiOpen() ||
        ClientInstance::isGameUiOpen()) {
        return false;
    }

    if (m_rightClickReleasePending.load(std::memory_order_acquire)) {
        LOG_WARN("[AutoFishing] Previous right-click is still awaiting release.");
        return false;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;

    const UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        LOG_WARN("[AutoFishing] SendInput did not insert right-button down (sent={}, error={}).",
            sent, GetLastError());
        return false;
    }

    m_rightClickReleasePending.store(true, std::memory_order_release);
    LOG_INFO("[AutoFishing] Right-button down queued; release scheduled for next player tick.");
    return true;
}

void AutoFishing::release_rod_use() {
    if (!m_rightClickReleasePending.load(std::memory_order_acquire)) return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    const UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        LOG_WARN("[AutoFishing] SendInput did not insert right-button up (sent={}, error={}); will retry.",
            sent, GetLastError());
        return;
    }

    m_rightClickReleasePending.store(false, std::memory_order_release);
    LOG_INFO("[AutoFishing] Right-button up queued after one player tick.");
}

void AutoFishing::prepare_cast(std::uint64_t now) {
    m_previousBobberIds.clear();
    for (const auto& [id, data] : m_bobbers) {
        if (now >= data.lastUpdate && now - data.lastUpdate <= kStaleBobberTimeoutMs) {
            m_previousBobberIds.insert(id);
        }
    }

    ++m_castAttempts;
    if (!send_rod_use()) {
        m_stateSince = now;
        if (m_castAttempts < 2) {
            m_state = State::RecastDelay;
            LOG_WARN("[AutoFishing] Cast input failed; one retry is queued.");
        } else {
            m_state = State::SearchingForBobber;
            ui::notifications::warning(
                "AutoFishing", "Automatic cast failed. Manual casts are still monitored.");
            LOG_WARN("[AutoFishing] Cast input failed twice; continuing to monitor manual casts.");
        }
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

    m_bobberRuntimeId = bestId;
    m_castAttempts = 0;
    m_pendingReel = false;
    m_state = State::Fishing;
    m_stateSince = now;
    LOG_INFO("[AutoFishing] Fishing bobber detected (runtime ID={}, distance={:.2f}).",
        bestId, std::sqrt(bestDistanceSq));
}

void AutoFishing::on_fishing_packet(FishingPacketEvent& event) {
    if (!enabled() || event.kind != FishingPacketEvent::Kind::Bite || event.runtimeId == 0) return;
    if (ClientInstance::isClientUiOpen() || ClientInstance::isGameUiOpen() || !game_is_focused()) return;

    std::lock_guard lock(m_mutex);
    if (!enabled() || m_state != State::Fishing ||
        m_bobberRuntimeId != event.runtimeId || m_pendingReel) {
        return;
    }

    m_pendingReel = true;
    LOG_INFO("[AutoFishing] Server-confirmed bite matched active bobber (runtime ID={}); reel queued.",
        event.runtimeId);
}

void AutoFishing::on_actor_entity_tick(ActorEntityTickEvent& event) {
    if (!enabled() || !event.localPlayer || !event.clientInstance || !event.tickingActor) return;

    if (event.tickingActor == event.localPlayer) {
        release_rod_use();
    }

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
            data.position = actor->getPosition();
            data.velocity = actor->getVelocity();
            data.lastUpdate = now;
            if (m_state == State::WaitingForBobber || m_state == State::SearchingForBobber ||
                m_state == State::WaitingForWorld || m_state == State::RecastDelay) {
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

    const auto worldActors = Actor::get_all(event.localPlayer);
    if (!g_traceWorldActorScan.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO("[AutoFishing][Trace] World actor scan is running ({} actor references).",
            worldActors.size());
    }

    for (Actor* worldActor : worldActors) {
        if (!worldActor || worldActor == event.localPlayer ||
            worldActor->getActorType() != ActorType::FishingHook) {
            continue;
        }

        const std::uint64_t runtimeId = worldActor->getRuntimeId();
        if (runtimeId == 0) continue;

        if (!g_traceWorldBobberScan.exchange(true, std::memory_order_relaxed)) {
            LOG_INFO("[AutoFishing][Trace] FishingHook found by world actor scan.");
        }

        auto& data = m_bobbers[runtimeId];
        data.position = worldActor->getPosition();
        data.velocity = worldActor->getVelocity();
        data.lastUpdate = now;
        if (m_state == State::WaitingForBobber || m_state == State::SearchingForBobber ||
            m_state == State::WaitingForWorld || m_state == State::RecastDelay) {
            try_adopt_bobber(now);
        }
    }

    switch (m_state) {
    case State::Disabled:
    case State::InputUnavailable:
    case State::Fishing:
        break;
    case State::WaitingForWorld: {
        try_adopt_bobber(now);
        const int delay = m_initialCastDelayMs ? m_initialCastDelayMs->value()
                                                : static_cast<int>(kInitialCastDelayMs);
        if (m_bobberRuntimeId == 0 && now >= m_stateSince &&
            now - m_stateSince >= static_cast<std::uint64_t>(delay)) {
            prepare_cast(now);
        }
        break;
    }
    case State::WaitingForBobber:
        try_adopt_bobber(now);
        if (m_state == State::WaitingForBobber && now - m_stateSince >= kBobberSpawnTimeoutMs) {
            if (m_castAttempts < 2) {
                m_state = State::RecastDelay;
                m_stateSince = now;
                LOG_WARN("[AutoFishing] No bobber detected; one automatic cast retry is queued.");
            } else {
                m_state = State::SearchingForBobber;
                m_stateSince = now;
                ui::notifications::warning(
                    "AutoFishing", "Automatic cast was not detected. Manual casts are still monitored.");
                LOG_WARN("[AutoFishing] No bobber detected after retry; continuing to monitor manual casts.");
            }
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
        try_adopt_bobber(now);
        const int delay = m_recastDelayMs ? m_recastDelayMs->value() : 400;
        if (m_state == State::RecastDelay &&
            now - m_stateSince >= static_cast<std::uint64_t>(delay)) {
            prepare_cast(now);
        }
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
    ImGui::TextDisabled("Bite event: %s",
        m_packetHooksReady.load(std::memory_order_acquire) ? "Ready" : "Unavailable");
    ImGui::TextDisabled("Hold a rod and aim at water before enabling.");
}
