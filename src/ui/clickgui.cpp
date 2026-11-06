#include "ui/clickgui.h"
#include "ui/widgets/widgets.h"
#include "input/input_manager.h"
#include "input/key_names.h"
#include "modules/module_manager.h"
#include "config/config_manager.h"
#include "config/setting.h"

#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

    constexpr float CAT_WIDTH = 215.0f;
    constexpr float CAT_HEIGHT = 32.0f;
    constexpr float MOD_HEIGHT = 30.0f;
    constexpr float CAT_GAP = 18.0f;
    constexpr float CORNER_RADIUS = 8.0f;

    const ImColor kDarkHeader(18, 20, 24, 255);
    const ImColor kModOffColor(24, 26, 32, 255);
    const ImColor kModHoverColor(32, 35, 44, 255);
    const ImColor kSettingBgColor(18, 20, 24, 255);
    const ImColor kEnumSubBg(14, 16, 20, 255);
    const ImColor kTextWhite(255, 255, 255, 255);
    const ImColor kTextMuted(160, 170, 180, 255);
    const ImColor kTextDisabled(110, 120, 130, 255);

    const ImColor kEmeraldAccent(16, 185, 129, 255);  
    const ImColor kEmeraldDark(5, 150, 105, 255);      
    const ImColor kEmeraldLight(52, 211, 153, 255);    
    const ImColor kEmeraldGlow(16, 185, 129, 65);

    struct CategoryState {
        float x = 0.0f;
        float y = 0.0f;
        bool isDragging = false;
        bool isExtended = true;
        float extendAnim = 1.0f;
        float dragOffsetX = 0.0f;
        float dragOffsetY = 0.0f;
        float scrollEase = 0.0f;
        float yOffset = 0.0f;
    };

    static std::map<std::string, CategoryState> g_catStates;
    static bool g_initPositions = false;

    static config::Setting* g_activeDraggedSlider = nullptr;

    static config::ColorSetting* g_activeColorSetting = nullptr;
    static bool g_showColorPicker = false;

    static float g_configPanelHeight = 220.0f;

    static std::map<Module*, float> g_modToggleScale;
    static std::map<Module*, float> g_modSettingsAnim;
    static std::map<Module*, float> g_modHoldAnim;
    static std::map<Module*, bool>  g_modShowSettings;

    static std::map<config::Setting*, float> g_settingBoolScale;
    static std::map<config::Setting*, float> g_settingSliderEase;
    static std::map<config::Setting*, float> g_settingEnumSlide;
    static std::map<config::Setting*, bool>  g_settingEnumOpen;

    inline float Animate(float target, float current, float speed) {
        float dt = ImGui::GetIO().DeltaTime;
        return current + (target - current) * std::clamp(dt * speed, 0.0f, 1.0f);
    }

    inline bool IsMouseOver(const ImVec4& r) {
        ImVec2 m = ImGui::GetIO().MousePos;
        return m.x >= r.x && m.y >= r.y && m.x < r.z && m.y < r.w;
    }

    void FillRectangle(const ImVec4& pos, const ImColor& color, float alpha, float radius, ImDrawFlags flags, ImDrawList* dl) {
        if (pos.z <= pos.x || pos.w <= pos.y) return;
        dl->AddRectFilled(
            ImVec2(pos.x, pos.y), ImVec2(pos.z, pos.w),
            ImColor(color.Value.x, color.Value.y, color.Value.z, color.Value.w * alpha),
            radius, flags
        );
    }

    void FillRoundedGradientRectangle(const ImVec4& pos, const ImColor& col1, const ImColor& col2, float radius, float alpha, ImDrawFlags flags, ImDrawList* dl) {
        if (pos.z <= pos.x || pos.w <= pos.y) return;
        ImVec2 topLeft(pos.x, pos.y);
        ImVec2 bottomRight(pos.z, pos.w);

        ImU32 uCol1 = ImColor(col1.Value.x, col1.Value.y, col1.Value.z, alpha);
        ImU32 uCol2 = ImColor(col2.Value.x, col2.Value.y, col2.Value.z, alpha);

        int start = dl->VtxBuffer.Size;
        dl->AddRectFilled(topLeft, bottomRight, uCol1, radius, flags);
        int end = dl->VtxBuffer.Size;

        ImGui::ShadeVertsLinearColorGradientKeepAlpha(
            dl, start, end, topLeft, bottomRight,
            uCol1, uCol2
        );
    }

    void FillShadowCircle(const ImVec2& pos, float radius, const ImColor& color, float alpha, float thickness, ImDrawList* dl) {
        dl->AddShadowCircle(
            pos, radius,
            ImColor(color.Value.x, color.Value.y, color.Value.z, color.Value.w * alpha),
            thickness, ImVec2(0.0f, 0.0f), 0, 16
        );
    }

    void DrawCheckMark(const ImVec2& pos, float size, const ImColor& color, float alpha, float thickness, ImDrawList* dl) {
        ImVec2 end1(pos.x + (3.0f * size), pos.y + (3.0f * size));
        ImVec2 end2(pos.x + (7.0f * size), pos.y - (3.0f * size));
        ImU32 col = ImColor(color.Value.x, color.Value.y, color.Value.z, color.Value.w * alpha);
        dl->AddLine(pos, end1, col, thickness);
        dl->AddLine(end1, end2, col, thickness);
    }

    void render_config_panel(float catX, float startY, float catWidth, float visibleHeight, float yOffset) {
        if (visibleHeight < 5.0f) return;

        ImGui::SetNextWindowPos(ImVec2(catX, startY));
        ImGui::SetNextWindowSize(ImVec2(catWidth, visibleHeight));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoCollapse;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        if (ImGui::Begin("##ConfigsCategoryContent", nullptr, flags)) {
            ImGui::SetScrollY(yOffset);

            ImGui::PushItemWidth(catWidth - 16.0f);

            static char configNameBuf[64] = "default";
            ui::InputText("##ConfigProfileName", configNameBuf, sizeof(configNameBuf));

            ImGui::Spacing();
            const float btnW = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
            if (ui::PrimaryButton("Save", ImVec2(btnW, 26.0f))) {
                if (configNameBuf[0] != '\0') {
                    ConfigManager::instance().save_config(configNameBuf);
                }
            }
            ImGui::SameLine(0.0f, 6.0f);
            if (ui::SecondaryButton("Load", ImVec2(btnW, 26.0f))) {
                if (configNameBuf[0] != '\0') {
                    ConfigManager::instance().load_config(configNameBuf);
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(kTextMuted.Value, "Saved Profiles:");
            ImGui::Spacing();

            auto configs = ConfigManager::instance().get_available_configs();
            if (configs.empty()) {
                ImGui::TextDisabled("No profiles found.");
            } else {
                for (const auto& cfg : configs) {
                    ImGui::PushID(cfg.c_str());
                    const bool isSel = (cfg == configNameBuf);
                    if (ImGui::Selectable(cfg.c_str(), isSel)) {
                        strncpy_s(configNameBuf, cfg.c_str(), sizeof(configNameBuf) - 1);
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        ConfigManager::instance().load_config(cfg);
                    }

                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Load Profile")) {
                            ConfigManager::instance().load_config(cfg);
                            strncpy_s(configNameBuf, cfg.c_str(), sizeof(configNameBuf) - 1);
                        }
                        if (ImGui::MenuItem("Overwrite Profile")) {
                            ConfigManager::instance().save_config(cfg);
                        }
                        if (ImGui::MenuItem("Delete Profile")) {
                            ConfigManager::instance().delete_config(cfg);
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }

            float measuredH = std::ceil(ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y + 6.0f);
            if (measuredH > 50.0f) {
                g_configPanelHeight = measuredH;
            }

            ImGui::PopItemWidth();
        }
        ImGui::End();

        ImGui::PopStyleVar(2);
    }

} // anonymous namespace

namespace clickgui {

    void render() {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 screen = io.DisplaySize;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();

        ModuleManager& moduleManager = ModuleManager::instance();
        const auto& categories = moduleManager.categories();

        {
            float glowTop = screen.y - (screen.y / 3.0f);
            ImColor glowCol(16, 185, 129, 36);
            dl->AddRectFilledMultiColor(
                ImVec2(0.0f, glowTop), screen,
                ImColor(glowCol.Value.x, glowCol.Value.y, glowCol.Value.z, 0.0f),
                ImColor(glowCol.Value.x, glowCol.Value.y, glowCol.Value.z, 0.0f),
                glowCol, glowCol
            );
        }

        std::vector<std::string> visibleCategories;
        for (const auto& cat : categories) {
            auto mods = moduleManager.modules_in_category(cat);
            if (!mods.empty() || cat == "Configs") {
                visibleCategories.push_back(cat);
            }
        }

        if (!g_initPositions || g_catStates.size() != visibleCategories.size()) {
            float totalWidth = static_cast<float>(visibleCategories.size()) * (CAT_WIDTH + CAT_GAP) - CAT_GAP;
            float startX = (screen.x - totalWidth) * 0.5f;
            float startY = 60.0f;

            for (size_t i = 0; i < visibleCategories.size(); ++i) {
                auto& st = g_catStates[visibleCategories[i]];
                if (st.x == 0.0f && st.y == 0.0f) {
                    st.x = std::round((startX + static_cast<float>(i) * (CAT_WIDTH + CAT_GAP)) / 2.0f) * 2.0f;
                    st.y = std::round(startY / 2.0f) * 2.0f;
                }
            }
            g_initPositions = true;
        }

        std::string hoveredTooltip = "";

        for (size_t catIdx = 0; catIdx < visibleCategories.size(); ++catIdx) {
            const std::string& category = visibleCategories[catIdx];
            auto& catState = g_catStates[category];

            std::vector<Module*> modules = moduleManager.modules_in_category(category);

            float totalContentHeight = 0.0f;
            if (category == "Configs") {
                totalContentHeight = g_configPanelHeight;
            } else {
                for (auto* mod : modules) {
                    if (!mod) continue;
                    totalContentHeight += MOD_HEIGHT;

                    bool targetSettingsOpen = g_modShowSettings[mod];
                    g_modSettingsAnim[mod] = Animate(targetSettingsOpen ? 1.0f : 0.0f, g_modSettingsAnim[mod], 14.0f);
                    float sAnim = g_modSettingsAnim[mod];

                    if (sAnim > 0.005f) {
                        totalContentHeight += MOD_HEIGHT * sAnim;

                        for (const auto& s : mod->settings()) {
                            if (!s || !s->is_visible()) continue;
                            totalContentHeight += (s->type() == config::SettingType::FLOAT || s->type() == config::SettingType::INT ? (MOD_HEIGHT + 2.0f) : MOD_HEIGHT) * sAnim;

                            if (s->type() == config::SettingType::ENUM) {
                                bool targetEnumOpen = g_settingEnumOpen[s.get()];
                                g_settingEnumSlide[s.get()] = Animate(targetEnumOpen ? 1.0f : 0.0f, g_settingEnumSlide[s.get()], 14.0f);
                                float enumAnim = g_settingEnumSlide[s.get()];
                                auto* es = static_cast<config::EnumSetting*>(s.get());
                                if (enumAnim > 0.005f) {
                                    totalContentHeight += static_cast<float>(es->options().size()) * MOD_HEIGHT * enumAnim * sAnim;
                                }
                            }
                        }
                    }
                }
            }

            float catX = catState.x;
            float catY = catState.y;
            ImVec4 catHeaderRect(catX, catY, catX + CAT_WIDTH, catY + CAT_HEIGHT);

            if (catState.isDragging) {
                if (ImGui::IsMouseDown(0)) {
                    catState.x = std::clamp(io.MousePos.x - catState.dragOffsetX, 0.0f, screen.x - CAT_WIDTH);
                    catState.y = std::clamp(io.MousePos.y - catState.dragOffsetY, 0.0f, screen.y - CAT_HEIGHT);
                    catState.x = std::round(catState.x / 2.0f) * 2.0f;
                    catState.y = std::round(catState.y / 2.0f) * 2.0f;
                } else {
                    catState.isDragging = false;
                }
            } else if (IsMouseOver(catHeaderRect) && ImGui::IsMouseClicked(0) && !io.WantCaptureMouse) {
                catState.isDragging = true;
                catState.dragOffsetX = io.MousePos.x - catState.x;
                catState.dragOffsetY = io.MousePos.y - catState.y;
            }

            if (IsMouseOver(catHeaderRect) && ImGui::IsMouseClicked(1) && !io.WantCaptureMouse) {
                catState.isExtended = !catState.isExtended;
            }

            catState.extendAnim = Animate(catState.isExtended ? 1.0f : 0.0f, catState.extendAnim, 14.0f);

            const float maxVisibleH = (std::max)(60.0f, screen.y - catY - 70.0f);
            const float maxScroll = (std::max)(0.0f, totalContentHeight - maxVisibleH);

            catState.scrollEase = std::clamp(catState.scrollEase, 0.0f, maxScroll);
            if (catState.extendAnim <= 0.005f || maxScroll <= 0.0f) {
                catState.scrollEase = 0.0f;
            }
            catState.yOffset = Animate(catState.scrollEase, catState.yOffset, 14.0f);
            if (catState.extendAnim <= 0.005f) {
                catState.yOffset = 0.0f;
            }

            const float targetContentH = (totalContentHeight > maxVisibleH && maxVisibleH > 0.0f) ? maxVisibleH : totalContentHeight;
            const float visibleHeight = targetContentH * catState.extendAnim;
            const float catBottomY = catY + CAT_HEIGHT + visibleHeight;
            ImVec4 catWindowRect(catX, catY, catX + CAT_WIDTH, catBottomY);

            if (IsMouseOver(catWindowRect) && catState.extendAnim > 0.01f) {
                if (io.MouseWheel != 0.0f) {
                    catState.scrollEase -= io.MouseWheel * MOD_HEIGHT * 2.0f;
                    catState.scrollEase = std::clamp(catState.scrollEase, 0.0f, maxScroll);
                }
            }

            FillRectangle(
                catHeaderRect, kDarkHeader, 1.0f, CORNER_RADIUS,
                (catState.extendAnim > 0.005f) ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll,
                dl
            );

            ImVec2 dotPos(catX + 14.0f, catY + CAT_HEIGHT * 0.5f);
            dl->AddCircleFilled(dotPos, 5.0f, static_cast<ImU32>(kEmeraldGlow));
            dl->AddCircleFilled(dotPos, 2.5f, static_cast<ImU32>(kEmeraldAccent));

            ImVec2 titleSize = ImGui::CalcTextSize(category.c_str());
            float titleX = catX + (CAT_WIDTH - titleSize.x) * 0.5f;
            float titleY = catY + (CAT_HEIGHT - titleSize.y) * 0.5f;
            dl->AddText(ImVec2(titleX, titleY), static_cast<ImU32>(kTextWhite), category.c_str());

            const char* collapseIcon = (catState.isExtended ? "-" : "+");
            ImVec2 collSz = ImGui::CalcTextSize(collapseIcon);
            dl->AddText(ImVec2(catX + CAT_WIDTH - collSz.x - 12.0f, catY + (CAT_HEIGHT - collSz.y) * 0.5f), static_cast<ImU32>(kTextMuted), collapseIcon);

            dl->AddLine(
                ImVec2(catX, catY + CAT_HEIGHT),
                ImVec2(catX + CAT_WIDTH, catY + CAT_HEIGHT),
                static_cast<ImU32>(kEmeraldGlow), 1.0f
            );

            if (catState.extendAnim <= 0.005f) {
                continue;
            }

            FillRectangle(
                ImVec4(catX, catY + CAT_HEIGHT, catX + CAT_WIDTH, catBottomY),
                kModOffColor, 1.0f, CORNER_RADIUS,
                ImDrawFlags_RoundCornersBottom, dl
            );

            ImVec4 catClipRect(catX - 2.0f, catY + CAT_HEIGHT, catX + CAT_WIDTH + 2.0f, catBottomY);
            dl->PushClipRect(ImVec2(catClipRect.x, catClipRect.y), ImVec2(catClipRect.z, catClipRect.w), true);

            float currentY = catY + CAT_HEIGHT - catState.yOffset;

            if (category == "Configs") {
                dl->PopClipRect();
                render_config_panel(catX, catY + CAT_HEIGHT, CAT_WIDTH, visibleHeight, catState.yOffset);
                continue;
            }

            for (size_t mIdx = 0; mIdx < modules.size(); ++mIdx) {
                auto* mod = modules[mIdx];
                if (!mod) continue;

                bool isLastModule = (mIdx == modules.size() - 1);
                float targetToggle = mod->enabled() ? 1.0f : 0.0f;
                g_modToggleScale[mod] = Animate(targetToggle, g_modToggleScale[mod], 14.0f);
                float toggleScale = g_modToggleScale[mod];

                bool targetSettingsOpen = g_modShowSettings[mod];
                float settingsAnim = g_modSettingsAnim[mod];

                ImVec4 modRect(catX, currentY, catX + CAT_WIDTH, currentY + MOD_HEIGHT);
                float bottomRadius = (isLastModule && settingsAnim < 0.05f) ? CORNER_RADIUS : 0.0f;
                if (modRect.w >= catBottomY - 1.0f) {
                    modRect.w = catBottomY;
                    bottomRadius = CORNER_RADIUS;
                }

                const bool isHovered = IsMouseOver(modRect) && io.MousePos.y <= catBottomY && !io.WantCaptureMouse;

                if (modRect.w > modRect.y) {
                    ImColor bgCol = isHovered ? kModHoverColor : kModOffColor;
                    FillRectangle(modRect, bgCol, 1.0f, bottomRadius, ImDrawFlags_RoundCornersBottom, dl);

                    if (toggleScale > 0.01f) {
                        FillRoundedGradientRectangle(
                            modRect, kEmeraldAccent, kEmeraldDark,
                            bottomRadius, toggleScale,
                            ImDrawFlags_RoundCornersBottom, dl
                        );
                    }
                }

                const float textH = ImGui::GetTextLineHeight();
                const float textY = currentY + (MOD_HEIGHT - textH) * 0.5f;
                ImColor textCol = (toggleScale > 0.5f) ? kTextWhite : (isHovered ? kTextWhite : kTextMuted);
                dl->AddText(ImVec2(catX + 12.0f, textY), static_cast<ImU32>(textCol), mod->name().c_str());

                const bool isBindingThis = (InputManager::instance().binding_module() == mod);
                if (isBindingThis) {
                    std::string bindText = "[ ... ]";
                    ImVec2 bSz = ImGui::CalcTextSize(bindText.c_str());
                    dl->AddText(ImVec2(catX + CAT_WIDTH - bSz.x - 24.0f, textY), static_cast<ImU32>(kEmeraldLight), bindText.c_str());
                } else if (mod->keybind() != 0) {
                    std::string keyStr = "[" + std::string(input::get_key_name(mod->keybind())) + "]";
                    ImVec2 bSz = ImGui::CalcTextSize(keyStr.c_str());
                    dl->AddText(ImVec2(catX + CAT_WIDTH - bSz.x - 24.0f, textY), static_cast<ImU32>(kTextDisabled), keyStr.c_str());
                }

                const char* chevron = targetSettingsOpen ? "v" : ">";
                dl->AddText(ImVec2(catX + CAT_WIDTH - 14.0f, textY), static_cast<ImU32>(kTextDisabled), chevron);

                if (isHovered) {
                    if (!mod->tooltip().empty()) {
                        hoveredTooltip = mod->tooltip();
                    }

                    if (ImGui::IsMouseClicked(0)) {
                        mod->toggle();
                    }
                    else if (ImGui::IsMouseClicked(1)) {
                        g_modShowSettings[mod] = !g_modShowSettings[mod];
                    }
                    else if (ImGui::IsMouseClicked(2)) {
                        if (isBindingThis) {
                            InputManager::instance().cancel_binding();
                        } else {
                            InputManager::instance().begin_binding(mod);
                        }
                    }
                }

                currentY += MOD_HEIGHT;

                if (settingsAnim > 0.005f) {
                    float drawerStartY = currentY;

                    float drawerContentH = MOD_HEIGHT * settingsAnim; // hold mode
                    for (const auto& s : mod->settings()) {
                        if (!s || !s->is_visible()) continue;
                        drawerContentH += (s->type() == config::SettingType::FLOAT || s->type() == config::SettingType::INT ? (MOD_HEIGHT + 2.0f) : MOD_HEIGHT) * settingsAnim;
                        if (s->type() == config::SettingType::ENUM) {
                            float enumAnim = g_settingEnumSlide[s.get()];
                            auto* es = static_cast<config::EnumSetting*>(s.get());
                            if (enumAnim > 0.005f) {
                                drawerContentH += static_cast<float>(es->options().size()) * MOD_HEIGHT * enumAnim * settingsAnim;
                            }
                        }
                    }

                    float drawerEndY = drawerStartY + drawerContentH;
                    float drawerRadius = isLastModule ? (CORNER_RADIUS * settingsAnim) : 0.0f;
                    if (drawerEndY >= catBottomY - 1.0f) {
                        drawerEndY = catBottomY;
                        drawerRadius = CORNER_RADIUS;
                    }

                    if (drawerEndY > drawerStartY) {
                        FillRectangle(
                            ImVec4(catX, drawerStartY, catX + CAT_WIDTH, drawerEndY),
                            kSettingBgColor, settingsAnim, drawerRadius,
                            ImDrawFlags_RoundCornersBottom, dl
                        );
                    }

                    {
                        float rowH = MOD_HEIGHT * settingsAnim;
                        if (rowH >= 1.0f) {
                            ImVec4 rowRect(catX, currentY, catX + CAT_WIDTH, currentY + rowH);
                            float textRowY = currentY + (rowH - textH) * 0.5f;

                            dl->PushClipRect(ImVec2(rowRect.x, rowRect.y), ImVec2(rowRect.z, rowRect.w + 0.5f), true);

                            dl->AddText(ImVec2(catX + 14.0f, textRowY), static_cast<ImU32>(kTextWhite), "Hold Mode");

                            ImVec2 checkCenter(catX + CAT_WIDTH - 18.0f, currentY + rowH * 0.5f);
                            ImVec2 boxMin(checkCenter.x - 7.0f, checkCenter.y - 7.0f);
                            ImVec2 boxMax(checkCenter.x + 7.0f, checkCenter.y + 7.0f);

                            dl->AddRectFilled(boxMin, boxMax, IM_COL32(32, 32, 32, static_cast<int>(240 * settingsAnim)), 3.5f);
                            dl->AddRect(boxMin, boxMax, IM_COL32(75, 75, 75, static_cast<int>(220 * settingsAnim)), 3.5f, 0, 1.0f);

                            float targetHold = mod->hold_to_enable() ? 1.0f : 0.0f;
                            g_modHoldAnim[mod] = Animate(targetHold, g_modHoldAnim[mod], 14.0f);
                            float hScale = g_modHoldAnim[mod];

                            if (hScale > 0.01f) {
                                FillShadowCircle(checkCenter, 5.5f, kEmeraldAccent, hScale * 0.85f * settingsAnim, 20.0f, dl);
                                dl->AddRectFilled(boxMin, boxMax, ImColor(kEmeraldAccent.Value.x, kEmeraldAccent.Value.y, kEmeraldAccent.Value.z, hScale * settingsAnim), 3.5f);
                                dl->AddRect(boxMin, boxMax, ImColor(kEmeraldDark.Value.x, kEmeraldDark.Value.y, kEmeraldDark.Value.z, hScale * settingsAnim), 3.5f, 0, 1.0f);
                            }

                            if (hScale > 0.02f) {
                                ImVec4 checkClip(boxMin.x, boxMin.y, boxMin.x + 14.0f * hScale, boxMax.y);
                                dl->PushClipRect(ImVec2(checkClip.x, checkClip.y), ImVec2(checkClip.z, checkClip.w), true);
                                DrawCheckMark(ImVec2(checkCenter.x - 4.5f, checkCenter.y - 1.0f), 1.25f, kTextWhite, settingsAnim, 2.0f, dl);
                                dl->PopClipRect();
                            }

                            if (IsMouseOver(rowRect) && !io.WantCaptureMouse) {
                                hoveredTooltip = "When enabled, module is only active while holding its bound key.";
                                if (ImGui::IsMouseClicked(0)) {
                                    mod->set_hold_to_enable(!mod->hold_to_enable());
                                }
                            }

                            dl->PopClipRect();
                            currentY += rowH;
                        }
                    }

                    for (size_t sIdx = 0; sIdx < mod->settings().size(); ++sIdx) {
                        auto* setting = mod->settings()[sIdx].get();
                        if (!setting || !setting->is_visible()) continue;

                        const bool isSlider = (setting->type() == config::SettingType::FLOAT || setting->type() == config::SettingType::INT);
                        float setHeight = (isSlider ? (MOD_HEIGHT + 2.0f) : MOD_HEIGHT) * settingsAnim;
                        if (setHeight < 1.0f) continue;

                        ImVec4 setRect(catX, currentY, catX + CAT_WIDTH, currentY + setHeight);
                        dl->PushClipRect(ImVec2(setRect.x, setRect.y), ImVec2(setRect.z, setRect.w + 0.5f), true);

                        if (setting->type() == config::SettingType::BOOL) {
                            auto* bs = static_cast<config::BoolSetting*>(setting);
                            float targetBool = bs->value() ? 1.0f : 0.0f;
                            g_settingBoolScale[setting] = Animate(targetBool, g_settingBoolScale[setting], 14.0f);
                            float bScale = g_settingBoolScale[setting];

                            float textRowY = currentY + (setHeight - textH) * 0.5f;
                            dl->AddText(ImVec2(catX + 14.0f, textRowY), static_cast<ImU32>(kTextWhite), setting->name().c_str());

                            ImVec2 checkCenter(catX + CAT_WIDTH - 18.0f, currentY + setHeight * 0.5f);
                            ImVec2 boxMin(checkCenter.x - 7.0f, checkCenter.y - 7.0f);
                            ImVec2 boxMax(checkCenter.x + 7.0f, checkCenter.y + 7.0f);

                            dl->AddRectFilled(boxMin, boxMax, IM_COL32(32, 32, 32, static_cast<int>(240 * settingsAnim)), 3.5f);
                            dl->AddRect(boxMin, boxMax, IM_COL32(75, 75, 75, static_cast<int>(220 * settingsAnim)), 3.5f, 0, 1.0f);

                            if (bScale > 0.01f) {
                                FillShadowCircle(checkCenter, 5.5f, kEmeraldAccent, bScale * 0.85f * settingsAnim, 20.0f, dl);
                                dl->AddRectFilled(boxMin, boxMax, ImColor(kEmeraldAccent.Value.x, kEmeraldAccent.Value.y, kEmeraldAccent.Value.z, bScale * settingsAnim), 3.5f);
                                dl->AddRect(boxMin, boxMax, ImColor(kEmeraldDark.Value.x, kEmeraldDark.Value.y, kEmeraldDark.Value.z, bScale * settingsAnim), 3.5f, 0, 1.0f);
                            }

                            if (bScale > 0.02f) {
                                ImVec4 checkClip(boxMin.x, boxMin.y, boxMin.x + 14.0f * bScale, boxMax.y);
                                dl->PushClipRect(ImVec2(checkClip.x, checkClip.y), ImVec2(checkClip.z, checkClip.w), true);
                                DrawCheckMark(ImVec2(checkCenter.x - 4.5f, checkCenter.y - 1.0f), 1.25f, kTextWhite, settingsAnim, 2.0f, dl);
                                dl->PopClipRect();
                            }

                            if (IsMouseOver(setRect) && !io.WantCaptureMouse) {
                                if (!setting->description().empty()) hoveredTooltip = setting->description();
                                if (ImGui::IsMouseClicked(0)) {
                                    bs->set_value(!bs->value());
                                }
                            }
                        }
                        else if (isSlider) {
                            float val = 0.0f, minVal = 0.0f, maxVal = 1.0f;
                            char valBuf[32];
                            if (setting->type() == config::SettingType::FLOAT) {
                                auto* fs = static_cast<config::FloatSetting*>(setting);
                                val = fs->value(); minVal = fs->min(); maxVal = fs->max();
                                snprintf(valBuf, sizeof(valBuf), fs->format(), val);
                            } else {
                                auto* is = static_cast<config::IntSetting*>(setting);
                                val = static_cast<float>(is->value()); minVal = static_cast<float>(is->min()); maxVal = static_cast<float>(is->max());
                                snprintf(valBuf, sizeof(valBuf), "%d", static_cast<int>(val));
                            }

                            float textTopY = currentY + 3.0f;
                            dl->AddText(ImVec2(catX + 14.0f, textTopY), static_cast<ImU32>(kTextWhite), setting->name().c_str());

                            ImVec2 valSz = ImGui::CalcTextSize(valBuf);
                            dl->AddText(ImVec2(catX + CAT_WIDTH - valSz.x - 12.0f, textTopY), static_cast<ImU32>(kEmeraldLight), valBuf);

                            // Bottom Track Bar
                            float trackX1 = catX + 14.0f;
                            float trackX2 = catX + CAT_WIDTH - 12.0f;
                            float trackW = trackX2 - trackX1;
                            float trackY = currentY + setHeight - 6.0f;

                            float fraction = (maxVal > minVal) ? std::clamp((val - minVal) / (maxVal - minVal), 0.0f, 1.0f) : 0.0f;
                            float targetSliderWidth = fraction * trackW;
                            g_settingSliderEase[setting] = Animate(targetSliderWidth, g_settingSliderEase[setting], 16.0f);
                            float fillWidth = g_settingSliderEase[setting];

                            dl->AddRectFilled(ImVec2(trackX1, trackY - 1.5f), ImVec2(trackX2, trackY + 1.5f), IM_COL32(45, 45, 45, static_cast<int>(200 * settingsAnim)), 2.0f);

                            ImVec4 fillRect(trackX1, trackY - 2.0f, trackX1 + fillWidth, trackY + 2.0f);
                            FillRectangle(fillRect, kEmeraldAccent, settingsAnim, 2.0f, 0, dl);

                            ImVec2 handlePos(trackX1 + fillWidth, trackY);
                            FillShadowCircle(handlePos, 4.5f, kEmeraldAccent, 0.85f * settingsAnim, 20.0f, dl);
                            dl->AddCircleFilled(handlePos, 4.0f, static_cast<ImU32>(kTextWhite));

                            if (IsMouseOver(setRect) && !io.WantCaptureMouse) {
                                if (!setting->description().empty()) hoveredTooltip = setting->description();
                                if (ImGui::IsMouseDown(0)) {
                                    g_activeDraggedSlider = setting;
                                }
                            }

                            if (g_activeDraggedSlider == setting) {
                                if (ImGui::IsMouseDown(0)) {
                                    float newFrac = std::clamp((io.MousePos.x - trackX1) / trackW, 0.0f, 1.0f);
                                    float newVal = minVal + newFrac * (maxVal - minVal);
                                    if (setting->type() == config::SettingType::FLOAT) {
                                        static_cast<config::FloatSetting*>(setting)->set_value(newVal);
                                    } else {
                                        static_cast<config::IntSetting*>(setting)->set_value(static_cast<int>(std::round(newVal)));
                                    }
                                } else {
                                    g_activeDraggedSlider = nullptr;
                                }
                            }
                        }
                        else if (setting->type() == config::SettingType::ENUM) {
                            auto* es = static_cast<config::EnumSetting*>(setting);
                            float textRowY = currentY + (setHeight - textH) * 0.5f;

                            dl->AddText(ImVec2(catX + 14.0f, textRowY), static_cast<ImU32>(kTextWhite), setting->name().c_str());

                            std::string optName = es->current_option();
                            ImVec2 optSz = ImGui::CalcTextSize(optName.c_str());
                            dl->AddText(ImVec2(catX + CAT_WIDTH - optSz.x - 12.0f, textRowY), static_cast<ImU32>(kTextMuted), optName.c_str());

                            if (IsMouseOver(setRect) && !io.WantCaptureMouse) {
                                if (!setting->description().empty()) hoveredTooltip = setting->description();
                                if (ImGui::IsMouseClicked(0)) {
                                    int curVal = es->value();
                                    int nextVal = (curVal + 1) % static_cast<int>(es->options().size());
                                    es->set_value(nextVal);
                                } else if (ImGui::IsMouseClicked(1)) {
                                    g_settingEnumOpen[setting] = !g_settingEnumOpen[setting];
                                }
                            }
                        }
                        else if (setting->type() == config::SettingType::COLOR) {
                            auto* cs = static_cast<config::ColorSetting*>(setting);
                            float textRowY = currentY + (setHeight - textH) * 0.5f;
                            dl->AddText(ImVec2(catX + 14.0f, textRowY), static_cast<ImU32>(kTextWhite), setting->name().c_str());

                            ImVec4 swatchRect(catX + CAT_WIDTH - 24.0f, currentY + 7.0f, catX + CAT_WIDTH - 8.0f, currentY + setHeight - 7.0f);
                            float colArr[4];
                            cs->get_color(colArr);
                            ImColor imCol(colArr[0], colArr[1], colArr[2], colArr[3]);

                            dl->AddRectFilled(ImVec2(swatchRect.x, swatchRect.y), ImVec2(swatchRect.z, swatchRect.w), imCol, 3.0f);
                            dl->AddRect(ImVec2(swatchRect.x, swatchRect.y), ImVec2(swatchRect.z, swatchRect.w), IM_COL32(255, 255, 255, 70), 3.0f);

                            if (IsMouseOver(setRect) && !io.WantCaptureMouse) {
                                if (!setting->description().empty()) hoveredTooltip = setting->description();
                                if (ImGui::IsMouseClicked(0)) {
                                    g_activeColorSetting = cs;
                                    g_showColorPicker = true;
                                }
                            }
                        }

                        dl->PopClipRect();
                        currentY += setHeight;

                        if (setting->type() == config::SettingType::ENUM) {
                            auto* es = static_cast<config::EnumSetting*>(setting);
                            float enumAnim = g_settingEnumSlide[setting];
                            if (enumAnim > 0.005f) {
                                int curIdx = es->value();
                                for (size_t oIdx = 0; oIdx < es->options().size(); ++oIdx) {
                                    float optHeight = MOD_HEIGHT * enumAnim * settingsAnim;
                                    if (optHeight < 1.0f) continue;

                                    ImVec4 optRect(catX, currentY, catX + CAT_WIDTH, currentY + optHeight);
                                    dl->PushClipRect(ImVec2(optRect.x, optRect.y), ImVec2(optRect.z, optRect.w + 0.5f), true);

                                    FillRectangle(optRect, kEnumSubBg, enumAnim * settingsAnim, 0.0f, 0, dl);

                                    const bool isSelected = (static_cast<int>(oIdx) == curIdx);
                                    if (isSelected) {
                                        FillRectangle(ImVec4(optRect.x, optRect.y, optRect.x + 2.5f, optRect.w), kEmeraldAccent, enumAnim * settingsAnim, 0.0f, 0, dl);
                                    }

                                    float optTextY = currentY + (optHeight - textH) * 0.5f;
                                    ImColor optCol = isSelected ? kTextWhite : kTextMuted;
                                    dl->AddText(ImVec2(catX + 18.0f, optTextY), static_cast<ImU32>(optCol), es->options()[oIdx].c_str());

                                    if (IsMouseOver(optRect) && ImGui::IsMouseClicked(0) && !io.WantCaptureMouse) {
                                        es->set_value(static_cast<int>(oIdx));
                                    }

                                    dl->PopClipRect();
                                    currentY += optHeight;
                                }
                            }
                        }
                    }
                }
            }

            dl->PopClipRect();
        }

        {
            const bool isBinding = InputManager::instance().is_binding();
            const char* keyPrefix = isBinding ? "[Binding]" : "[Middle Click]";
            std::string hintSuffix = isBinding
                ? (" Press any key for " + (InputManager::instance().binding_module() ? InputManager::instance().binding_module()->name() : "module") + " ESC to cancel")
                : " to set keybind";

            ImVec2 preSz = ImGui::CalcTextSize(keyPrefix);
            ImVec2 sufSz = ImGui::CalcTextSize(hintSuffix.c_str());
            const float dotW = 14.0f;
            const float pillW = preSz.x + sufSz.x + dotW + 20.0f;
            const float pillH = 24.0f;
            const float pillX = (screen.x - pillW) * 0.5f;
            const float pillY = screen.y - 34.0f;

            ImVec4 pillRect(pillX, pillY, pillX + pillW, pillY + pillH);

            FillRectangle(pillRect, kDarkHeader, 0.90f, 12.0f, ImDrawFlags_RoundCornersAll, dl);
            dl->AddRect(ImVec2(pillRect.x, pillRect.y), ImVec2(pillRect.z, pillRect.w), static_cast<ImU32>(kEmeraldGlow), 12.0f, 0, 1.0f);

            ImVec2 dotPos(pillX + 11.0f, pillY + pillH * 0.5f);
            dl->AddCircleFilled(dotPos, 4.5f, static_cast<ImU32>(kEmeraldGlow));
            dl->AddCircleFilled(dotPos, 2.2f, static_cast<ImU32>(kEmeraldAccent));

            float textY = pillY + (pillH - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(pillX + dotW + 8.0f, textY), static_cast<ImU32>(kEmeraldLight), keyPrefix);
            dl->AddText(ImVec2(pillX + dotW + 8.0f + preSz.x, textY), static_cast<ImU32>(kTextMuted), hintSuffix.c_str());
        }

        if (!hoveredTooltip.empty()) {
            ImVec2 m = io.MousePos;
            ImVec2 ttSize = ImGui::CalcTextSize(hoveredTooltip.c_str());
            ImVec4 ttRect(m.x + 12.0f, m.y + 12.0f, m.x + ttSize.x + 24.0f, m.y + ttSize.y + 20.0f);

            if (ttRect.z > screen.x - 8.0f) {
                float shift = ttRect.z - (screen.x - 8.0f);
                ttRect.x -= shift;
                ttRect.z -= shift;
            }

            ImDrawList* fg = ImGui::GetForegroundDrawList();
            FillRectangle(ttRect, kDarkHeader, 0.96f, 6.0f, ImDrawFlags_RoundCornersAll, fg);
            fg->AddRect(ImVec2(ttRect.x, ttRect.y), ImVec2(ttRect.z, ttRect.w), static_cast<ImU32>(kEmeraldGlow), 6.0f, 0, 1.0f);
            fg->AddText(ImVec2(ttRect.x + 6.0f, ttRect.y + 4.0f), static_cast<ImU32>(kTextWhite), hoveredTooltip.c_str());
        }

        if (g_showColorPicker && g_activeColorSetting) {
            ImGui::SetNextWindowSize(ImVec2(240.0f, 260.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Color Picker", &g_showColorPicker, ImGuiWindowFlags_NoCollapse)) {
                float colArr[4];
                g_activeColorSetting->get_color(colArr);
                if (ImGui::ColorPicker4("##ColorPickerDiag", colArr, ImGuiColorEditFlags_AlphaBar)) {
                    g_activeColorSetting->set_color(colArr[0], colArr[1], colArr[2], colArr[3]);
                }
            }
            ImGui::End();
        }
    }

} // namespace clickgui
