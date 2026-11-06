#include "modules/visual/Watermark.h"

#include <imgui.h>

Watermark::Watermark()
    : Module("Visual", "Watermark", "Displays the JhonClient name on screen.") {
}

void Watermark::on_imgui_render() {
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) {
        return;
    }

    constexpr ImVec2 origin{ 14.0f, 14.0f };
    constexpr float height = 30.0f;
    constexpr float horizontalPadding = 11.0f;
    constexpr float verticalPadding = 7.0f;

    constexpr const char* brand = "JhonClient";
    constexpr const char* version = "26.60";

    const ImVec2 brandSize = ImGui::CalcTextSize(brand);
    const ImVec2 versionSize = ImGui::CalcTextSize(version);
    const float gap = 12.0f;
    const float width = horizontalPadding * 2.0f + brandSize.x + gap + versionSize.x;

    const ImVec2 min = origin;
    const ImVec2 max{ origin.x + width, origin.y + height };

    drawList->AddRectFilled(min, max, IM_COL32(15, 19, 27, 220), 7.0f);
    drawList->AddRect(min, max, IM_COL32(44, 190, 147, 230), 7.0f, 0, 1.0f);
    drawList->AddRectFilled(
        ImVec2(min.x, min.y + 5.0f),
        ImVec2(min.x + 3.0f, max.y - 5.0f),
        IM_COL32(44, 210, 160, 255),
        1.5f
    );

    const float textY = origin.y + verticalPadding;
    drawList->AddText(
        ImVec2(origin.x + horizontalPadding, textY),
        IM_COL32(244, 247, 250, 255),
        brand
    );
    drawList->AddText(
        ImVec2(origin.x + horizontalPadding + brandSize.x + gap, textY),
        IM_COL32(150, 164, 180, 255),
        version
    );
}
