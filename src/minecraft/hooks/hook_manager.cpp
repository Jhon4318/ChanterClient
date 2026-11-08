#include "minecraft/hooks/hook_manager.h"
#include "skin/skin_capture_manager.h"
#include "minecraft/hooks/client_instance_hook.h"
#include "minecraft/hooks/grab_mouse_hook.h"
#include "minecraft/hooks/screen_hook.h"
#include "minecraft/hooks/actor_tick_hook.h"
#include "minecraft/hooks/actor_set_rot_hook.h"
#include "minecraft/hooks/perspective_hook.h"
#include "minecraft/hooks/attack_hook.h"
#include "minecraft/hooks/block_use_hook.h"
#include "minecraft/sdk/ui/container_ui.h"
#include "minecraft/hooks/level_render_hook.h"
#include "minecraft/hooks/hook_memory.h"
#include "minecraft/bedrock_memory.h"
#include "minecraft/sdk/client/client_instance.h"
#include "events/event_bus.h"
#include "events/client_instance_update_event.h"
#include "utils/logger.h"

#include <MinHook.h>

namespace minecraft::hooks {

    namespace {
        std::uintptr_t s_getLocalPlayerAddr = 0;

        void on_client_update(ClientInstanceUpdateEvent& event) {
            if (!s_getLocalPlayerAddr) return;

            void* localPlayer = hook_memory::call_function<void*>(
                s_getLocalPlayerAddr,
                event.clientInstance);

            if (localPlayer) {
                ClientInstance::setLocalPlayer(static_cast<LocalPlayer*>(localPlayer));
                install_actor_tick_hook(localPlayer, event.clientInstance);
                install_attack_hook(static_cast<LocalPlayer*>(localPlayer));
                install_block_use_hook(static_cast<LocalPlayer*>(localPlayer));
                minecraft::container_ui::tick_offstack_screen();
            }
        }
    }

    bool initialize() {
        LOG_INFO("Initializing MinHook framework...");
        if (MH_Initialize() != MH_OK) {
            LOG_ERROR("Failed to initialize MinHook!");
            return false;
        }

        LOG_INFO("Resolving Minecraft Bedrock v26.60 signatures...");

        const std::uintptr_t updateAddr = hook_memory::resolve_signature(BedrockSignatures::clientInstance_Update);
        const std::uintptr_t grabMouseAddr = hook_memory::resolve_signature(BedrockSignatures::clientInstance_grabMouse);
        const std::uintptr_t releaseMouseAddr = hook_memory::resolve_signature(BedrockSignatures::clientInstance_releaseMouse);
        const std::uintptr_t screenNameAddr = hook_memory::resolve_signature(BedrockSignatures::clientInstance_getScreenName);
        s_getLocalPlayerAddr = hook_memory::resolve_signature(BedrockSignatures::clientInstance_getLocalPlayer);
        const std::uintptr_t actorTickAddr = hook_memory::resolve_signature(BedrockSignatures::actor_normalTick);
        const std::uintptr_t actorSetRotAddr = hook_memory::resolve_signature(BedrockSignatures::actor_setRotation);
        const std::uintptr_t perspectiveAddr = hook_memory::resolve_signature(BedrockSignatures::options_getPerspective);
        const std::uintptr_t levelRenderCallAddr = hook_memory::resolve_signature(BedrockSignatures::render_levelRender);

        // These signatures are required for the current SDK/hook pipeline.
        // On a different game build, abort before installing any detours.
        if (!updateAddr || !s_getLocalPlayerAddr || !actorTickAddr || !levelRenderCallAddr) {
            LOG_ERROR("Required Minecraft signatures were not found. JhonClient is configured for v26.60; initialization was aborted.");
            return false;
        }

        // Native in-world drawing is not needed by the initial HUD module,
        // but report missing rendering helpers clearly for later modules.
        hook_memory::resolve_signature(BedrockSignatures::render_uiFillColor);
        hook_memory::resolve_signature(BedrockSignatures::render_tessBegin);
        hook_memory::resolve_signature(BedrockSignatures::render_tessColor);
        hook_memory::resolve_signature(BedrockSignatures::render_tessVertex);
        hook_memory::resolve_signature(BedrockSignatures::render_meshImmediate);

        set_actor_tick_address(actorTickAddr);
        EventBus::instance().subscribe<ClientInstanceUpdateEvent>(on_client_update);

        if (!install_client_instance_hook(updateAddr)) {
            LOG_ERROR("Failed to install ClientInstance::update hook!");
            return false;
        }

        if (grabMouseAddr && releaseMouseAddr) {
            if (!install_grab_mouse_hook(grabMouseAddr, releaseMouseAddr)) {
                LOG_WARN("Could not install mouse-grab hooks; GUI input capture may be limited.");
            }
        }

        if (screenNameAddr && !install_screen_hook(screenNameAddr)) {
            LOG_WARN("Could not install ClientInstance::getScreenName hook.");
        }

        if (actorSetRotAddr && !install_actor_set_rot_hook(actorSetRotAddr)) {
            LOG_WARN("Could not install Actor::setRotation hook.");
        }

        if (perspectiveAddr && !install_perspective_hook(perspectiveAddr)) {
            LOG_WARN("Could not install Options::getPerspective hook.");
        }

        if (!install_level_render_hook(levelRenderCallAddr)) {
            LOG_ERROR("Failed to install LevelRenderer::renderLevel hook!");
            return false;
        }

        // Skin packet capture is optional: a missing version-specific packet factory
        // must not prevent the rest of the client from starting.
        if (!minecraft::skin::initialize()) {
            LOG_WARN("SkinStealer packet hooks are unavailable for this exact Bedrock build.");
        }

        LOG_INFO("All required game hooks initialized successfully.");
        return true;
    }

    void shutdown() {
        LOG_INFO("Shutting down game hooks...");
        remove_block_use_hook();
        minecraft::container_ui::shutdown();
        minecraft::skin::shutdown();
        ClientInstance::setLocalPlayer(nullptr);
        remove_screen_hook();
        remove_grab_mouse_hook();
        remove_level_render_hook();
        remove_perspective_hook();
        remove_actor_set_rot_hook();
        remove_attack_hook();
        remove_actor_tick_hook();
        remove_client_instance_hook();
        s_getLocalPlayerAddr = 0;
        MH_Uninitialize();
    }

}
