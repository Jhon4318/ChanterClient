#include "ui/clickgui.h"

#include "input/input_manager.h"
#include "input/key_names.h"
#include "modules/module_manager.h"
#include "config/config_manager.h"
#include "render/dx12/dx12_hook.h"

#include <imgui.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {
    struct UiState {
        char search[128]{};
        char profile[64] = "default";
        std::string category = "Visual";
        Module* selected = nullptr;
        bool initialized = false;
    };

    UiState g_ui;

    std::string display_category(const std::string& category) {
        if (category == "Visual") return "表示";
        if (category == "Tools") return "ツール";
        if (category == "Configs") return "プロファイル";
        return category;
    }

    std::string display_module_name(const Module* module) {
        if (module == nullptr) return {};
        if (module->name() == "ContainerTransfer") return "コンテナ一括転送 (ContainerTransfer)";
        if (module->name() == "SkinStealer") return "スキン保存 (SkinStealer)";
        if (module->name() == "Watermark") return "画面ロゴ (Watermark)";
        return module->name();
    }

    // Light theme for the entire client operation panel.
    const ImVec4 kWindowBg{ 0.965f, 0.973f, 0.984f, 0.995f };
    const ImVec4 kPanelBg{ 1.000f, 1.000f, 1.000f, 1.000f };
    const ImVec4 kRaisedBg{ 0.940f, 0.953f, 0.973f, 1.000f };
    const ImVec4 kBorder{ 0.835f, 0.865f, 0.905f, 1.000f };
    const ImVec4 kText{ 0.120f, 0.155f, 0.205f, 1.000f };
    const ImVec4 kMuted{ 0.390f, 0.445f, 0.515f, 1.000f };
    const ImVec4 kAccent{ 0.145f, 0.390f, 0.690f, 1.000f };
    const ImVec4 kAccentDim{ 0.875f, 0.925f, 0.980f, 1.000f };
    const ImVec4 kWarm{ 0.690f, 0.390f, 0.120f, 1.000f };

    void apply_jhon_style() {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowPadding = ImVec2(16, 15);
        s.FramePadding = ImVec2(10, 7);
        s.ItemSpacing = ImVec2(9, 9);
        s.ItemInnerSpacing = ImVec2(7, 6);
        s.WindowRounding = 13.0f;
        s.ChildRounding = 10.0f;
        s.FrameRounding = 7.0f;
        s.PopupRounding = 8.0f;
        s.ScrollbarRounding = 8.0f;
        s.WindowBorderSize = 1.0f;
        s.ChildBorderSize = 1.0f;
        s.FrameBorderSize = 1.0f;

        auto* c = s.Colors;
        c[ImGuiCol_WindowBg] = kWindowBg;
        c[ImGuiCol_ChildBg] = kPanelBg;
        c[ImGuiCol_PopupBg] = kPanelBg;
        c[ImGuiCol_Border] = kBorder;
        c[ImGuiCol_Text] = kText;
        c[ImGuiCol_TextDisabled] = kMuted;
        c[ImGuiCol_FrameBg] = ImVec4(0.965f, 0.973f, 0.984f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.920f, 0.945f, 0.975f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.870f, 0.920f, 0.975f, 1.0f);
        c[ImGuiCol_Button] = kRaisedBg;
        c[ImGuiCol_ButtonHovered] = ImVec4(0.885f, 0.930f, 0.980f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.825f, 0.895f, 0.970f, 1.0f);
        c[ImGuiCol_Header] = kAccentDim;
        c[ImGuiCol_HeaderHovered] = ImVec4(0.825f, 0.895f, 0.970f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.760f, 0.855f, 0.955f, 1.0f);
        c[ImGuiCol_CheckMark] = kAccent;
        c[ImGuiCol_SliderGrab] = kAccent;
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.100f, 0.315f, 0.590f, 1.0f);
        c[ImGuiCol_Separator] = kBorder;
        c[ImGuiCol_TitleBg] = kPanelBg;
        c[ImGuiCol_TitleBgActive] = kPanelBg;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.940f, 0.950f, 0.965f, 1.0f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.740f, 0.785f, 0.840f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.640f, 0.705f, 0.790f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.500f, 0.610f, 0.760f, 1.0f);
        c[ImGuiCol_TextSelectedBg] = ImVec4(0.600f, 0.760f, 0.940f, 0.45f);
        c[ImGuiCol_NavHighlight] = kAccent;
    }

    bool category_button(const std::string& label, bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentDim);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDim);
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        }
        const bool clicked = ImGui::Button(label.c_str(), ImVec2(-1.0f, 36.0f));
        if (active) ImGui::PopStyleColor(4);
        return clicked;
    }

    std::vector<Module*> modules_for_category(const std::string& category) {
        auto modules = ModuleManager::instance().modules_in_category(category);
        modules.erase(std::remove(modules.begin(), modules.end(), nullptr), modules.end());
        return modules;
    }

    void draw_profiles() {
        ImGui::TextColored(kAccent, "設定プロファイル");
        ImGui::TextColored(kMuted, "設定をファイル単位で保存・切り替えできます。");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(250.0f);
        ImGui::InputTextWithHint("##profile_name", "プロファイル名", g_ui.profile, sizeof(g_ui.profile));
        ImGui::SameLine();
        if (ImGui::Button("保存")) {
            if (g_ui.profile[0] != '\0') ConfigManager::instance().save_config(g_ui.profile);
        }
        ImGui::SameLine();
        if (ImGui::Button("読み込み")) {
            if (g_ui.profile[0] != '\0') ConfigManager::instance().load_config(g_ui.profile);
        }
        ImGui::Separator();
        ImGui::Text("保存済みプロファイル");
        const auto profiles = ConfigManager::instance().get_available_configs();
        if (profiles.empty()) {
            ImGui::TextColored(kMuted, "保存済みプロファイルはありません。");
        }
        for (const auto& profile : profiles) {
            ImGui::PushID(profile.c_str());
            if (ImGui::Selectable(profile.c_str(), profile == g_ui.profile)) {
                strncpy_s(g_ui.profile, profile.c_str(), sizeof(g_ui.profile) - 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("読込")) ConfigManager::instance().load_config(profile);
            ImGui::SameLine();
            if (ImGui::SmallButton("削除")) ConfigManager::instance().delete_config(profile);
            ImGui::PopID();
        }
    }
}

