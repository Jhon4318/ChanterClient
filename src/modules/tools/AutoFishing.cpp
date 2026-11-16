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
constexpr std::uint64_t kBobberSpawnTimeoutMs = 2500;
constexpr std::uint64_t kRodUseMinimumHoldMs = 150;
constexpr std::uint64_t kBobberReturnTimeoutMs = 2000;
constexpr std::uint64_t kCastRetryDelayMs = 500;
constexpr std::uint64_t kBobberReturnMinimumObserveMs = 200;
constexpr int kBobberReturnMissingTicksRequired = 2;
constexpr std::uint64_t kStaleBobberTimeoutMs = 30000;
constexpr std::uint64_t kInitialCastDelayMs = 600;
// Actor scanning has no owner check, so only accept hooks near the local player's cast origin.
// A fishing hook is created beside the player; a large world radius can claim another player's bobber.
constexpr float kBobberSearchRange = 4.0f;
constexpr float kBobberSearchRangeSq = kBobberSearchRange * kBobberSearchRange;

std::atomic_bool g_traceLocalTick{false};
std::atomic_bool g_traceActorBobberTick{false};
std::atomic_bool g_traceWorldActorScan{false};
std::atomic_bool g_traceWorldBobberScan{false};
std::atomic_bool g_traceIgnoredDistantBobber{false};
}

AutoFishing::AutoFishing()
    : Module("Tools", "AutoFishing", "Reel in and recast when a fish bites.") {
    m_recastDelayMs = add_setting<config::IntSetting>(
        "Recast delay", "Delay in milliseconds after the active bobber is retrieved.", 400, 0, 10000);
    m_initialCastDelayMs = add_setting<config::IntSetting>(
        "Initial cast delay", "Delay in milliseconds after Minecraft is focused and menus are closed.", 600, 0, 10000);
}

