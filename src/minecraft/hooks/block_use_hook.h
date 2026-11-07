#pragma once

class LocalPlayer;

namespace minecraft::hooks {
    void install_block_use_hook(LocalPlayer* localPlayer);
    void remove_block_use_hook();
}
