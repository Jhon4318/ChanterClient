#pragma once

#include <imgui.h>
#include <string>
#include <vector>

namespace ui {

    struct Palette {
        ImU32 bg_dark = IM_COL32(11, 13, 17, 252);
        ImU32 panel_bg = IM_COL32(17, 20, 26, 252);
        ImU32 card_bg = IM_COL32(23, 27, 36, 255);
        ImU32 card_hover = IM_COL32(31, 37, 50, 255);
        ImU32 card_enabled = IM_COL32(14, 42, 33, 255);
        ImU32 border = IM_COL32(42, 50, 66, 180);
        ImU32 border_focused = IM_COL32(16, 185, 129, 220);

        ImU32 text_primary = IM_COL32(245, 248, 252, 255);
        ImU32 text_secondary = IM_COL32(156, 163, 175, 255);
        ImU32 text_muted = IM_COL32(100, 116, 139, 255);

        ImU32 accent = IM_COL32(16, 185, 129, 255);        // Emerald Accent (#10B981)
        ImU32 accent_hover = IM_COL32(52, 211, 153, 255);  // Light Emerald (#34D399)
        ImU32 accent_active = IM_COL32(5, 150, 105, 255);  // Deep Emerald (#059669)
        ImU32 accent_muted = IM_COL32(16, 185, 129, 50);   // Emerald Glow

        ImU32 danger = IM_COL32(239, 68, 68, 255);
        ImU32 danger_hover = IM_COL32(248, 113, 113, 255);
    };

    const Palette& get_palette();
    void apply_theme();

    // UI Widget Controls
    bool ToggleSwitch(const char* label, bool* v);
    bool Checkbox(const char* label, bool* v);
    bool SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.2f");
    bool SliderInt(const char* label, int* v, int v_min, int v_max);
    bool Combo(const char* label, int* current_item, const std::vector<std::string>& items);
    bool ColorPicker(const char* label, float col[4]);
    bool InputText(const char* label, char* buf, size_t buf_size);
    bool KeybindBadge(const char* label, int keybind, bool is_binding);
    bool SearchBar(const char* id, char* buf, size_t buf_size, const char* placeholder = "Search modules...");

    void SectionHeader(const char* title, const char* badge = nullptr);
    bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
    bool SecondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
    bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0));
}