void AutoFishing::on_enable() {
    g_traceLocalTick.store(false, std::memory_order_relaxed);
    g_traceActorBobberTick.store(false, std::memory_order_relaxed);
    g_traceWorldActorScan.store(false, std::memory_order_relaxed);
    g_traceWorldBobberScan.store(false, std::memory_order_relaxed);
    g_traceIgnoredDistantBobber.store(false, std::memory_order_relaxed);

    {
        std::lock_guard lock(m_mutex);
        m_state = State::WaitingForWorld;
        // Start the initial delay only after the game is focused and menus are closed.
        m_stateSince = 0;
        m_pauseStartedAt = 0;
        m_bobberRuntimeId = 0;
        m_castAttempts = 0;
        m_pendingReel = false;
        m_bobberReturnMissingTicks = 0;
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
    release_rod_use(true);

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
        // A player-tick callback may have queued a DOWN immediately before
        // disable() began. Release again while holding the state mutex so no
        // in-flight callback can leave the injected button pressed.
        release_rod_use(true);
        if (m_state == State::Fishing) {
            const std::uint64_t stoppedAt = GetTickCount64();
            const std::uint64_t elapsed = stoppedAt >= m_stateSince ? stoppedAt - m_stateSince : 0;
            LOG_INFO("[AutoFishing] Disabled while waiting for a bite ({}s, bobber ID={}).",
                elapsed / 1000, m_bobberRuntimeId);
        }
        m_state = State::Disabled;
        m_stateSince = 0;
        m_pauseStartedAt = 0;
        m_bobberRuntimeId = 0;
        m_castAttempts = 0;
        m_pendingReel = false;
        m_bobberReturnMissingTicks = 0;
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

bool AutoFishing::input_environment_ready() const {
    return game_is_focused() &&
        !ClientInstance::isClientUiOpen() &&
        !ClientInstance::isGameUiOpen();
}

bool AutoFishing::send_rod_use() {
    std::lock_guard inputLock(m_inputMutex);
    if (!input_environment_ready()) return false;

    if (m_rightClickReleasePending) {
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

    m_rightClickReleasePending = true;
    m_rightClickDownAt = GetTickCount64();
    LOG_INFO("[AutoFishing] Right-button down sent; holding for at least {} ms.",
        kRodUseMinimumHoldMs);
    return true;
}

void AutoFishing::release_rod_use(bool force) {
    std::lock_guard inputLock(m_inputMutex);
    if (!m_rightClickReleasePending) return;

    const std::uint64_t now = GetTickCount64();
    const std::uint64_t heldMs = now >= m_rightClickDownAt ? now - m_rightClickDownAt : 0;
    if (!force && heldMs < kRodUseMinimumHoldMs) return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    const UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        LOG_WARN("[AutoFishing] SendInput did not insert right-button up (sent={}, error={}); will retry.",
            sent, GetLastError());
        return;
    }

    m_rightClickReleasePending = false;
    m_rightClickDownAt = 0;
    LOG_INFO("[AutoFishing] Right-button up sent after {} ms{}.", heldMs,
        force ? " (forced during disable)" : "");
}

void AutoFishing::prepare_cast(std::uint64_t now) {
    // Do not consume a cast attempt while the game is unfocused or a menu is open.
    if (!input_environment_ready()) return;

    m_bobberRuntimeId = 0;
    m_pendingReel = false;
    m_bobberReturnMissingTicks = 0;
    m_previousBobberIds.clear();
    for (const auto& [id, data] : m_bobbers) {
        if (now >= data.lastUpdate && now - data.lastUpdate <= kStaleBobberTimeoutMs) {
            m_previousBobberIds.insert(id);
        }
    }

    ++m_castAttempts;
    LOG_INFO("[AutoFishing] Automatic cast attempt {}/2.", m_castAttempts);
    if (!send_rod_use()) {
        m_stateSince = now;
        if (m_castAttempts < 2) {
            m_state = State::RetryCastDelay;
            LOG_WARN("[AutoFishing] Cast input failed; retry queued in {} ms.", kCastRetryDelayMs);
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
    m_bobberReturnMissingTicks = 0;
}

void AutoFishing::try_adopt_bobber(std::uint64_t now) {
    if (m_bobberRuntimeId != 0 || !m_havePlayerPosition) return;

    std::uint64_t bestId = 0;
    float bestDistanceSq = kBobberSearchRangeSq;
    for (const auto& [id, data] : m_bobbers) {
        if (m_previousBobberIds.contains(id)) continue;
        if (now < data.lastUpdate || now - data.lastUpdate > kStaleBobberTimeoutMs) continue;
        const float distanceSq = data.position.distance_sq(m_localPlayerPosition);
        if (distanceSq > kBobberSearchRangeSq) {
            if (!g_traceIgnoredDistantBobber.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO("[AutoFishing][Trace] Ignored distant FishingHook (runtime ID={}, distance={:.2f}, adoption limit={:.1f} blocks).",
                    id, std::sqrt(distanceSq), kBobberSearchRange);
            }
            continue;
        }
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

    const bool inputReady = input_environment_ready();
    if (event.tickingActor != event.localPlayer && !inputReady) return;

    std::lock_guard lock(m_mutex);
    if (!enabled()) return;
    if (!inputReady) {
        // If the click GUI was used to enable the module, restart the initial
        // delay after the user closes it and returns focus to Minecraft.
        if (m_state == State::WaitingForWorld) m_stateSince = 0;
        if (m_pauseStartedAt == 0) m_pauseStartedAt = GetTickCount64();
        return;
    }

    const std::uint64_t now = GetTickCount64();
    if (m_pauseStartedAt != 0) {
        const std::uint64_t pausedFor = now >= m_pauseStartedAt ? now - m_pauseStartedAt : 0;
        if (m_state != State::WaitingForWorld && m_stateSince != 0) {
            m_stateSince += pausedFor;
        }
        m_pauseStartedAt = 0;
        LOG_INFO("[AutoFishing] Resumed after {} ms without game input.", pausedFor);
    }
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
                m_state == State::RetryCastDelay) {
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

    bool activeBobberPresent = false;
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

        if (m_state == State::WaitingForBobberReturn && runtimeId == m_bobberRuntimeId) {
            activeBobberPresent = true;
        }

        auto& data = m_bobbers[runtimeId];
        data.position = worldActor->getPosition();
        data.velocity = worldActor->getVelocity();
        data.lastUpdate = now;
        if (m_state == State::WaitingForBobber || m_state == State::SearchingForBobber ||
            m_state == State::RetryCastDelay) {
            try_adopt_bobber(now);
        }
    }

    if (m_state == State::WaitingForBobberReturn) {
        if (activeBobberPresent) m_bobberReturnMissingTicks = 0;
        else ++m_bobberReturnMissingTicks;
    }

    switch (m_state) {
    case State::Disabled:
    case State::InputUnavailable:
    case State::Fishing:
        break;
    case State::WaitingForWorld: {
        // Existing hooks may belong to other players; only adopt hooks spawned
        // after this module has sent its own cast input.
        if (m_stateSince == 0) {
            m_stateSince = now;
            LOG_INFO("[AutoFishing] Game input is ready; initial cast delay started ({} ms).",
                m_initialCastDelayMs ? m_initialCastDelayMs->value() : static_cast<int>(kInitialCastDelayMs));
        }
        const int delay = m_initialCastDelayMs ? m_initialCastDelayMs->value()
                                                : static_cast<int>(kInitialCastDelayMs);
        if (now >= m_stateSince &&
            now - m_stateSince >= static_cast<std::uint64_t>(delay)) {
            prepare_cast(now);
        }
        break;
    }
    case State::WaitingForBobber:
        try_adopt_bobber(now);
        if (m_state == State::WaitingForBobber && now - m_stateSince >= kBobberSpawnTimeoutMs) {
            if (m_castAttempts < 2) {
                m_state = State::RetryCastDelay;
                m_stateSince = now;
                LOG_WARN("[AutoFishing] No bobber detected; retry queued in {} ms.", kCastRetryDelayMs);
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
    case State::WaitingForBobberReturn: {
        const std::uint64_t elapsed = now >= m_stateSince ? now - m_stateSince : 0;
        const bool bobberReturned = elapsed >= kBobberReturnMinimumObserveMs &&
            m_bobberReturnMissingTicks >= kBobberReturnMissingTicksRequired;
        const bool returnTimedOut = elapsed >= kBobberReturnTimeoutMs;
        if (bobberReturned || returnTimedOut) {
            const std::uint64_t returnedId = m_bobberRuntimeId;
            if (returnedId != 0) {
                m_previousBobberIds.insert(returnedId);
                if (bobberReturned) m_bobbers.erase(returnedId);
            }
            m_bobberRuntimeId = 0;
            m_bobberReturnMissingTicks = 0;
            m_state = State::RecastDelay;
            m_stateSince = now;
            const int delay = m_recastDelayMs ? m_recastDelayMs->value() : 400;
            if (bobberReturned) {
                LOG_INFO("[AutoFishing] Bobber disappeared; configured recast delay started ({} ms).", delay);
            } else {
                LOG_WARN("[AutoFishing] Bobber return was not observed within {} ms; starting recast delay ({} ms).",
                    kBobberReturnTimeoutMs, delay);
            }
        }
        break;
    }
    case State::RetryCastDelay:
        try_adopt_bobber(now);
        if (m_state == State::RetryCastDelay && now >= m_stateSince &&
            now - m_stateSince >= kCastRetryDelayMs) {
            prepare_cast(now);
        }
        break;
    case State::RecastDelay: {
        const int delay = m_recastDelayMs ? m_recastDelayMs->value() : 400;
        if (now >= m_stateSince &&
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
            m_bobberReturnMissingTicks = 0;
            LOG_INFO("[AutoFishing] Reeling input dispatched; waiting for the active bobber to disappear.");
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
    case State::RetryCastDelay: status = "Retrying cast"; break;
    case State::RecastDelay: status = "Recasting"; break;
    case State::InputUnavailable: status = "Input unavailable"; break;
    }

    ImGui::TextDisabled("Status: %s", status);
    ImGui::TextDisabled("Bite event: %s",
        m_packetHooksReady.load(std::memory_order_acquire) ? "Ready" : "Unavailable");
    ImGui::TextDisabled("Recast delay: %d ms", m_recastDelayMs ? m_recastDelayMs->value() : 400);
    ImGui::TextDisabled("Initial cast delay: %d ms", m_initialCastDelayMs ? m_initialCastDelayMs->value() : static_cast<int>(kInitialCastDelayMs));
    if (m_state == State::RecastDelay) {
        const std::uint64_t now = GetTickCount64();
        const int delay = m_recastDelayMs ? m_recastDelayMs->value() : 400;
        const std::uint64_t elapsed = now >= m_stateSince ? now - m_stateSince : 0;
        const std::uint64_t remaining = elapsed >= static_cast<std::uint64_t>(delay)
            ? 0 : static_cast<std::uint64_t>(delay) - elapsed;
        ImGui::TextDisabled("Next cast in: %llu ms", static_cast<unsigned long long>(remaining));
    }
    ImGui::TextDisabled("Hold a rod and aim at water before enabling.");
}
