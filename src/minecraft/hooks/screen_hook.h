#pragma once

#include <cstdint>
#include <string>

class ClientInstance;

namespace minecraft::hooks {
    bool install_screen_hook(std::uintptr_t screenNameAddr);
    void remove_screen_hook();

    void update_screen_name(ClientInstance* self);
    std::string get_current_screen_name();
    bool is_game_ui_open();

    void set_screen_override(const std::string& screenName);
    void clear_screen_override();
}
