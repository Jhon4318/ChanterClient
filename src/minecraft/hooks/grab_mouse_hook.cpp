#include "minecraft/hooks/grab_mouse_hook.h"
#include "minecraft/sdk/client/client_instance.h"
#include "events/event_bus.h"
#include "events/mouse_grab_event.h"
#include "events/client_instance_update_event.h"
#include "render/dx12/dx12_hook.h"
#include "utils/logger.h"

#include <MinHook.h>

namespace {
    using GrabMouseFn = void(__fastcall*)(ClientInstance* self);
    using ReleaseMouseFn = void(__fastcall*)(ClientInstance* self);

    GrabMouseFn g_originalGrabMouse = nullptr;
    void* g_grabMouseTarget = nullptr;
    std::uintptr_t g_releaseMouseAddr = 0;
    EventBus::ListenerId g_updateListener = 0;

    void __fastcall hook_client_instance_grab_mouse(ClientInstance* self) {
        MouseGrabEvent event(self);
        EventBus::instance().publish(event);

        if (event.is_cancelled() || dx12_hook::is_menu_open()) {
            return;
        }

        if (g_originalGrabMouse) {
            g_originalGrabMouse(self);
        }
    }

    void on_client_update(ClientInstanceUpdateEvent& event) {
        if (!event.clientInstance) return;

        static bool wasMenuOpen = false;
        const bool menuOpen = dx12_hook::is_menu_open();
        if (menuOpen != wasMenuOpen) {
            if (menuOpen) {
                minecraft::hooks::release_mouse(event.clientInstance);
            } else {
                if (ClientInstance::getLocalPlayer() && !event.clientInstance->isGameUiOpen()) {
                    minecraft::hooks::grab_mouse(event.clientInstance);
                }
            }
            wasMenuOpen = menuOpen;
        }
    }
}

namespace minecraft::hooks {
    bool install_grab_mouse_hook(std::uintptr_t grabMouseAddr, std::uintptr_t releaseMouseAddr) {
        if (!grabMouseAddr) {
            LOG_ERROR("Invalid ClientInstance::grabMouse address provided!");
            return false;
        }

        g_releaseMouseAddr = releaseMouseAddr;

        LOG_INFO("Hooking ClientInstance::grabMouse at address: {:#x}", grabMouseAddr);

        g_grabMouseTarget = reinterpret_cast<void*>(grabMouseAddr);
        if (MH_CreateHook(g_grabMouseTarget, reinterpret_cast<LPVOID>(&hook_client_instance_grab_mouse), reinterpret_cast<void**>(&g_originalGrabMouse)) != MH_OK) {
            LOG_ERROR("Failed to create ClientInstance::grabMouse hook!");
            g_grabMouseTarget = nullptr;
            return false;
        }

        if (MH_EnableHook(g_grabMouseTarget) != MH_OK) {
            LOG_ERROR("Failed to enable ClientInstance::grabMouse hook!");
            return false;
        }

        g_updateListener = EventBus::instance().subscribe<ClientInstanceUpdateEvent>(on_client_update);

        LOG_INFO("ClientInstance grabMouse hook enabled.");
        return true;
    }

    void remove_grab_mouse_hook() {
        if (g_updateListener != 0) {
            EventBus::instance().unsubscribe(g_updateListener);
            g_updateListener = 0;
        }

        if (g_grabMouseTarget) {
            MH_DisableHook(g_grabMouseTarget);
            MH_RemoveHook(g_grabMouseTarget);
            g_grabMouseTarget = nullptr;
            g_originalGrabMouse = nullptr;
            LOG_INFO("ClientInstance grabMouse hook removed.");
        }

        g_releaseMouseAddr = 0;
    }

    void grab_mouse(ClientInstance* instance) {
        if (!instance) {
            instance = ClientInstance::get();
        }
        if (!instance || !g_grabMouseTarget) return;

        __try {
            reinterpret_cast<GrabMouseFn>(g_grabMouseTarget)(instance);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void release_mouse(ClientInstance* instance) {
        if (!instance) {
            instance = ClientInstance::get();
        }
        if (!instance || !g_releaseMouseAddr) return;

        __try {
            reinterpret_cast<ReleaseMouseFn>(g_releaseMouseAddr)(instance);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
