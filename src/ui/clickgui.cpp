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

    constexpr ImVec4 kWindowBg{ 0.035f, 0.043f, 0.060f, 0.985f };
    constexpr ImVec4 kPanelBg{ 0.060f, 0.073f, 0.098f, 1.0f };
    constexpr ImVec4 kRaisedBg{ 0.085f, 0.103f, 0.136f, 1.0f };
    constexpr ImVec4 kBorder{ 0.16f, 0.19f, 0.25f, 0.9f };
    constexpr ImVec4 kText{ 0.91f, 0.93f, 0.97f, 1.0f };
    constexpr ImVec4 kMuted{ 0.52f, 0.58f, 0.68f, 1.0f };
    constexpr ImVec4 kAccent{ 0.32f, 0.72f, 0.88f, 1.0f };
    constexpr ImVec4 kAccentDim{ 0.12f, 0.28f, 0.36f, 1.0f };
    constexpr ImVec4 kWarm{ 0.96f, 0.64f, 0.34f, 1.0f };

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
        s.FrameBorderSize = 0.0f;

        auto* c = s.Colors;
        c[ImGuiCol_WindowBg] = kWindowBg;
        c[ImGuiCol_ChildBg] = kPanelBg;
        c[ImGuiCol_PopupBg] = kPanelBg;
        c[ImGuiCol_Border] = kBorder;
        c[ImGuiCol_Text] = kText;
        c[ImGuiCol_TextDisabled] = kMuted;
        c[ImGuiCol_FrameBg] = kRaisedBg;
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.16f, 0.21f, 1.0f);
        c[ImGuiCol_FrameBgActive] = kAccentDim;
        c[ImGuiCol_Button] = kRaisedBg;
        c[ImGuiCol_ButtonHovered] = ImVec4(0.14f, 0.20f, 0.27f, 1.0f);
        c[ImGuiCol_ButtonActive] = kAccentDim;
        c[ImGuiCol_Header] = kAccentDim;
        c[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.24f, 0.31f, 1.0f);
        c[ImGuiCol_HeaderActive] = kAccent;
        c[ImGuiCol_CheckMark] = kAccent;
        c[ImGuiCol_SliderGrab] = kAccent;
        c[ImGuiCol_SliderGrabActive] = kWarm;
        c[ImGuiCol_Separator] = kBorder;
        c[ImGuiCol_TitleBg] = kPanelBg;
        c[ImGuiCol_TitleBgActive] = kPanelBg;
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
        ImGui::TextColored(kAccent, "CONFIGURATION PROFILES");
        ImGui::TextColored(kMuted, "設定をファイル単位で保存・切り替えできます。");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(250.0f);
        ImGui::InputTextWithHint("##profile_name", "Profile name", g_ui.profile, sizeof(g_ui.profile));
        ImGui::SameLine();
        if (ImGui::Button("Save profile")) {
            if (g_ui.profile[0] != '\0') ConfigManager::instance().save_config(g_ui.profile);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load profile")) {
            if (g_ui.profile[0] != '\0') ConfigManager::instance().load_config(g_ui.profile);
        }
        ImGui::Separator();
        ImGui::Text("SAVED PROFILES");
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
            if (ImGui::SmallButton("Load")) ConfigManager::instance().load_config(profile);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) ConfigManager::instance().delete_config(profile);
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
        if (ImGui::Begin("JHONCLIENT     /     CONTROL STUDIO", &open, flags)) {
            // Header: identity and runtime status, deliberately styled as a single app dashboard.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaisedBg);
            ImGui::BeginChild("##topbar", ImVec2(0, 58), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(kAccent, "JH");
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("JHONCLIENT");
            ImGui::TextColored(kMuted, "BEDROCK EDITION  /  BUILD 26.60");
            ImGui::EndGroup();
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 205.0f);
            ImGui::TextColored(kWarm, "●  LIVE SESSION");
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            const float bodyHeight = ImGui::GetContentRegionAvail().y - 28.0f;
            const float sideWidth = 176.0f;
            const float listWidth = 292.0f;

            // Left navigation rail.
            ImGui::BeginChild("##navigation", ImVec2(sideWidth, bodyHeight), true);
            ImGui::TextColored(kMuted, "WORKSPACE");
            ImGui::Spacing();
            for (const auto& category : categories) {
                const bool active = g_ui.category == category;
                if (category_button(category, active)) {
                    g_ui.category = category;
                    g_ui.selected = nullptr;
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            if (category_button("Configs", g_ui.category == "Configs")) {
                g_ui.category = "Configs";
                g_ui.selected = nullptr;
            }
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 53.0f);
            ImGui::TextColored(kMuted, "INSERT  /  TOGGLE");
            ImGui::EndChild();

            ImGui::SameLine();

            // Middle index: searchable module list.
            ImGui::BeginChild("##module_index", ImVec2(listWidth, bodyHeight), true);
            if (g_ui.category == "Configs") {
                ImGui::TextColored(kAccent, "PROFILES");
                ImGui::TextColored(kMuted, "保存済み設定");
            } else {
                ImGui::TextColored(kAccent, "MODULE INDEX");
                const auto modules = modules_for_category(g_ui.category);
                ImGui::TextColored(kMuted, "%zu modules in %s", modules.size(), g_ui.category.c_str());
                ImGui::Spacing();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##module_search", "Filter modules...", g_ui.search, sizeof(g_ui.search));
                ImGui::Spacing();

                for (Module* module : modules) {
                    if (!module) continue;
                    if (g_ui.search[0] != '\0' && std::string(module->name()).find(g_ui.search) == std::string::npos) continue;
                    ImGui::PushID(module);
                    const bool selected = g_ui.selected == module;
                    std::string row = module->name();
                    row += module->enabled() ? "     [ON]" : "     [OFF]";
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
            ImGui::BeginChild("##module_workspace", ImVec2(0, bodyHeight), true);
            if (g_ui.category == "Configs") {
                draw_profiles();
            } else if (g_ui.selected) {
                Module* module = g_ui.selected;
                ImGui::TextColored(kAccent, "MODULE / DETAIL");
                ImGui::Spacing();
                ImGui::Text("%s", module->name().c_str());
                if (!module->tooltip().empty()) ImGui::TextWrapped("%s", module->tooltip().c_str());
                ImGui::Spacing();

                const bool enabled = module->enabled();
                if (enabled) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.32f, 0.27f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.40f, 0.32f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.37f, 0.46f, 1.0f));
                }
                if (ImGui::Button(enabled ? "DISABLE MODULE" : "ENABLE MODULE", ImVec2(180.0f, 34.0f))) module->toggle();
                ImGui::PopStyleColor(2);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(kMuted, "INPUT");
                const bool binding = InputManager::instance().binding_module() == module;
                const std::string keyLabel = binding ? "Press a key  /  ESC = UNBOUND" :
                    (module->keybind() == 0 ? "Keybind: 未登録" : std::string("Keybind: ") + input::get_key_name(module->keybind()));
                if (ImGui::Button(keyLabel.c_str(), ImVec2(240.0f, 32.0f))) {
                    InputManager::instance().begin_binding(module);
                }
                bool hold = module->hold_to_enable();
                if (ImGui::Checkbox("Hold to enable", &hold)) module->set_hold_to_enable(hold);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(kMuted, "MODULE SETTINGS");
                if (module->has_settings()) {
                    module->on_settings_render();
                } else {
                    ImGui::TextColored(kMuted, "このモジュールに追加設定はありません。");
                }
            } else {
                ImGui::TextColored(kAccent, "SELECT A MODULE");
                ImGui::Spacing();
                ImGui::TextWrapped("左側の一覧からモジュールを選択すると、ここに設定とキーバインドが表示されます。");
                ImGui::Spacing();
                ImGui::TextColored(kMuted, "JHONCLIENT / PERSONAL CONTROL ENVIRONMENT");
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::TextColored(kMuted, "ESC clears the keybind while listening  •  ESC closes the menu otherwise");
        }
        ImGui::End();

        if (!open) dx12_hook::set_menu_open(false);
    }
}
