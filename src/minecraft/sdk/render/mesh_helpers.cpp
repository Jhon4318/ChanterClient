#include "minecraft/sdk/render/mesh_helpers.h"
#include "minecraft/hooks/hook_memory.h"
#include "minecraft/bedrock_memory.h"

using namespace minecraft;

void MeshHelpers::renderMeshImmediately(ScreenContext* ctx, Tessellator* tess, MaterialPtr* material) {
    static std::uintptr_t func = hook_memory::resolve_signature(BedrockSignatures::render_meshImmediate);
    if (!func || !ctx || !tess || !material) return;

    char pad[0x58]{};
    using Fn = void(*)(ScreenContext*, Tessellator*, MaterialPtr*, char*);
    reinterpret_cast<Fn>(func)(ctx, tess, material, pad);
}
