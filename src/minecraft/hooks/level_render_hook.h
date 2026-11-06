#pragma once

#include <cstdint>

namespace minecraft::hooks {
    bool install_level_render_hook(std::uintptr_t callSigAddr);
    void remove_level_render_hook();
}
