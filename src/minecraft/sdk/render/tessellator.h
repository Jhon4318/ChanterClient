#pragma once

#include "minecraft/sdk/render/screen_context.h"

enum class Primitive : int {
    Quad = 1,
    TriangleFan = 2,
    TriangleStrip = 3,
    LineList = 4
};

class Tessellator {
public:
    void vertex(float x, float y, float z);
    void begin(Primitive primitive, int numVertices = 0);
    void color(float r, float g, float b, float a);
    void color(const mce::Color& col);
};