namespace clickgui {
    void render() {
        if (!g_ui.initialized) {
            apply_jhon_style();
            g_ui.initialized = true;
        }

        ImGuiIO& io = ImGui::GetIO();
        const auto categories = ModuleManager::instance().categories();
        const auto allModules = ModuleManager::instance().modules();

        if (g_ui.category != "Configs") {
            const bool categoryExists = std::find(categories.begin(), categories.end(), g_ui.category) != categories.end();
            if (!categoryExists) g_ui.category = categories.empty() ? "Visual" : categories.front();
        }
        if (g_ui.selected) {
            const bool exists = std::find(allModules.begin(), allModules.end(), g_ui.selected) != allModules.end();
            if (!exists) g_ui.selected = nullptr;
        }

        ImGui::SetNextWindowSize(ImVec2(1010.0f, 650.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

        bool open = true;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("CHANTERCLIENT     /     操作パネル", &open, flags)) {
            // Header: identity and runtime status, deliberately styled as a single app dashboard.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaisedBg);
            ImGui::BeginChild("##topbar", ImVec2(0, 58), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(kAccent, "CC");
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("CHANTERCLIENT");
            ImGui::TextColored(kMuted, "Minecraft Bedrock Edition 1.26.60");
            ImGui::EndGroup();
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 205.0f);
            ImGui::TextColored(kWarm, "●  セッション中");
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            const float bodyHeight = ImGui::GetContentRegionAvail().y - 28.0f;
            const float sideWidth = 176.0f;
            const float listWidth = 292.0f;

            // Left navigation rail.
            ImGui::BeginChild("##navigation", ImVec2(sideWidth, bodyHeight), true);
            ImGui::TextColored(kMuted, "メニュー");
            ImGui::Spacing();
            for (const auto& category : categories) {
                if (category == "Configs") continue;
                const bool active = g_ui.category == category;
                if (category_button(display_category(category), active)) {
                    g_ui.category = category;
                    g_ui.selected = nullptr;
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            if (category_button("プロファイル", g_ui.category == "Configs")) {
                g_ui.category = "Configs";
                g_ui.selected = nullptr;
            }
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 53.0f);
            ImGui::TextColored(kMuted, "INSERTキー：操作パネルの開閉");
            ImGui::EndChild();

            ImGui::SameLine();

            // Middle index: searchable module list.
            ImGui::BeginChild("##module_index", ImVec2(listWidth, bodyHeight), true);
            if (g_ui.category == "Configs") {
                ImGui::TextColored(kAccent, "プロファイル");
                ImGui::TextColored(kMuted, "保存済み設定");
            } else {
                ImGui::TextColored(kAccent, "機能一覧");
                const auto modules = modules_for_category(g_ui.category);
                const std::string categoryName = display_category(g_ui.category);
                ImGui::TextColored(kMuted, "%zu 個の機能 / %s", modules.size(), categoryName.c_str());
                ImGui::Spacing();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##module_search", "機能を検索...", g_ui.search, sizeof(g_ui.search));
                ImGui::Spacing();

                for (Module* module : modules) {
                    if (!module) continue;
                    const std::string moduleLabel = display_module_name(module);
                    if (g_ui.search[0] != '\0' &&
                        moduleLabel.find(g_ui.search) == std::string::npos &&
                        std::string(module->name()).find(g_ui.search) == std::string::npos) continue;
                    ImGui::PushID(module);
                    const bool selected = g_ui.selected == module;
                    std::string row = moduleLabel;
                    row += module->enabled() ? "     [有効]" : "     [無効]";
                    if (ImGui::Selectable(row.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 42))) {
                        g_ui.selected = module;
                    }
                    if (ImGui::IsItemHovered() && !module->tooltip().empty()) ImGui::SetTooltip("%s", module->tooltip().c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // Right workspace: selected module details and settings.
            ImGui::BeginChild("##module_workspace", ImVec2(0, bodyHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (g_ui.category == "Configs") {
                draw_profiles();
            } else if (g_ui.selected) {
                Module* module = g_ui.selected;
                ImGui::TextColored(kAccent, "機能の詳細");
                ImGui::Spacing();
                const std::string moduleLabel = display_module_name(module);
                ImGui::Text("%s", moduleLabel.c_str());
                if (!module->tooltip().empty()) ImGui::TextWrapped("%s", module->tooltip().c_str());
                ImGui::Spacing();

                const bool enabled = module->enabled();
                if (enabled) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.865f, 0.950f, 0.900f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.805f, 0.925f, 0.855f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.105f, 0.390f, 0.235f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.825f, 0.895f, 0.970f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                }
                if (ImGui::Button(enabled ? "機能を無効化" : "機能を有効化", ImVec2(180.0f, 34.0f))) module->toggle();
                ImGui::PopStyleColor(3);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(kMuted, "キー設定");
                const bool binding = InputManager::instance().binding_module() == module;
                const std::string keyLabel = binding ? "キーを押してください  /  DELで割り当て解除" :
                    (module->keybind() == 0 ? "割り当てキー：未設定" : std::string("割り当てキー：") + input::get_key_name(module->keybind()));
                if (ImGui::Button(keyLabel.c_str(), ImVec2(240.0f, 32.0f))) {
                    InputManager::instance().begin_binding(module);
                }
                bool hold = module->hold_to_enable();
                if (ImGui::Checkbox("キーを押している間だけ有効", &hold)) module->set_hold_to_enable(hold);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(kMuted, "機能の設定");
                if (module->has_settings()) {
                    module->on_settings_render();
                } else {
                    ImGui::TextColored(kMuted, "このモジュールに追加設定はありません。");
                }
            } else {
                ImGui::TextColored(kAccent, "機能を選択してください");
                ImGui::Spacing();
                ImGui::TextWrapped("左側の機能一覧から選択すると、ここに説明・有効化ボタン・キー設定・各種設定が表示されます。");
                ImGui::Spacing();
                ImGui::TextColored(kMuted, "CHANTERCLIENT / Minecraft Bedrock 操作環境");
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::TextColored(kMuted, "DEL / BACKSPACE：キー割り当て解除  ・  ESC：キー設定をキャンセル、またはパネルを閉じる");
        }
        ImGui::End();

        if (!open) dx12_hook::set_menu_open(false);
    }
}
