#pragma once

#include <cstdint>
#include <string_view>
#include "utils/hash.h"

// Forward declarations
class LocalPlayer;
class ClientInstance;
class LevelRenderer;
class GuiData;
class ScreenContext;

#define DEFINE_SIG(name, readableName, patternStr) \
    struct name##_sig_t { \
        static constexpr const char*     symbol       = #name; \
        static constexpr const char*     display_name = (readableName); \
        static constexpr std::string_view pattern     {patternStr}; \
        static constexpr std::uint64_t   hash         = ::utils::hash::fnv1a_64(patternStr); \
        \
        [[nodiscard]] constexpr operator const char*() const noexcept { return patternStr; } \
        [[nodiscard]] constexpr operator std::string_view() const noexcept { return pattern; } \
        [[nodiscard]] static constexpr const char* get() noexcept { return patternStr; } \
        [[nodiscard]] static constexpr const char* name() noexcept { return display_name; } \
        [[nodiscard]] static constexpr std::string_view view() noexcept { return pattern; } \
    }; \
    static inline constexpr name##_sig_t name{}

#define DEFINE_OFFSET(OwnerClass, FieldType, name, offsetVal) \
    struct name##_offset_t { \
        using owner_type = OwnerClass; \
        using field_type = FieldType; \
        static constexpr std::uintptr_t  offset = (offsetVal); \
        static constexpr const char*     symbol = #name; \
        \
        template <typename T> \
        [[nodiscard]] constexpr FieldType& get(T* inst) const noexcept { \
            return *reinterpret_cast<FieldType*>(reinterpret_cast<std::uintptr_t>(inst) + offset); \
        } \
        template <typename T> \
        [[nodiscard]] constexpr FieldType& operator()(T* inst) const noexcept { \
            return get(inst); \
        } \
        [[nodiscard]] constexpr operator std::uintptr_t() const noexcept { \
            return offset; \
        } \
    }; \
    static inline constexpr name##_offset_t name{}

#define DEFINE_VTABLE_SLOT(name, slotIndex) \
    struct name##_slot_t { \
        static constexpr int         index  = (slotIndex); \
        static constexpr const char* symbol = #name; \
        [[nodiscard]] constexpr operator int() const noexcept { return index; } \
    }; \
    static inline constexpr name##_slot_t name{}

namespace minecraft {

    struct BedrockVtableSlots { 
        DEFINE_VTABLE_SLOT(gameMode_UseItemOn, 13);
        DEFINE_VTABLE_SLOT(gameMode_Attack, 15);
    };

    struct BedrockOffsets {
        DEFINE_OFFSET(ClientInstance, void*,            clientInstance_levelRenderer, 0x1E0); 
        DEFINE_OFFSET(ClientInstance, float*,           clientInstance_glMatrix,      0x438);
        DEFINE_OFFSET(ClientInstance, GuiData*,         clientInstance_guiData,       0x668);

        DEFINE_OFFSET(LocalPlayer,    void*,            localPlayer_gameMode,         0xAC8); 

        DEFINE_OFFSET(LevelRenderer,  void*,            levelRenderer_renderPlayer,   0x480);
        DEFINE_OFFSET(void,           float*,           renderPlayer_cameraPos,       0x64C);
        DEFINE_OFFSET(void,           float,            renderPlayer_fovX,            0xF50); 
        DEFINE_OFFSET(void,           float,            renderPlayer_fovY,            0xF64);
         
        DEFINE_OFFSET(GuiData,        float*,           guiData_windowSize,           0x040);
        DEFINE_OFFSET(ScreenContext,  float*,           screenContext_shaderColor,    0x030);
        DEFINE_OFFSET(ScreenContext,  void*,            screenContext_tessellator,    0x0B0);
    };

    struct BedrockSignatures {
        DEFINE_SIG(clientInstance_Update, "ClientInstance::update",
            "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 45 89 C6 89 D3 48 89 CE 48 8B 01 48 8B 80 ? ? ? ? 48 8D 95 ? ? ? ? FF 15");

        DEFINE_SIG(clientInstance_grabMouse, "ClientInstance::grabMouse",
            "56 48 83 EC 20 48 89 CE 48 8B 01 48 8B 80 ? ? ? ? FF 15 ? ? ? ? 84 C0 74 ? 48 8B 8E ? ? ? ? 48 8B 01 48 8B 80 38 04 00 00");

        DEFINE_SIG(clientInstance_releaseMouse, "ClientInstance::releaseMouse",
            "56 48 83 EC 20 48 89 CE 48 8B 01 48 8B 80 ? ? ? ? FF 15 ? ? ? ? 84 C0 74 ? 48 8B 8E ? ? ? ? 48 8B 01 48 8B 80 40 04 00 00");

        DEFINE_SIG(clientInstance_getLocalPlayer, "ClientInstance::getLocalPlayer",
            "48 83 EC ? 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 48 8D 91 68 02 00 00 48 8D 4C 24 ? E8 ? ? ? ? 80 7C 24 ? 01 75 ? 48 8D 4C 24 ? B2 01");

