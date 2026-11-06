#pragma once

#include <cstdint>

namespace minecraft::hooks {
    bool install_actor_set_rot_hook(std::uintptr_t setRotAddr);
    void remove_actor_set_rot_hook();
}
