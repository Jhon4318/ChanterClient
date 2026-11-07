#include "minecraft/hooks/block_use_hook.h"

#include "events/block_use_event.h"
#include "events/event_bus.h"
#include "minecraft/hooks/hook_memory.h"
#include "minecraft/bedrock_memory.h"
#include "minecraft/sdk/actor/game_mode.h"
#include "minecraft/sdk/actor/local_player.h"
#include "minecraft/sdk/world/block_identity.h"
#include "utils/logger.h"

#include <MinHook.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace {
    using GameModeUseItemOnFn = InteractionResult(__fastcall*)(
        void* self,
        void* itemStack,
        const minecraft::sdk::BlockPosition* position,
        std::uint8_t face,
        const Vec3* hitPosition,
        HandSlot handSlot,
        const void* targetBlock,
        bool isFirstEvent);

    GameModeUseItemOnFn g_originalUseItemOn = nullptr;
    std::uintptr_t g_hookedAddress = 0;
    std::mutex g_hookMutex;

    InteractionResult __fastcall hook_game_mode_use_item_on(
        void* self,
        void* itemStack,
        const minecraft::sdk::BlockPosition* position,
        std::uint8_t face,
        const Vec3* hitPosition,
        HandSlot handSlot,
        const void* targetBlock,
        bool isFirstEvent)
    {
        const auto original = g_originalUseItemOn;
        if (original == nullptr) {
            return {};
        }

        // Always preserve vanilla behavior and its return value.
        const InteractionResult result = original(
            self, itemStack, position, face, hitPosition, handSlot,
            targetBlock, isFirstEvent);

        // Only the initial user-input event is relevant. Simulation repeats are ignored.
        if (result.mSuccess && isFirstEvent && position != nullptr && targetBlock != nullptr) {
            std::string blockType;
            if (minecraft::sdk::block_identity::try_get_type_name(targetBlock, blockType)) {
                BlockUseEvent event(
                    std::move(blockType),
                    position->x, position->y, position->z,
                    face,
                    static_cast<std::uint8_t>(handSlot),
                    isFirstEvent);
                EventBus::instance().publish(event);
            }
        }

        return result;
    }
}

namespace minecraft::hooks {

    void install_block_use_hook(LocalPlayer* localPlayer) {
        if (localPlayer == nullptr) {
            return;
        }

        GameMode* gameMode = localPlayer->getGameMode();
        if (gameMode == nullptr) {
            return;
        }

        const std::uintptr_t target = hook_memory::vtable_entry(
            gameMode, minecraft::BedrockVtableSlots::gameMode_UseItemOn);
        if (target == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_hookMutex);
        if (g_hookedAddress == target) {
            return;
        }

        if (g_hookedAddress != 0) {
            MH_DisableHook(reinterpret_cast<LPVOID>(g_hookedAddress));
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_hookedAddress));
            g_hookedAddress = 0;
            g_originalUseItemOn = nullptr;
        }

        if (MH_CreateHook(
                reinterpret_cast<LPVOID>(target),
                reinterpret_cast<LPVOID>(&hook_game_mode_use_item_on),
                reinterpret_cast<void**>(&g_originalUseItemOn)) != MH_OK) {
            LOG_WARN("[BlockUseHook] Could not create GameMode::useItemOn hook.");
            return;
        }

        if (MH_EnableHook(reinterpret_cast<LPVOID>(target)) != MH_OK) {
            MH_RemoveHook(reinterpret_cast<LPVOID>(target));
            g_originalUseItemOn = nullptr;
            LOG_WARN("[BlockUseHook] Could not enable GameMode::useItemOn hook.");
            return;
        }

        g_hookedAddress = target;
        LOG_INFO("[BlockUseHook] GameMode::useItemOn hook attached at {:#x}.", target);
    }

    void remove_block_use_hook() {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        if (g_hookedAddress == 0) {
            return;
        }

        MH_DisableHook(reinterpret_cast<LPVOID>(g_hookedAddress));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_hookedAddress));
        g_hookedAddress = 0;
        g_originalUseItemOn = nullptr;
    }

}
