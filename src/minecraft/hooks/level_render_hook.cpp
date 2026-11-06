#include "minecraft/hooks/level_render_hook.h"
#include "minecraft/sdk/render/screen_context.h"
#include "minecraft/sdk/client/client_instance.h"
#include "minecraft/hooks/hook_memory.h"
#include "events/event_bus.h"
#include "events/level_render_event.h"
#include "utils/logger.h"

#include <MinHook.h>

namespace {
    using LevelRenderFn = __int64(__fastcall*)(LevelRenderer*, void*, __int64);
    LevelRenderFn g_originalLevelRender = nullptr;
    void* g_levelRenderTarget = nullptr;

    __int64 __fastcall hook_level_renderer_render_level(
        LevelRenderer* self, void* screenContext, __int64 renderParams)
    {
        ScreenContext::s_instance3d = reinterpret_cast<ScreenContext*>(screenContext);

        __int64 result = g_originalLevelRender
            ? g_originalLevelRender(self, screenContext, renderParams)
            : 0;

        if (self && ClientInstance::getLocalPlayer()) {
            LevelRenderEvent event(self, ScreenContext::s_instance3d);
            EventBus::instance().publish(event);
        }

        ScreenContext::s_instance3d = nullptr;
        return result;
    }
}

namespace minecraft::hooks {

    bool install_level_render_hook(std::uintptr_t callSigAddr) {
        if (!callSigAddr) {
            LOG_ERROR("Invalid LevelRenderer::renderLevel call signature address!");
            return false;
        }

        std::uintptr_t funcAddr = minecraft::hook_memory::resolve_call(callSigAddr);
        if (!funcAddr) {
            LOG_ERROR("Failed to resolve LevelRenderer::renderLevel call target!");
            return false;
        }

        LOG_INFO("Hooking LevelRenderer::renderLevel at {:#x} (call at {:#x})", funcAddr, callSigAddr);

        g_levelRenderTarget = reinterpret_cast<void*>(funcAddr);
        if (MH_CreateHook(g_levelRenderTarget, &hook_level_renderer_render_level,
                          reinterpret_cast<void**>(&g_originalLevelRender)) != MH_OK) {
            LOG_ERROR("Failed to create LevelRenderer::renderLevel hook!");
            g_levelRenderTarget = nullptr;
            return false;
        }

        if (MH_EnableHook(g_levelRenderTarget) != MH_OK) {
            LOG_ERROR("Failed to enable LevelRenderer::renderLevel hook!");
            return false;
        }

        LOG_INFO("LevelRenderer::renderLevel hook enabled.");
        return true;
    }

    void remove_level_render_hook() {
        if (g_levelRenderTarget) {
            MH_DisableHook(g_levelRenderTarget);
            MH_RemoveHook(g_levelRenderTarget);
            g_levelRenderTarget = nullptr;
            g_originalLevelRender = nullptr;
            LOG_INFO("LevelRenderer::renderLevel hook removed.");
        }
    }
}
