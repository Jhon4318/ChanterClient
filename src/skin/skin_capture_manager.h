#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace minecraft::skin {

struct OnlinePlayer {
    std::string uuid;
    std::string player_name;
    std::uint64_t actor_unique_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool has_skin = false;
    bool has_cape = false;
    bool has_geometry = false;
};

struct CapturedSkin {
    std::string uuid;
    std::string player_name;
    std::filesystem::path file_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t index = 0;
    bool has_cape = false;
    bool has_geometry = false;
};

bool initialize();
void shutdown();

void set_capture_enabled(bool enabled);
std::vector<OnlinePlayer> online_players();
std::vector<CapturedSkin> captured_skins();
bool save_player_skin(const std::string& uuid);
std::filesystem::path skins_directory();
void open_skins_directory();

} // namespace minecraft::skin
