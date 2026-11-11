#pragma once
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace ui::notifications {
enum class Level { Info, Success, Warning, Error };
struct Toast {
    std::string title;
    std::string message;
    Level level = Level::Info;
    std::chrono::steady_clock::time_point createdAt;
};
inline std::mutex& queue_mutex() { static std::mutex value; return value; }
inline std::deque<Toast>& queue() { static std::deque<Toast> value; return value; }
inline void push(std::string title, std::string message, Level level = Level::Info) {
    if (message.empty()) return;
    std::lock_guard lock(queue_mutex());
    auto& items = queue();
    items.push_back({std::move(title), std::move(message), level, std::chrono::steady_clock::now()});
    while (items.size() > 5) items.pop_front();
}
inline void info(std::string title, std::string message) { push(std::move(title), std::move(message), Level::Info); }
inline void success(std::string title, std::string message) { push(std::move(title), std::move(message), Level::Success); }
inline void warning(std::string title, std::string message) { push(std::move(title), std::move(message), Level::Warning); }
inline void error(std::string title, std::string message) { push(std::move(title), std::move(message), Level::Error); }

inline void render() {
    if (!ImGui::GetCurrentContext()) return;
    const auto now = std::chrono::steady_clock::now();
    std::vector<Toast> active;
    {
        std::lock_guard lock(queue_mutex());
        auto& items = queue();
        std::erase_if(items, [&](const Toast& toast) {
            return now - toast.createdAt >= std::chrono::milliseconds(4600);
        });
        active.assign(items.begin(), items.end());
    }
    if (active.empty()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;

    constexpr float width = 352.0f, rightMargin = 18.0f, topMargin = 18.0f;
    constexpr float padX = 13.0f, padY = 10.0f, textWidth = width - padX * 2.0f - 8.0f;
    float y = topMargin;
    for (auto it = active.rbegin(); it != active.rend(); ++it) {
        const Toast& toast = *it;
        const float age = std::chrono::duration<float>(now - toast.createdAt).count();
        const float fade = std::clamp(std::min(age / 0.16f, (4.6f - age) / 0.35f), 0.0f, 1.0f);
        const ImVec2 titleSize = ImGui::CalcTextSize(toast.title.c_str());
        const ImVec2 messageSize = ImGui::CalcTextSize(toast.message.c_str(), nullptr, false, textWidth);
        const float height = padY * 2.0f + titleSize.y + 5.0f + std::max(1.0f, messageSize.y);
        const ImVec2 min(ImGui::GetIO().DisplaySize.x - width - rightMargin + 14.0f * (1.0f - fade), y);
        const ImVec2 max(min.x + width, min.y + height);
        ImU32 accent = IM_COL32(80, 130, 190, static_cast<int>(255.0f * fade));
        if (toast.level == Level::Success) accent = IM_COL32(42, 155, 102, static_cast<int>(255.0f * fade));
        if (toast.level == Level::Warning) accent = IM_COL32(205, 140, 38, static_cast<int>(255.0f * fade));
        if (toast.level == Level::Error) accent = IM_COL32(195, 65, 65, static_cast<int>(255.0f * fade));
        draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(max.x + 2.0f, max.y + 3.0f),
            IM_COL32(20, 30, 45, static_cast<int>(35.0f * fade)), 8.0f);
        draw->AddRectFilled(min, max, IM_COL32(250, 252, 255, static_cast<int>(245.0f * fade)), 8.0f);
        draw->AddRect(min, max, IM_COL32(205, 216, 230, static_cast<int>(230.0f * fade)), 8.0f, 0, 1.0f);
        draw->AddRectFilled(ImVec2(min.x, min.y + 7.0f), ImVec2(min.x + 3.0f, max.y - 7.0f), accent, 2.0f);
        draw->AddText(ImVec2(min.x + padX, min.y + padY),
            IM_COL32(34, 46, 62, static_cast<int>(255.0f * fade)), toast.title.c_str());
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
            ImVec2(min.x + padX, min.y + padY + titleSize.y + 5.0f),
            IM_COL32(83, 96, 113, static_cast<int>(255.0f * fade)),
            toast.message.c_str(), nullptr, textWidth);
        y = max.y + 9.0f;
    }
}
} // namespace ui::notifications
