#pragma once

#include <cstdint>

class ClientInstance;

namespace minecraft::hooks {
    bool install_grab_mouse_hook(std::uintptr_t grabMouseAddr, std::uintptr_t releaseMouseAddr);
    void remove_grab_mouse_hook();

    void grab_mouse(ClientInstance* instance = nullptr);
    void release_mouse(ClientInstance* instance = nullptr);
}
