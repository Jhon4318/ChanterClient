#include "minecraft/sdk/render/material_ptr.h"
#include "minecraft/hooks/hook_memory.h"
#include "minecraft/bedrock_memory.h"

using namespace minecraft;

MaterialPtr* MaterialPtr::getUIColor() {
    static MaterialPtr* resolved = nullptr;
    static bool attempted = false;

    if (!attempted) {
        attempted = true;
        std::uintptr_t sig = hook_memory::resolve_signature(BedrockSignatures::render_uiFillColor);
        if (sig) {
            auto addr = hook_memory::resolve_rip_relative(sig, 7, 11);
            if (addr) {
                resolved = reinterpret_cast<MaterialPtr*>(addr);
            }
        }
    }

    return resolved;
}
