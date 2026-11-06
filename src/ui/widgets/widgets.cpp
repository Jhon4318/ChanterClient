#include "ui/widgets/widgets.h"
#include "input/key_names.h"

#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

    static Palette g_palette{};

    const Palette& get_palette() {
        return g_palette;
    }

    static void DrawGlowCircle(const ImVec2& pos, float radius, ImU32 color, ImDrawList* dl) {
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(color);
        dl->AddCircleFilled(pos, radius * 1.8f, ImColor(col.x, col.y, col.z, col.w * 0.12f));
        dl->AddCircleFilled(pos, radius * 1.3f, ImColor(col.x, col.y, col.z, col.w * 0.28f));
        dl->AddCircleFilled(pos, radius, ImColor(col.x, col.y, col.z, col.w * 0.55f));
    }

    static void DrawCheckMark(const ImVec2& pos, float size, ImU32 color, float thickness, ImDrawList* dl) {
        ImVec2 end1(pos.x + (3.0f * size), pos.y + (3.0f * size));
        ImVec2 end2(pos.x + (7.0f * size), pos.y - (3.0f * size));
        dl->AddLine(pos, end1, color, thickness);
        dl->AddLine(end1, end2, color, thickness);
    }

    void apply_theme() {
        const Palette& p = get_palette();
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowPadding = ImVec2(12.0f, 12.0f);
        style.FramePadding = ImVec2(10.0f, 6.0f);
        style.ItemSpacing = ImVec2(8.0f, 8.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);

        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 4.0f;

        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(p.text_primary);
        colors[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(p.text_secondary);
        colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(p.bg_dark);
        colors[ImGuiCol_ChildBg] = ImGui::ColorConvertU32ToFloat4(p.panel_bg);
        colors[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(p.panel_bg);
        colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(p.border);
        colors[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(p.card_bg);
        colors[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(p.card_hover);
        colors[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(p.card_hover);
        colors[ImGuiCol_TitleBg] = ImGui::ColorConvertU32ToFloat4(p.panel_bg);
        colors[ImGuiCol_TitleBgActive] = ImGui::ColorConvertU32ToFloat4(p.panel_bg);
        colors[ImGuiCol_ScrollbarBg] = ImGui::ColorConvertU32ToFloat4(p.bg_dark);
        colors[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(p.card_hover);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImGui::ColorConvertU32ToFloat4(p.border);
        colors[ImGuiCol_ScrollbarGrabActive] = ImGui::ColorConvertU32ToFloat4(p.accent);
        colors[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(p.accent);
        colors[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(p.accent);
        colors[ImGuiCol_SliderGrabActive] = ImGui::ColorConvertU32ToFloat4(p.accent_active);
        colors[ImGuiCol_Button] = ImGui::ColorConvertU32ToFloat4(p.card_bg);
        colors[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(p.card_hover);
        colors[ImGuiCol_ButtonActive] = ImGui::ColorConvertU32ToFloat4(p.accent_muted);
        colors[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(p.accent_muted);
        colors[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(p.card_hover);
        colors[ImGuiCol_HeaderActive] = ImGui::ColorConvertU32ToFloat4(p.accent);
        colors[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(p.border);
    }

    bool Checkbox(const char* label, bool* v) {
        ImGui::PushID(label);
        const Palette& p = get_palette();

        const float boxSize = 15.0f;
        const float availW = ImGui::GetContentRegionAvail().x;
        ImVec2 cursor = ImGui::GetCursorScreenPos();

        if (label && !(label[0] == '#' && label[1] == '#')) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
        }

        ImVec2 boxPos(cursor.x + availW - boxSize, cursor.y + (ImGui::GetTextLineHeight() - boxSize) * 0.5f);

        ImGui::SetCursorScreenPos(boxPos);
        ImGui::InvisibleButton("##chk_btn", ImVec2(boxSize, boxSize));

        const bool clicked = ImGui::IsItemClicked();
        const bool hovered = ImGui::IsItemHovered();

        if (clicked) {
            *v = !(*v);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 boxMax(boxPos.x + boxSize, boxPos.y + boxSize);
        ImVec2 boxCenter(boxPos.x + boxSize * 0.5f, boxPos.y + boxSize * 0.5f);

        if (*v) {
            DrawGlowCircle(boxCenter, 6.0f, p.accent_muted, dl);
            dl->AddRectFilled(boxPos, boxMax, p.accent, 3.5f);
            dl->AddRect(boxPos, boxMax, p.accent_active, 3.5f, 0, 1.0f);
            DrawCheckMark(ImVec2(boxCenter.x - 4.5f, boxCenter.y - 0.5f), 1.25f, p.text_primary, 2.0f, dl);
        } else {
            const ImU32 bg = hovered ? p.card_hover : p.card_bg;
            dl->AddRectFilled(boxPos, boxMax, bg, 3.5f);
            dl->AddRect(boxPos, boxMax, hovered ? p.border_focused : p.border, 3.5f, 0, 1.0f);
        }

        ImGui::PopID();
        return clicked;
    }

    bool ToggleSwitch(const char* label, bool* v) {
        ImGui::PushID(label);
        const Palette& p = get_palette();

        const float switchW = 32.0f;
        const float switchH = 17.0f;
        const float availW = ImGui::GetContentRegionAvail().x;

        ImVec2 cursor = ImGui::GetCursorScreenPos();

        if (label && !(label[0] == '#' && label[1] == '#')) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
        }

        ImVec2 switchPos(cursor.x + availW - switchW, cursor.y + (ImGui::GetTextLineHeight() - switchH) * 0.5f);

        ImGui::SetCursorScreenPos(switchPos);
        ImGui::InvisibleButton("##sw_btn", ImVec2(switchW, switchH));

        const bool clicked = ImGui::IsItemClicked();
        const bool hovered = ImGui::IsItemHovered();

        if (clicked) {
            *v = !(*v);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 switchMax(switchPos.x + switchW, switchPos.y + switchH);

        if (*v) {
            DrawGlowCircle(ImVec2(switchPos.x + switchW - 8.5f, switchPos.y + switchH * 0.5f), 7.0f, p.accent_muted, dl);
            dl->AddRectFilled(switchPos, switchMax, p.accent, 8.5f);
            dl->AddRect(switchPos, switchMax, p.accent_active, 8.5f, 0, 1.0f);

            const float knobX = switchPos.x + switchW - 8.5f;
            dl->AddCircleFilled(ImVec2(knobX, switchPos.y + switchH * 0.5f), 5.5f, p.text_primary);
        } else {
            const ImU32 bg = hovered ? p.card_hover : p.card_bg;
            dl->AddRectFilled(switchPos, switchMax, bg, 8.5f);
            dl->AddRect(switchPos, switchMax, hovered ? p.border_focused : p.border, 8.5f, 0, 1.0f);

            const float knobX = switchPos.x + 8.5f;
            dl->AddCircleFilled(ImVec2(knobX, switchPos.y + switchH * 0.5f), 5.5f, p.text_secondary);
        }

        ImGui::PopID();
        return clicked;
    }

    bool SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format) {
        ImGui::PushID(label);
        const Palette& p = get_palette();

        const float availW = ImGui::GetContentRegionAvail().x;
        ImVec2 cursor = ImGui::GetCursorScreenPos();

        char valBuf[64];
        std::snprintf(valBuf, sizeof(valBuf), format, *v);
        const ImVec2 valSize = ImGui::CalcTextSize(valBuf);

        const float rowHeight = 28.0f;
        ImGui::InvisibleButton("##slider_area", ImVec2(availW, rowHeight));

        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        bool changed = false;

        const float trackX1 = cursor.x;
        const float trackX2 = cursor.x + availW;
        const float trackW = trackX2 - trackX1;
        const float trackY = cursor.y + rowHeight - 6.0f;

        if (active) {
            float mouseX = ImGui::GetIO().MousePos.x - trackX1;
            float percent = ImClamp(mouseX / trackW, 0.0f, 1.0f);
            float newVal = v_min + percent * (v_max - v_min);
            if (*v != newVal) {
                *v = newVal;
                changed = true;
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (label && !(label[0] == '#' && label[1] == '#')) {
            dl->AddText(cursor, p.text_primary, label);
        }
        dl->AddText(ImVec2(trackX2 - valSize.x, cursor.y), p.accent_hover, valBuf);

        float fraction = (v_max > v_min) ? ImClamp((*v - v_min) / (v_max - v_min), 0.0f, 1.0f) : 0.0f;
        float fillW = fraction * trackW;

        dl->AddRectFilled(ImVec2(trackX1, trackY - 2.0f), ImVec2(trackX2, trackY + 2.0f), p.card_hover, 2.0f);
        if (fillW > 0.0f) {
            dl->AddRectFilled(ImVec2(trackX1, trackY - 2.0f), ImVec2(trackX1 + fillW, trackY + 2.0f), active ? p.accent_active : p.accent, 2.0f);
        }

        ImVec2 knobPos(trackX1 + fillW, trackY);
        if (hovered || active) {
            DrawGlowCircle(knobPos, 7.0f, p.accent_muted, dl);
            dl->AddCircleFilled(knobPos, 5.0f, p.text_primary);
        } else {
            dl->AddCircleFilled(knobPos, 4.0f, p.accent_hover);
        }

        ImGui::PopID();
        return changed;
    }

    bool SliderInt(const char* label, int* v, int v_min, int v_max) {
        float f_val = static_cast<float>(*v);
        bool changed = SliderFloat(label, &f_val, static_cast<float>(v_min), static_cast<float>(v_max), "%.0f");
        if (changed) {
            *v = static_cast<int>(std::round(f_val));
        }
        return changed;
    }

    bool Combo(const char* label, int* current_item, const std::vector<std::string>& items) {
        if (items.empty()) return false;
        ImGui::PushID(label);
        const Palette& p = get_palette();

        if (label && !(label[0] == '#' && label[1] == '#')) {
            ImGui::TextUnformatted(label);
            ImGui::Spacing();
        }

        std::string currentStr = (*current_item >= 0 && *current_item < static_cast<int>(items.size()))
            ? items[*current_item] : "Select...";

        bool changed = false;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, p.card_bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.card_hover);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, p.card_hover);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, p.panel_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, p.border);
        ImGui::PushStyleColor(ImGuiCol_Header, p.accent_muted);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, p.accent_muted);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, p.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_primary);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##combo_box", currentStr.c_str())) {
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const bool isSelected = (*current_item == i);
                if (ImGui::Selectable(items[i].c_str(), isSelected)) {
                    *current_item = i;
                    changed = true;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);
        ImGui::PopID();
        return changed;
    }

    bool ColorPicker(const char* label, float col[4]) {
        ImGui::PushID(label);
        const Palette& p = get_palette();

        if (label && !(label[0] == '#' && label[1] == '#')) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
        }

        ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, p.card_bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.card_hover);
        bool changed = ImGui::ColorEdit4("##color_edit", col, flags);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        ImGui::PopID();
        return changed;
    }

    bool InputText(const char* label, char* buf, size_t buf_size) {
        ImGui::PushID(label);
        const Palette& p = get_palette();

        if (label && !(label[0] == '#' && label[1] == '#')) {
            ImGui::TextUnformatted(label);
            ImGui::Spacing();
        }

        ImGui::PushStyleColor(ImGuiCol_FrameBg, p.card_bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.card_hover);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, p.panel_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, p.border);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        ImGui::SetNextItemWidth(-1.0f);
        bool changed = ImGui::InputText("##text_input", buf, buf_size);

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        return changed;
    }

    bool KeybindBadge(const char* label, int keybind, bool is_binding) {
        ImGui::PushID(label);
        const Palette& p = get_palette();

        if (label && !(label[0] == '#' && label[1] == '#')) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
        }

        std::string keyStr = is_binding ? "[ ... ]" : (std::string("[ ") + input::get_key_name(keybind) + " ]");
        ImVec2 textSz = ImGui::CalcTextSize(keyStr.c_str());
        ImVec2 btnSz(textSz.x + 14.0f, textSz.y + 6.0f);

        const float availW = ImGui::GetContentRegionAvail().x;
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImVec2 btnPos(cursor.x + availW - btnSz.x, cursor.y + (ImGui::GetTextLineHeight() - btnSz.y) * 0.5f);

        ImGui::SetCursorScreenPos(btnPos);
        ImGui::InvisibleButton("##keybind_btn", btnSz);

        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 bg = is_binding ? p.accent_muted : (hovered ? p.card_hover : p.card_bg);
        ImU32 border = is_binding ? p.accent : (hovered ? p.border_focused : p.border);
        ImU32 textCol = is_binding ? p.accent_hover : (hovered ? p.text_primary : p.text_secondary);

        if (is_binding) {
            DrawGlowCircle(ImVec2(btnPos.x + btnSz.x * 0.5f, btnPos.y + btnSz.y * 0.5f), btnSz.y * 0.6f, p.accent_muted, dl);
        }

        dl->AddRectFilled(btnPos, ImVec2(btnPos.x + btnSz.x, btnPos.y + btnSz.y), bg, 4.0f);
        dl->AddRect(btnPos, ImVec2(btnPos.x + btnSz.x, btnPos.y + btnSz.y), border, 4.0f, 0, 1.0f);
        dl->AddText(ImVec2(btnPos.x + 7.0f, btnPos.y + 3.0f), textCol, keyStr.c_str());

        ImGui::PopID();
        return clicked;
    }

    bool SearchBar(const char* id, char* buf, size_t buf_size, const char* placeholder) {
        ImGui::PushID(id);
        const Palette& p = get_palette();

        ImGui::PushStyleColor(ImGuiCol_FrameBg, p.card_bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.card_hover);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, p.panel_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, p.border);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));

        bool changed = ImGui::InputTextWithHint("##search_input", placeholder, buf, buf_size);

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        return changed;
    }

    void SectionHeader(const char* title, const char* badge) {
        const Palette& p = get_palette();
        ImGui::TextColored(ImColor(p.text_primary), "%s", title);
        if (badge && badge[0] != '\0') {
            ImGui::SameLine();
            ImGui::TextColored(ImColor(p.accent_hover), "(%s)", badge);
        }
        ImGui::Spacing();
    }

    bool PrimaryButton(const char* label, const ImVec2& size) {
        const Palette& p = get_palette();
        ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.accent_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.accent_active);
        ImGui::PushStyleColor(ImGuiCol_Text, p.bg_dark);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);

        bool clicked = ImGui::Button(label, size);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return clicked;
    }

    bool SecondaryButton(const char* label, const ImVec2& size) {
        const Palette& p = get_palette();
        ImGui::PushStyleColor(ImGuiCol_Button, p.card_bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.card_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.panel_bg);
        ImGui::PushStyleColor(ImGuiCol_Border, p.border);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_primary);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

        bool clicked = ImGui::Button(label, size);

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
        return clicked;
    }

    bool DangerButton(const char* label, const ImVec2& size) {
        const Palette& p = get_palette();
        ImGui::PushStyleColor(ImGuiCol_Button, p.danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.danger_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.danger);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_primary);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);

        bool clicked = ImGui::Button(label, size);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return clicked;
    }
}