        DEFINE_SIG(clientInstance_getScreenName, "ClientInstance::getScreenName",
            "55 56 57 48 83 EC ? 48 8D 6C 24 ? 48 C7 45 ? FE FF FF FF 48 89 D6 48 8B 01 48 8B 80 ? ? 00 00 48 8D 55 ? FF 15 ? ? ? ? 48 8B 45 ? 48 85 C0 0F 84 ? ? 00 00 80 38 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 4D ? 48 8B 01 48 8B 80 60 01 00 00");

        DEFINE_SIG(actor_normalTick, "Actor::normalTick",
            "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 85 ? ? ? ? 66 0F 7F BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 48 89 CE 80 B9 ? ? ? ? 01 75 ? 83 BE ? ? ? ? 00 7F ? 48 8B 8E ? ? ? ? 48 8B 01");

        DEFINE_SIG(actor_setRotation, "Actor::setRotation",
            "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? 00 00 44 0F 29 ? 24 ? ? 00 00 44 0F 29 ? 24 ? ? 00 00 44 0F 29 ? 24 ? ? 00 00 0F 29 ? 24 ? ? 00 00 0F 29 ? 24 ? ? 00 00 4C 89 C6 49 8B 40 10 ? 8B ? 18");

        DEFINE_SIG(options_getPerspective, "Options::getPerspective",
            "48 83 EC ? 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 48 8B 01 48 8B 40 ? 48 8D 54 24 ? 41 B8 03 00 00 00");

        // Container screen and slot state-machine signatures for Bedrock 1.26.60.
        // These are optional: ContainerTransfer remains inactive unless every required
        // signature and both controller offsets are resolved.
        DEFINE_SIG(container_sm_handle, "ContainerScreen state-machine handle",
            "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 4D 89 CC 44 89 C5 89 D7 48 89 CB 48 8B 05 ? ? ? ?");
        DEFINE_SIG(container_sm_offset_site, "ContainerScreen state-machine offset site",
            "56 48 83 EC 30 48 8B 44 24 60 48 8B 71 08 8B 12 45 8B 00 8B 00 48 8D 8E ? ? ? ? 89 44 24 20 E8");
        DEFINE_SIG(container_manager_offset_site, "ContainerScreen container-manager offset site",
            "48 8B 8B ? ? ? ? 48 8B 01 48 8B 40 40 4C 89 FA FF 15");
        DEFINE_SIG(container_get_item, "ContainerManager::getItem",
            "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 44 89 C6 48 89 D3 4C 8B 72 10 48 83 7A 18 10");
        DEFINE_SIG(item_stack_is_null, "ItemStack::isNull",
            "41 56 56 57 53 48 83 EC 28 B0 01 80 79 23 01 0F 85 ? ? ? ? 48 8B 51 08 48 85 D2 0F 84 ? ? ? ? 48 8B 32 48 85 F6 0F 84 ? ? ? ? 48 8B 96 20 01 00 00 48 3B 15 ? ? ? ? 0F 85 ? ? ? ?");
        DEFINE_SIG(item_stack_max_stack_size, "ItemStack::getMaxStackSize",
            "55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 8B 51 08 B0 FF 48 85 D2 74");
        DEFINE_SIG(container_screen_tick, "ContainerScreenController::tick",
            "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 30 48 C7 45 28 FE FF FF FF 48 89 CE 48 8D B9 ? ? 00 00 C6 45 F8 00 48 81 C1 ? ? 00 00 E8");
        DEFINE_SIG(container_screen_ctor, "ContainerScreenController constructor",
            "55 41 57 41 56 41 55 41 54 56 57 53 B8 D8 2D 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 80 00 00 00 0F 29 B5 40 2D 00 00");
        DEFINE_SIG(scene_stack_push, "SceneStack::push",
            "55 56 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF C6 45 D8 00 0F 57 C0 0F 29 45 E0 48 8B 42 08 48 85 C0 74 ? F0 FF 40 08 48 8B 42 08 EB ? 31 C0 48 89 55 F8 48 8B 12 48 89 55 E0 48 89 45 E8 48 8D 55 E0 4C 8D 4D B0 E8");

        DEFINE_SIG(render_levelRender, "LevelRenderer::renderLevel",
            "E8 ? ? ? ? 31 FF 48 8B 86 ? ? 00 00 48 85 C0");

        DEFINE_SIG(render_tessVertex, "Tessellator::vertex",
            "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 44 0F 29 4C 24 60 44 0F 29 44 24 50 0F 29 7C 24 40 0F 29 74 24 30 8B 81 98 02 00 00 3B 81 9C 02 00 00");

        DEFINE_SIG(render_tessBegin, "Tessellator::begin",
            "56 57 55 53 48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 80 B9 A0 02 00 00 00 0F 85");

        DEFINE_SIG(render_tessColor, "Tessellator::color",
            "80 B9 54 02 00 00 00 0F 85 ? ? 00 00 F3 0F 10 05 ? ? ? ? F3 0F 59 C8");

        DEFINE_SIG(render_meshImmediate, "MeshHelpers::renderMeshImmediately",
            "55 41 57 41 56 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 80 BA ? ? ? ? ? 0F 85 ? ? ? ? 4C 89 CF");

        DEFINE_SIG(render_uiFillColor, "MaterialPtr::getUIColor",
            "C6 45 C0 00 4C 8D 05 ? ? ? ? 4C 8D 4D A8 48 89 F1 48 89 FA E8");
    };

} // namespace minecraft