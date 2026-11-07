#pragma once

#include "modules/module.h"
#include "config/setting.h"

class SkinStealer final : public Module {
public:
    SkinStealer();

    void on_enable() override;
    void on_disable() override;
    void on_imgui_render() override;
    void on_settings_render() override;

private:
    config::BoolSetting* m_onlyTarget = nullptr;
    config::StringSetting* m_targetUuid = nullptr;
};
