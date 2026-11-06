#pragma once

class ScreenContext;
class Tessellator;
class MaterialPtr;

namespace MeshHelpers {
    void renderMeshImmediately(ScreenContext* ctx, Tessellator* tess, MaterialPtr* material);
}
