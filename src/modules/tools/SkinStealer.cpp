#include "modules/tools/SkinStealer.h"

#include "skin/skin_capture_manager.h"
#include "utils/logger.h"
#include "input/input_manager.h"
#include "input/key_names.h"
#include "events/event_bus.h"
#include "events/key_event.h"
#include "events/actor_tick_event.h"
#include "minecraft/sdk/actor/actor.h"
#include "minecraft/sdk/actor/local_player.h"
#include "minecraft/sdk/client/client_instance.h"

#include <Windows.h>
#include <imgui.h>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

bool ray_intersects_aabb(
    const math::Vec3& origin,
    const math::Vec3& direction,
    const AABBShapeComponent& bounds,
    float maxDistance,
    float& hitDistance) {

    const math::Vec3 minimum{
        std::min(bounds.mMin.x, bounds.mMax.x),
        std::min(bounds.mMin.y, bounds.mMax.y),
        std::min(bounds.mMin.z, bounds.mMax.z)
    };
    const math::Vec3 maximum{
        std::max(bounds.mMin.x, bounds.mMax.x),
        std::max(bounds.mMin.y, bounds.mMax.y),
        std::max(bounds.mMin.z, bounds.mMax.z)
    };

    float nearDistance = 0.0f;
    float farDistance = maxDistance;

    const auto intersectAxis = [&](float rayOrigin, float rayDirection, float minValue, float maxValue) {
        if (std::abs(rayDirection) < 0.00001f) {
            return rayOrigin >= minValue && rayOrigin <= maxValue;
        }

        const float inverse = 1.0f / rayDirection;
        float first = (minValue - rayOrigin) * inverse;
        float second = (maxValue - rayOrigin) * inverse;
        if (first > second) std::swap(first, second);

        nearDistance = std::max(nearDistance, first);
        farDistance = std::min(farDistance, second);
        return nearDistance <= farDistance;
    };

    if (!intersectAxis(origin.x, direction.x, minimum.x, maximum.x) ||
        !intersectAxis(origin.y, direction.y, minimum.y, maximum.y) ||
        !intersectAxis(origin.z, direction.z, minimum.z, maximum.z)) {
        return false;
    }

    hitDistance = nearDistance;
    return farDistance >= 0.0f && nearDistance <= maxDistance;
}

} // namespace

SkinStealer::SkinStealer()
    : Module("Tools", "SkinStealer",
        "Save a skin by aiming at a player with the target-save key, or by selecting their gamertag from the current world roster.") {

    m_saveWithMiddleMouse = add_setting<config::BoolSetting>(
        "Also allow middle-click save",
        "Optional shortcut: save the player under the crosshair with the middle mouse button.",
        true
    );

    // Stored in profiles, but rendered as a key-capture button below rather than a numeric slider.
    m_saveKeyCode = add_setting<config::IntSetting>(
        "Save key code",
        "Virtual-key code used to save the player under the crosshair.",
        'K', 1, 255,
        [] { return false; }
    );

    // SkinStealer's module keybind is reserved for saving, not toggling the module.
    set_keybind(0);
    set_hold_to_enable(false);
}

void SkinStealer::on_enable() {
    if (!minecraft::skin::initialize()) {
        LOG_ERROR("[SkinStealer] Could not initialize packet hooks; the module will remain disabled.");
        set_enabled(false);
        return;
    }

    minecraft::skin::set_capture_enabled(true);

    m_actorTickListener = EventBus::instance().subscribe<ActorTickEvent>(
        [this](ActorTickEvent& event) {
            update_aimed_player(event.localPlayer, event.clientInstance);
        });

    m_keyListener = EventBus::instance().subscribe<KeyEvent>(
        [this](KeyEvent& event) {
            handle_key_event(event);
        });

    LOG_INFO("[SkinStealer] Skin packet capture enabled. Aim at a player and press {} to save.",
        input::get_key_name(m_saveKeyCode ? m_saveKeyCode->value() : 'K'));
}

void SkinStealer::on_disable() {
    minecraft::skin::set_capture_enabled(false);

    if (m_actorTickListener != 0) {
        EventBus::instance().unsubscribe(m_actorTickListener);
        m_actorTickListener = 0;
    }
    if (m_keyListener != 0) {
        EventBus::instance().unsubscribe(m_keyListener);
        m_keyListener = 0;
    }

    {
        std::lock_guard lock(m_aimedPlayerMutex);
        m_aimedPlayerUuid.clear();
        m_aimedPlayerName.clear();
    }
    m_waitingForSaveKey.store(false, std::memory_order_release);

    LOG_INFO("[SkinStealer] Skin packet capture paused. Saved snapshots are kept.");
}

