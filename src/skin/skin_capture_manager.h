#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace minecraft::skin {

struct CapturedSkin {
    std::string uuid;
    std::string player_name;
    std::filesystem::path file_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

bool initialize();
void shutdown();

void set_capture_enabled(bool enabled);
void set_target_filter(bool enabled, std::string uuid);
std::vector<CapturedSkin> captured_skins();
std::filesystem::path skins_directory();
void open_skins_directory();

} // namespace minecraft::skin
