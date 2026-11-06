#include "minecraft/sdk/client/client_instance.h"
#include "minecraft/hooks/grab_mouse_hook.h"
#include "minecraft/hooks/screen_hook.h"
#include "render/dx12/dx12_hook.h"

#include <cmath>

bool ClientInstance::world_to_screen(const math::Vec3& worldPos, math::Vec2& screenPos) const {
    if (!this) return false;

    __try {
        LevelRenderer* lr = getLevelRenderer();
        if (!lr) return false;

        LevelRendererPlayer* rp = lr->getLevelRendererPlayer();
        if (!rp) return false;

        GuiData* gui = getGuiData();
        if (!gui) return false;

        const math::Vec3 origin = rp->getOrigin();
        const math::Vec3 rel = worldPos - origin;

        const GLMatrix mat = getGLMatrix();
        const math::Vec3 transformed = mat.transform(rel);

        if (transformed.z > 0.0f) {
            return false;
        }

        const float divisor = -transformed.z;
        if (divisor < 0.001f) {
            return false;
        }

        const math::Vec2 screenSize = gui->getWindowSize();
        const float halfW = screenSize.x * 0.5f;
        const float halfH = screenSize.y * 0.5f;
        const float fovX = rp->getFovX();
        const float fovY = rp->getFovY();

        screenPos.x = halfW + (halfW * transformed.x / divisor * fovX);
        screenPos.y = halfH - (halfH * transformed.y / divisor * fovY);

        return std::isfinite(screenPos.x) && std::isfinite(screenPos.y);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClientInstance::releaseMouse() {
    minecraft::hooks::release_mouse(this);
}

void ClientInstance::grabMouse() {
    minecraft::hooks::grab_mouse(this);
}

std::string ClientInstance::getScreenName() {
    return minecraft::hooks::get_current_screen_name();
}

bool ClientInstance::isGameUiOpen() {
    return minecraft::hooks::is_game_ui_open();
}

bool ClientInstance::isClientUiOpen() {
    return dx12_hook::is_menu_open();
}