void SkinStealer::update_aimed_player(LocalPlayer* localPlayer, ClientInstance* clientInstance) {
    if (!localPlayer || !clientInstance) return;

    const auto players = minecraft::skin::online_players();
    if (players.empty()) {
        std::lock_guard lock(m_aimedPlayerMutex);
        m_aimedPlayerUuid.clear();
        m_aimedPlayerName.clear();
        return;
    }

    std::unordered_map<std::uint64_t, std::pair<std::string, std::string>> playerByActorId;
    playerByActorId.reserve(players.size());
    for (const auto& player : players) {
        if (player.actor_unique_id == 0 || !player.has_skin) continue;
        playerByActorId.insert_or_assign(
            player.actor_unique_id,
            std::make_pair(player.uuid, player.player_name));
    }

    if (playerByActorId.empty()) {
        std::lock_guard lock(m_aimedPlayerMutex);
        m_aimedPlayerUuid.clear();
        m_aimedPlayerName.clear();
        return;
    }

    LevelRenderer* levelRenderer = clientInstance->getLevelRenderer();
    LevelRendererPlayer* renderPlayer = levelRenderer ? levelRenderer->getLevelRendererPlayer() : nullptr;
    if (!renderPlayer) return;

    const math::Vec3 origin = renderPlayer->getOrigin();
    const math::Vec3 direction = math::get_direction(localPlayer->getRotation()).normalized();
    if (direction.length_sq() < 0.5f) return;

    constexpr float kTargetRange = 8.0f;
    float closestDistance = kTargetRange;
    std::string aimedUuid;
    std::string aimedName;

    const auto actors = Actor::get_all(localPlayer);
    for (Actor* actor : actors) {
        if (!actor || actor == localPlayer || !actor->isPlayer() || !actor->isAlive()) continue;

        const auto found = playerByActorId.find(actor->getUniqueId());
        if (found == playerByActorId.end()) continue;

        const AABBShapeComponent* bounds = actor->getAABB();
        if (!bounds) continue;

        float hitDistance = 0.0f;
        if (ray_intersects_aabb(origin, direction, *bounds, closestDistance, hitDistance) &&
            hitDistance < closestDistance) {
            closestDistance = hitDistance;
            aimedUuid = found->second.first;
            aimedName = found->second.second;
        }
    }

    std::lock_guard lock(m_aimedPlayerMutex);
    m_aimedPlayerUuid = std::move(aimedUuid);
    m_aimedPlayerName = std::move(aimedName);
}

void SkinStealer::handle_key_event(KeyEvent& event) {
    if (event.action != KeyAction::Press) return;
    if (m_waitingForSaveKey.load(std::memory_order_acquire)) return;

    if (InputManager::instance().is_binding() ||
        ClientInstance::isClientUiOpen() ||
        ClientInstance::isGameUiOpen()) {
        return;
    }

    const bool keyPressed = !event.isMouse && m_saveKeyCode &&
        event.key == m_saveKeyCode->value();
    const bool middlePressed = event.isMouse && event.key == VK_MBUTTON &&
        m_saveWithMiddleMouse && m_saveWithMiddleMouse->value();

    if (keyPressed || middlePressed) {
        save_aimed_player();
    }
}

void SkinStealer::save_aimed_player() {
    std::string uuid;
    std::string playerName;
    {
        std::lock_guard lock(m_aimedPlayerMutex);
        uuid = m_aimedPlayerUuid;
        playerName = m_aimedPlayerName;
    }

    if (uuid.empty()) {
        LOG_WARN("[SkinStealer] No player is under the crosshair. Aim directly at a player first.");
        return;
    }

    if (!minecraft::skin::save_player_skin(uuid)) {
        LOG_WARN("[SkinStealer] No complete skin data is available for {} yet.", playerName);
    }
}

void SkinStealer::on_imgui_render() {
    // Target selection is updated on ActorTickEvent so the render hook never reads live actor memory.
}

