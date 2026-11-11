#pragma once

#include "modules/module.h"
#include "config/setting.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class SkinStealer final : public Module {
public:
    SkinStealer();

    void on_enable() override;
    void on_disable() override;
    void on_imgui_render() override;
    void on_settings_render() override;

private:
    void update_aimed_player(class LocalPlayer* localPlayer, class ClientInstance* clientInstance);
    void handle_key_event(struct KeyEvent& event);
    void save_aimed_player();

    config::BoolSetting* m_saveWithMiddleMouse = nullptr;
    config::IntSetting* m_saveKeyCode = nullptr;

    std::atomic_bool m_waitingForSaveKey{ false };
    std::mutex m_aimedPlayerMutex;
    std::string m_aimedPlayerUuid;
    std::string m_aimedPlayerName;

    std::uint64_t m_actorTickListener = 0;
    std::uint64_t m_keyListener = 0;
};
