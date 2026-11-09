#include "minecraft/sdk/ui/container_ui.h"

#include "minecraft/bedrock_memory.h"
#include "minecraft/hooks/hook_memory.h"
#include "utils/logger.h"

#include <MinHook.h>
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace minecraft::container_ui {
namespace {

using ContainerSmHandleFn = int(__fastcall*)(
    void* stateMachine, std::uint32_t buttonId, int state,
    const std::string* collection, int slot);
using StackIsNullFn = bool(__fastcall*)(const void* stack);
using StackMaxSizeFn = std::uint8_t(__fastcall*)(const void* stack);
using ScreenCtorFn = void*(__fastcall*)(void* self, void* a2, void* a3, void* a4);
using ScenePushFn = void(__fastcall*)(void* stack, void* scene, void* a3, void* a4);
using ControllerTickFn = std::uint32_t(__fastcall*)(void* controller);

struct ScreenState {
    void* controller = nullptr;
    void* stateMachine = nullptr;
    void* manager = nullptr;
    std::shared_ptr<void> managerModelOwner;
    const void* emptyStack = nullptr;
};

struct OffstackState {
    void* sceneObject = nullptr;
    void* sharedCounts = nullptr;
    void* controller = nullptr;
    void* tick = nullptr;
    DWORD threadId = 0;
};

constexpr std::ptrdiff_t kStackCountOffset = 0x22;
constexpr std::ptrdiff_t kStackNetTagOffset = 0x90;
constexpr std::ptrdiff_t kStackItemOffset = 0x08;
constexpr std::ptrdiff_t kItemNameOffset = 0x128;
// Verified against Minecraft package 1.26.6005.0: ContainerManagerModel::mContainers.
constexpr std::ptrdiff_t kContainerModelsOffset = 0xC8;
constexpr std::size_t kContainerModelGetSizeSlot = 4;
constexpr std::size_t kContainerModelGetItemStackSlot = 8;
constexpr int kMaxCollectionSlots = 64;

constexpr std::uint32_t button_id(const char* text) {
    std::uint32_t hash = 0x811C9DC5u;
    for (; *text != '\0'; ++text) {
        hash ^= static_cast<std::uint8_t>(*text);
        hash *= 0x01000193u;
    }
    return hash;
}
constexpr std::uint32_t kShiftClickButton = button_id("button.container_auto_place");
constexpr int kPressed = 0;
constexpr int kHeld = 1;
constexpr int kReleased = 2;

ContainerSmHandleFn g_smHandle = nullptr;
StackIsNullFn g_stackIsNull = nullptr;
StackMaxSizeFn g_stackMaxSize = nullptr;
ControllerTickFn g_containerScreenTick = nullptr;
ScreenCtorFn g_screenCtor = nullptr;
ScenePushFn g_scenePush = nullptr;

std::ptrdiff_t g_stateMachineOffset = -1;
std::ptrdiff_t g_managerOffset = -1;
void* g_screenCtorTarget = nullptr;
void* g_scenePushTarget = nullptr;
bool g_initialized = false;
bool g_available = false;

ScreenState g_screen;
OffstackState g_offstack;
Listener* g_listener = nullptr;

// The block-use hook arms only the current game-thread call. The matching
// container controller must be constructed on that same thread.
thread_local bool t_expectedOpen = false;
thread_local void* t_constructedController = nullptr;

bool read_pointer(const void* address, void** output) noexcept {
    if (address == nullptr || output == nullptr) return false;
    __try {
        *output = *reinterpret_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_u8(const void* address, std::uint8_t* output) noexcept {
    if (address == nullptr || output == nullptr) return false;
    __try {
        *output = *reinterpret_cast<const std::uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_std_string(const void* address, char* output, std::size_t capacity) noexcept {
    if (address == nullptr || output == nullptr || capacity < 2) return false;
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(address);
        const std::size_t size = *reinterpret_cast<const std::size_t*>(bytes + 0x10);
        const std::size_t reserved = *reinterpret_cast<const std::size_t*>(bytes + 0x18);
        if (size == 0 || size >= capacity || size > reserved || reserved > 0x100000) return false;

        const char* source = reserved >= 16
            ? *reinterpret_cast<const char* const*>(bytes)
            : reinterpret_cast<const char*>(bytes);
        if (source == nullptr) return false;
        std::memcpy(output, source, size);
        output[size] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using ContainerModels = std::unordered_map<std::string, std::shared_ptr<void>>;
static_assert(sizeof(ContainerModels) == 64);

// The controller owns a weak_ptr<ContainerManagerModel> at offset zero. Hold a
// shared reference while the hidden screen is active so the model map stays alive.
bool adopt_manager_model(void* manager, std::shared_ptr<void>& owner) {
    if (manager == nullptr) return false;
    try {
        const auto* modelWeak = reinterpret_cast<const std::weak_ptr<void>*>(manager);
        owner = modelWeak->lock();
        return owner != nullptr;
    } catch (...) {
        owner.reset();
        return false;
    }
}

// mContainers is at +0xC8 in the 1.26.6005.0 ContainerManagerModel.
// Values are shared_ptr<ContainerModel>; shared_ptr<void> has the same MSVC layout.
__declspec(noinline) void* find_model_guarded(
    void* managerModel, const std::string* collection) noexcept {
    if (managerModel == nullptr || collection == nullptr) return nullptr;
    try {
        auto* models = reinterpret_cast<ContainerModels*>(
            static_cast<std::byte*>(managerModel) + kContainerModelsOffset);
        if (models->size() > 128) return nullptr;
        const auto it = models->find(*collection);
        return it != models->end() ? it->second.get() : nullptr;
    } catch (...) {
        return nullptr;
    }
}

void* find_container_model(std::string_view collection) {
    if (!g_screen.managerModelOwner) return nullptr;
    const std::string key(collection);
    return find_model_guarded(g_screen.managerModelOwner.get(), &key);
}

bool call_model_size(void* model, int* output) noexcept {
    if (model == nullptr || output == nullptr) return false;
    __try {
        void** vtable = *reinterpret_cast<void***>(model);
        if (vtable == nullptr || vtable[kContainerModelGetSizeSlot] == nullptr) return false;
        using GetSizeFn = int(__fastcall*)(void*);
        *output = reinterpret_cast<GetSizeFn>(vtable[kContainerModelGetSizeSlot])(model);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = 0;
        return false;
    }
}

bool call_model_item(void* model, int slot, const void** output) noexcept {
    if (model == nullptr || slot < 0 || output == nullptr) return false;
    __try {
        void** vtable = *reinterpret_cast<void***>(model);
        if (vtable == nullptr || vtable[kContainerModelGetItemStackSlot] == nullptr) return false;
        using GetItemFn = const void*(__fastcall*)(void*, int);
        *output = reinterpret_cast<GetItemFn>(vtable[kContainerModelGetItemStackSlot])(model, slot);
        return *output != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

bool call_stack_is_null(const void* stack, bool* output) noexcept {
    if (g_stackIsNull == nullptr || stack == nullptr || output == nullptr) return false;
    __try {
        *output = g_stackIsNull(stack);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_stack_max(const void* stack, int* output) noexcept {
    if (g_stackMaxSize == nullptr || stack == nullptr || output == nullptr) return false;
    __try {
        *output = static_cast<int>(g_stackMaxSize(stack));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_sm(void* stateMachine, std::uint32_t id, int state,
             const std::string* collection, int slot) noexcept {
    if (g_smHandle == nullptr || stateMachine == nullptr || collection == nullptr) return false;
    __try {
        g_smHandle(stateMachine, id, state, collection, slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool adopt_controller(void* controller) {
    if (controller == nullptr || g_stateMachineOffset <= 0 || g_managerOffset <= 0) {
        return false;
    }

    void* manager = nullptr;
    if (!read_pointer(static_cast<std::byte*>(controller) + g_managerOffset, &manager) || manager == nullptr) {
        return false;
    }

    std::shared_ptr<void> managerModelOwner;
    if (!adopt_manager_model(manager, managerModelOwner)) {
        return false;
    }

    void* stateMachine = static_cast<std::byte*>(controller) + g_stateMachineOffset;
    g_screen.controller = controller;
    g_screen.stateMachine = stateMachine;
    g_screen.manager = manager;
    g_screen.managerModelOwner = std::move(managerModelOwner);
    g_screen.emptyStack = nullptr;
    return true;
}

void* find_controller_tick(void* controller) noexcept {
    if (controller == nullptr || g_containerScreenTick == nullptr) return nullptr;
    __try {
        void** vtable = *reinterpret_cast<void***>(controller);
        if (vtable == nullptr) return nullptr;

        const auto target = reinterpret_cast<const std::uint8_t*>(g_containerScreenTick);
        for (int index = 0; index < 16; ++index) {
            const auto* function = static_cast<const std::uint8_t*>(vtable[index]);
            if (function == nullptr) continue;
            if (function == target) return const_cast<std::uint8_t*>(function);

            for (int offset = 0; offset + 5 <= 0x40; ++offset) {
                if (function[offset] != 0xE8) continue;
                std::int32_t relative = 0;
                std::memcpy(&relative, function + offset + 1, sizeof(relative));
                if (function + offset + 5 + relative == target) {
                    return const_cast<std::uint8_t*>(function);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

bool read_scene(void* scene, void** object, void** counts) noexcept {
    if (scene == nullptr || object == nullptr || counts == nullptr) return false;
    __try {
        *object = static_cast<void* const*>(scene)[0];
        *counts = static_cast<void* const*>(scene)[1];
        return *object != nullptr && *counts != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tick_controller(void* tick, void* controller) noexcept {
    if (tick == nullptr || controller == nullptr) return false;
    __try {
        reinterpret_cast<ControllerTickFn>(tick)(controller);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void release_shared_counts(void* counts) noexcept {
    if (counts == nullptr) return;
    __try {
        auto* bytes = static_cast<std::uint8_t*>(counts);
        void** vtable = *reinterpret_cast<void***>(bytes);
        if (vtable == nullptr) return;

        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(bytes + 8)) == 0) {
            using ReleaseFn = void(__fastcall*)(void*);
            reinterpret_cast<ReleaseFn>(vtable[0])(bytes);
            if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(bytes + 0x0C)) == 0) {
                reinterpret_cast<ReleaseFn>(vtable[1])(bytes);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void release_offstack_internal() {
    if (g_offstack.sceneObject == nullptr && g_offstack.sharedCounts == nullptr) return;

    const OffstackState held = g_offstack;
    g_offstack = {};
    g_screen = {};

    if (g_listener != nullptr) {
        g_listener->on_container_lost();
    }
    release_shared_counts(held.sharedCounts);
}

bool capture_offstack(void* scene, void* controller) {
    if (g_offstack.sceneObject != nullptr || !adopt_controller(controller)) return false;

    void* sceneObject = nullptr;
    void* counts = nullptr;
    if (!read_scene(scene, &sceneObject, &counts)) {
        g_screen = {};
        return false;
    }

    void* tick = find_controller_tick(controller);
    if (tick == nullptr) {
        g_screen = {};
        LOG_WARN("[ContainerUI] Could not resolve the container controller tick; leaving the UI visible.");
        return false;
    }

    g_offstack = {sceneObject, counts, controller, tick, GetCurrentThreadId()};
    LOG_INFO("[ContainerUI] Captured a container screen off the scene stack.");
    return true;
}

void* __fastcall hook_screen_ctor(void* self, void* a2, void* a3, void* a4) {
    void* result = g_screenCtor != nullptr ? g_screenCtor(self, a2, a3, a4) : self;
    if (t_expectedOpen && self != nullptr) {
        t_constructedController = self;
    }
    return result;
}

void __fastcall hook_scene_stack_push(void* stack, void* scene, void* a3, void* a4) {
    if (t_expectedOpen && t_constructedController != nullptr && g_offstack.sceneObject == nullptr) {
        void* controller = t_constructedController;
        if (capture_offstack(scene, controller)) {
            t_expectedOpen = false;
            t_constructedController = nullptr;
            if (g_listener != nullptr) {
                g_listener->on_container_opened(controller);
            }
            return; // Deliberately do not add this container screen to the visible scene stack.
        }
    }

    if (g_offstack.sceneObject != nullptr) {
        release_offstack_internal();
    }
    if (g_scenePush != nullptr) {
        g_scenePush(stack, scene, a3, a4);
    }
}

bool install_hook(void* target, void* detour, void** original, const char* label) {
    if (target == nullptr) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        LOG_WARN("[ContainerUI] Could not create {} hook.", label);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        *original = nullptr;
        LOG_WARN("[ContainerUI] Could not enable {} hook.", label);
        return false;
    }
    return true;
}

} // namespace

void set_listener(Listener* listener) {
    g_listener = listener;
}

bool initialize() {
    if (g_initialized) return g_available;
    g_initialized = true;

    const std::uintptr_t smHandle =
        minecraft::hook_memory::resolve_signature(BedrockSignatures::container_sm_handle);
    g_smHandle = reinterpret_cast<ContainerSmHandleFn>(smHandle);
    g_stackIsNull = reinterpret_cast<StackIsNullFn>(
        minecraft::hook_memory::resolve_signature(BedrockSignatures::item_stack_is_null));
    g_stackMaxSize = reinterpret_cast<StackMaxSizeFn>(
        minecraft::hook_memory::resolve_signature(BedrockSignatures::item_stack_max_stack_size));
    g_containerScreenTick = reinterpret_cast<ControllerTickFn>(
        minecraft::hook_memory::resolve_signature(BedrockSignatures::container_screen_tick));

    const std::uintptr_t smOffsetSite =
        minecraft::hook_memory::resolve_signature(BedrockSignatures::container_sm_offset_site);
    const std::uintptr_t managerOffsetSite =
        minecraft::hook_memory::resolve_signature(BedrockSignatures::container_manager_offset_site);
    const std::uintptr_t screenCtor =
        minecraft::hook_memory::resolve_signature(BedrockSignatures::container_screen_ctor);
    const std::uintptr_t scenePush =
        minecraft::hook_memory::resolve_signature(BedrockSignatures::scene_stack_push);

    if (smOffsetSite != 0) {
        __try {
            std::int32_t offset = 0;
            std::memcpy(&offset, reinterpret_cast<const void*>(smOffsetSite + 0x18), sizeof(offset));
            if (offset > 0 && offset < 0x4000 && (offset % 8) == 0) g_stateMachineOffset = offset;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_stateMachineOffset = -1;
        }
    }
    if (managerOffsetSite != 0) {
        __try {
            std::int32_t offset = 0;
            std::memcpy(&offset, reinterpret_cast<const void*>(managerOffsetSite + 3), sizeof(offset));
            if (offset > 0 && offset < 0x4000 && (offset % 8) == 0) g_managerOffset = offset;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_managerOffset = -1;
        }
    }

    if (g_smHandle == nullptr || g_stackIsNull == nullptr ||
        g_stackMaxSize == nullptr || g_containerScreenTick == nullptr ||
        g_stateMachineOffset < 0 || g_managerOffset < 0 ||
        screenCtor == 0 || scenePush == 0) {
        LOG_WARN("[ContainerUI] Required 1.26.60 container signatures/offsets were not resolved; transfer remains disabled.");
        return false;
    }

    g_screenCtorTarget = reinterpret_cast<void*>(screenCtor);
    g_scenePushTarget = reinterpret_cast<void*>(scenePush);

    if (!install_hook(g_screenCtorTarget, reinterpret_cast<void*>(&hook_screen_ctor),
                      reinterpret_cast<void**>(&g_screenCtor), "ContainerScreenCtor")) {
        return false;
    }
    if (!install_hook(g_scenePushTarget, reinterpret_cast<void*>(&hook_scene_stack_push),
                      reinterpret_cast<void**>(&g_scenePush), "SceneStackPush")) {
        MH_DisableHook(g_screenCtorTarget);
        MH_RemoveHook(g_screenCtorTarget);
        g_screenCtor = nullptr;
        g_screenCtorTarget = nullptr;
        return false;
    }

    g_available = true;
    LOG_INFO("[ContainerUI] Hidden container bridge initialized (state machine +{:#x}, manager +{:#x}).",
             g_stateMachineOffset, g_managerOffset);
    return true;
}

void shutdown() {
    release_offstack_internal();
    cancel_pending_open();

    if (g_scenePushTarget != nullptr) {
        MH_DisableHook(g_scenePushTarget);
        MH_RemoveHook(g_scenePushTarget);
    }
    if (g_screenCtorTarget != nullptr) {
        MH_DisableHook(g_screenCtorTarget);
        MH_RemoveHook(g_screenCtorTarget);
    }

    g_scenePushTarget = nullptr;
    g_screenCtorTarget = nullptr;
    g_scenePush = nullptr;
    g_screenCtor = nullptr;
    g_available = false;
    g_initialized = false;
}

bool available() {
    return g_available;
}

void arm_next_open() {
    if (!g_available) return;
    t_expectedOpen = true;
    t_constructedController = nullptr;
}

bool complete_user_open(bool gameAccepted) {
    const bool captured = g_offstack.sceneObject != nullptr &&
        g_offstack.controller != nullptr && g_offstack.threadId == GetCurrentThreadId();

    t_expectedOpen = false;
    t_constructedController = nullptr;

    if (!gameAccepted || !captured) {
        // prepare_block_use refuses to start while another hidden screen is active,
        // so any captured scene here belongs to this attempted click even if the
        // controller was created on a different thread.
        if (g_offstack.sceneObject != nullptr || g_offstack.sharedCounts != nullptr) {
            release_offstack_internal();
        }
        return false;
    }
    return true;
}

void cancel_pending_open() {
    t_expectedOpen = false;
    t_constructedController = nullptr;
}

bool has_hidden_screen() {
    return g_available && g_offstack.sceneObject != nullptr &&
        g_offstack.controller != nullptr && g_screen.manager != nullptr &&
        g_screen.stateMachine != nullptr;
}

void* screen_controller() {
    return has_hidden_screen() ? g_offstack.controller : nullptr;
}

bool tick_offstack_screen() {
    if (!has_hidden_screen()) return false;

    if (GetCurrentThreadId() != g_offstack.threadId) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARN("[ContainerUI] Hidden container belongs to another thread; waiting for its game thread.");
        }
        return true;
    }

    if (!tick_controller(g_offstack.tick, g_offstack.controller)) {
        LOG_WARN("[ContainerUI] Hidden container tick faulted; releasing it without further transfers.");
        release_offstack_internal();
        return false;
    }

    if (g_listener != nullptr) {
        g_listener->on_container_tick();
    }
    return true;
}

void release_offstack_screen() {
    release_offstack_internal();
}

int collection_size(std::string_view collection) {
    if (!has_hidden_screen()) return 0;

    void* model = find_container_model(collection);
    int size = 0;
    if (model == nullptr || !call_model_size(model, &size) ||
        size < 0 || size > kMaxCollectionSlots) {
        return 0;
    }
    return size;
}

const void* stack_at(std::string_view collection, int index) {
    if (!has_hidden_screen() || index < 0 || index >= collection_size(collection)) return nullptr;

    void* model = find_container_model(collection);
    const void* stack = nullptr;
    if (!call_model_item(model, index, &stack)) return nullptr;
    return stack == g_screen.emptyStack ? nullptr : stack;
}

bool is_empty(const void* stack) {
    if (stack == nullptr || stack == g_screen.emptyStack) return true;

    std::uint8_t count = 0;
    if (!read_u8(static_cast<const std::byte*>(stack) + kStackCountOffset, &count) || count == 0) {
        return true;
    }

    bool empty = true;
    return !call_stack_is_null(stack, &empty) || empty;
}

int count_of(const void* stack) {
    if (is_empty(stack)) return 0;
    std::uint8_t count = 0;
    return read_u8(static_cast<const std::byte*>(stack) + kStackCountOffset, &count)
        ? static_cast<int>(count) : 0;
}

int max_stack_of(const void* stack) {
    if (is_empty(stack)) return 0;
    int value = 0;
    if (!call_stack_max(stack, &value)) return 0;
    return value;
}

int net_tag_of(const void* stack) {
    if (is_empty(stack)) return -1;
    std::uint8_t value = 0;
    return read_u8(static_cast<const std::byte*>(stack) + kStackNetTagOffset, &value)
        ? static_cast<int>(value) : -1;
}

std::string item_name(const void* stack) {
    if (is_empty(stack)) return {};

    void* weakItem = nullptr;
    if (!read_pointer(static_cast<const std::byte*>(stack) + kStackItemOffset, &weakItem) ||
        weakItem == nullptr) return {};

    void* item = nullptr;
    if (!read_pointer(weakItem, &item) || item == nullptr) return {};

    char name[128]{};
    return read_std_string(static_cast<const std::byte*>(item) + kItemNameOffset, name, sizeof(name))
        ? std::string(name) : std::string{};
}

bool shift_click(std::string_view collection, int index) {
    if (!has_hidden_screen() || index < 0 || index >= collection_size(collection)) return false;
    const std::string name(collection);

    for (const int state : {kPressed, kHeld, kReleased}) {
        if (!call_sm(g_screen.stateMachine, kShiftClickButton, state, &name, index)) return false;
    }
    return true;
}

bool pending_predictions() {
    if (!has_hidden_screen()) return false;

    const auto hasPending = [](std::string_view collection) {
        const int size = collection_size(collection);
        for (int i = 0; i < size; ++i) {
            const void* stack = stack_at(collection, i);
            if (stack != nullptr && net_tag_of(stack) > 0) return true;
        }
        return false;
    };

    if (hasPending("container_items")) return true;
    if (hasPending("inventory_items")) return true;
    if (hasPending("hotbar_items")) return true;
    return hasPending("combined_hotbar_and_inventory_items");
}

} // namespace minecraft::container_ui
