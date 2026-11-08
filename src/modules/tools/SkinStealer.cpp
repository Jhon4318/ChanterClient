#include "modules/tools/SkinStealer.h"

#include "skin/skin_capture_manager.h"
#include "utils/logger.h"

#include <imgui.h>

SkinStealer::SkinStealer()
    : Module("Tools", "SkinStealer",
             "接続中のプレイヤーが送信したスキンデータを受信時にPNG保存します。") {
    m_onlyTarget = add_setting<config::BoolSetting>(
        "UUIDで対象を絞り込む",
        "有効にすると、指定したUUIDのスキンだけを保存します。",
        false
    );
    m_targetUuid = add_setting<config::StringSetting>(
        "対象プレイヤーUUID",
        "ハイフンあり・なしのどちらでも入力できます。",
        ""
    );
}

void SkinStealer::on_enable() {
    // Packet vtable hooks are deliberately opt-in. Installing them during
    // client startup caused a crash while Bedrock was processing world-join packets.
    if (!minecraft::skin::initialize()) {
        LOG_ERROR("[SkinStealer] Could not initialize packet hooks; the module will remain disabled.");
        set_enabled(false);
        return;
    }

    minecraft::skin::set_capture_enabled(true);
    LOG_INFO("[SkinStealer] Packet capture enabled.");
}

void SkinStealer::on_disable() {
    minecraft::skin::set_capture_enabled(false);
    LOG_INFO("[SkinStealer] Packet capture paused. Installed hooks remain dormant until client shutdown.");
}

void SkinStealer::on_imgui_render() {
    if (!enabled()) return;
    minecraft::skin::set_target_filter(
        m_onlyTarget && m_onlyTarget->value(),
        m_targetUuid ? m_targetUuid->value() : std::string{}
    );
}

void SkinStealer::on_settings_render() {
    Module::on_settings_render();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("SKIN LIBRARY");
    const auto skins = minecraft::skin::captured_skins();
    ImGui::Text("Saved skins: %zu", skins.size());
    if (ImGui::Button("OPEN SKIN LIBRARY FOLDER", ImVec2(240.0f, 32.0f))) {
        minecraft::skin::open_skins_directory();
    }
    ImGui::TextDisabled("保存先: %%APPDATA%%\\ChanterClient\\skins");
    if (skins.empty()) {
        ImGui::TextDisabled("まだ保存されたスキンはありません。");
    } else {
        for (const auto& skin : skins) {
            ImGui::BulletText("%s  /  %ux%u", skin.player_name.c_str(), skin.width, skin.height);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", skin.uuid.c_str());
        }
    }
}