void SkinStealer::on_settings_render() {
    // The overlay consumes keyboard messages before the module event bus sees
    // them. Poll the Win32 key edge here while the key-capture control is active.
    if (m_waitingForSaveKey.load(std::memory_order_acquire) && m_saveKeyCode) {
        constexpr int ignoredKeys[] = {
            VK_LBUTTON, VK_RBUTTON, VK_CANCEL, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2,
            VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
            VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN, VK_ESCAPE, VK_INSERT
        };
        for (int key = 1; key < 256; ++key) {
            if (std::find(std::begin(ignoredKeys), std::end(ignoredKeys), key) != std::end(ignoredKeys)) {
                continue;
            }
            if ((GetAsyncKeyState(key) & 0x8001) != 0) {
                m_saveKeyCode->set_value(key);
                m_waitingForSaveKey.store(false, std::memory_order_release);
                LOG_INFO("[SkinStealer] Target save key changed to {}.", input::get_key_name(key));
                break;
            }
        }
    }

    Module::on_settings_render();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("AIM AT PLAYER - QUICK SAVE");

    std::string aimedName;
    {
        std::lock_guard lock(m_aimedPlayerMutex);
        aimedName = m_aimedPlayerName;
    }

    ImGui::Text("Current target: %s", aimedName.empty() ? "No player in crosshair" : aimedName.c_str());
    if (m_waitingForSaveKey.load(std::memory_order_acquire)) {
        ImGui::TextDisabled("Press any keyboard key to assign it.");
        if (ImGui::Button("Cancel key capture", ImVec2(170.0f, 30.0f))) {
            m_waitingForSaveKey.store(false, std::memory_order_release);
        }
    } else {
        const std::string keyLabel = std::string("Target save key: ") +
            input::get_key_name(m_saveKeyCode ? m_saveKeyCode->value() : 'K');
        if (ImGui::Button(keyLabel.c_str(), ImVec2(210.0f, 30.0f))) {
            m_waitingForSaveKey.store(true, std::memory_order_release);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Aim directly at a player and press this key to save a snapshot.");
    ImGui::TextDisabled("Enable/Disable above controls the module; this key only saves.");
    ImGui::TextDisabled("Middle mouse can also save the aimed player when enabled below.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("SAVE BY GAMERTAG");

    const auto players = minecraft::skin::online_players();
    static char gamertagSearch[96]{};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##skin_gamertag_search",
        "Search a gamertag in the current world...",
        gamertagSearch,
        sizeof(gamertagSearch));

    std::string needle(gamertagSearch);
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    std::size_t visiblePlayers = 0;
    for (const auto& player : players) {
        std::string name = player.player_name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (needle.empty() || name.find(needle) != std::string::npos) {
            ++visiblePlayers;
        }
    }

    ImGui::TextDisabled(
        "%zu matching gamertags / %zu players received from this world.",
        visiblePlayers, players.size());

    ImGui::BeginChild(
        "##skin_gamertag_players",
        ImVec2(0.0f, 184.0f),
        true,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);

    if (players.empty()) {
        ImGui::TextWrapped(
            "No gamertags have been received yet. Enable SkinStealer before joining the world.");
    } else if (visiblePlayers == 0) {
        ImGui::TextDisabled("No gamertags match this search.");
    } else {
        for (const auto& player : players) {
            std::string nameLower = player.player_name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (!needle.empty() && nameLower.find(needle) == std::string::npos) continue;

            ImGui::PushID(player.uuid.c_str());
            ImGui::Text("Gamertag: %s", player.player_name.c_str());
            ImGui::SameLine();
            if (player.has_skin) {
                ImGui::TextDisabled("%ux%u", player.width, player.height);
                ImGui::SameLine();
                if (ImGui::SmallButton("Save snapshot")) {
                    if (!minecraft::skin::save_player_skin(player.uuid)) {
                        LOG_WARN("[SkinStealer] Skin data for gamertag {} is not ready to save.",
                            player.player_name);
                    }
                }
            } else {
                ImGui::TextDisabled("Skin data pending");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("SAVED SKIN SNAPSHOTS");

    const auto skins = minecraft::skin::captured_skins();
    ImGui::Text("Saved snapshots: %zu", skins.size());

    if (ImGui::Button("OPEN SKIN LIBRARY FOLDER", ImVec2(240.0f, 32.0f))) {
        minecraft::skin::open_skins_directory();
    }
    ImGui::TextDisabled("Folder: %s", minecraft::skin::skins_directory().string().c_str());

    ImGui::BeginChild("##skin_saved_list", ImVec2(0.0f, 120.0f), true);
    if (skins.empty()) {
        ImGui::TextDisabled("No saved skins yet.");
    } else {
        const std::size_t first = skins.size() > 8 ? skins.size() - 8 : 0;
        for (std::size_t index = first; index < skins.size(); ++index) {
            const auto& skin = skins[index];
            ImGui::BulletText("%s  #%03u  /  %ux%u%s%s",
                skin.player_name.c_str(),
                skin.index,
                skin.width,
                skin.height,
                skin.has_cape ? "  /  cape" : "",
                skin.has_geometry ? "  /  geometry" : "");
        }
    }
    ImGui::EndChild();
}
