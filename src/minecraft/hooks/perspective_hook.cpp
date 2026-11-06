#include "minecraft/hooks/perspective_hook.h"
#include "events/event_bus.h"
#include "events/perspective_event.h"
#include "utils/logger.h"

#include <MinHook.h>
#include <atomic>

namespace {
    using GetPerspectiveFn = int(__fastcall*)(void* options);
    GetPerspectiveFn g_originalGetPerspective = nullptr;
    void* g_perspectiveTarget = nullptr;

    std::atomic<int> g_currentPerspective{ 0 };
    std::atomic<int> g_overridePerspective{ -1 };

    int __fastcall hook_get_perspective(void* options) {
        const int actualValue = g_originalGetPerspective ? g_originalGetPerspective(options) : 0;
        g_currentPerspective.store(actualValue, std::memory_order_relaxed);

        PerspectiveEvent event(actualValue);
        EventBus::instance().publish(event);

        if (event.isOverridden()) {
            return event.overridePerspective;
        }

        const int overrideVal = g_overridePerspective.load(std::memory_order_relaxed);
        if (overrideVal >= 0) {
            return overrideVal;
        }

        return actualValue;
    }
}

namespace minecraft::hooks {
    bool install_perspective_hook(std::uintptr_t funcAddr) {
        if (!funcAddr) {
            LOG_ERROR("Invalid Options::getPerspective address provided!");
            return false;
        }

        LOG_INFO("Hooking Options::getPerspective at address: {:#x}", funcAddr);

        g_perspectiveTarget = reinterpret_cast<void*>(funcAddr);
        if (MH_CreateHook(g_perspectiveTarget, reinterpret_cast<LPVOID>(&hook_get_perspective), reinterpret_cast<void**>(&g_originalGetPerspective)) != MH_OK) {
            LOG_ERROR("Failed to create Options::getPerspective hook!");
            g_perspectiveTarget = nullptr;
            return false;
        }

        if (MH_EnableHook(g_perspectiveTarget) != MH_OK) {
            LOG_ERROR("Failed to enable Options::getPerspective hook!");
            return false;
        }

        LOG_INFO("Options::getPerspective hook enabled.");
        return true;
    }

    void remove_perspective_hook() {
        g_overridePerspective.store(-1, std::memory_order_relaxed);
        if (g_perspectiveTarget) {
            MH_DisableHook(g_perspectiveTarget);
            MH_RemoveHook(g_perspectiveTarget);
            g_perspectiveTarget = nullptr;
            g_originalGetPerspective = nullptr;
            LOG_INFO("Options::getPerspective hook removed.");
        }
    }

    int get_perspective() {
        const int overrideVal = g_overridePerspective.load(std::memory_order_relaxed);
        if (overrideVal >= 0) {
            return overrideVal;
        }
        return g_currentPerspective.load(std::memory_order_relaxed);
    }

    void set_perspective(int perspective) {
        g_overridePerspective.store(perspective, std::memory_order_relaxed);
    }
}
