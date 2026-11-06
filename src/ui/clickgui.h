#pragma once

namespace clickgui {
void render();
}

namespace menu {
inline void render() { clickgui::render(); }
}
