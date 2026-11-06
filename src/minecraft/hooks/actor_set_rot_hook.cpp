#include "minecraft/hooks/actor_set_rot_hook.h"
#include "minecraft/sdk/client/client_instance.h"
#include "minecraft/sdk/actor/local_player.h"
#include "events/event_bus.h"
#include "events/actor_set_rot_event.h"
#include "utils/logger.h"

#include <MinHook.h>

namespace {
    using ActorSetRotFn = float*(__fastcall*)(void* rotationAngles, void* rotationContext, Actor* actor);
    ActorSetRotFn g_originalActorSetRot = nullptr;
    void* g_actorSetRotTarget = nullptr;

    float* __fastcall hook_actor_set_rot(void* rotationAngles, void* rotationContext, Actor* actor) {
        float* result = g_originalActorSetRot 
            ? g_originalActorSetRot(rotationAngles, rotationContext, actor) 
            : nullptr;

        LocalPlayer* localPlayer = ClientInstance::getLocalPlayer();
        if (actor != nullptr && localPlayer != nullptr && actor == reinterpret_cast<Actor*>(localPlayer)) {
            ActorSetRotEvent event(actor, localPlayer);
            EventBus::instance().publish(event);
        }

        return result;
    }
}

namespace minecraft::hooks {
    bool install_actor_set_rot_hook(std::uintptr_t setRotAddr) {
        if (!setRotAddr) {
            LOG_ERROR("Invalid Actor::setRotation address provided!");
            return false;
        }

        LOG_INFO("Hooking Actor::setRotation at address: {:#x}", setRotAddr);

        g_actorSetRotTarget = reinterpret_cast<void*>(setRotAddr);
        if (MH_CreateHook(g_actorSetRotTarget, reinterpret_cast<LPVOID>(&hook_actor_set_rot), reinterpret_cast<void**>(&g_originalActorSetRot)) != MH_OK) {
            LOG_ERROR("Failed to create Actor::setRotation hook!");
            g_actorSetRotTarget = nullptr;
            return false;
        }

        if (MH_EnableHook(g_actorSetRotTarget) != MH_OK) {
            LOG_ERROR("Failed to enable Actor::setRotation hook!");
            return false;
        }

        LOG_INFO("Actor::setRotation hook enabled.");
        return true;
    }

    void remove_actor_set_rot_hook() {
        if (g_actorSetRotTarget) {
            MH_DisableHook(g_actorSetRotTarget);
            MH_RemoveHook(g_actorSetRotTarget);
            g_actorSetRotTarget = nullptr;
            g_originalActorSetRot = nullptr;
            LOG_INFO("Actor::setRotation hook removed.");
        }
    }
}
