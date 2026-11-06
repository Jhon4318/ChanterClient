#pragma once

#include "events/perspective_event.h"
#include <cstdint>

namespace minecraft::hooks {
    bool install_perspective_hook(std::uintptr_t funcAddr);
    void remove_perspective_hook();

    int get_perspective();
    void set_perspective(int perspective);
}
