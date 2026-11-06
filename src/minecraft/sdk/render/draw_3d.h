#pragma once

#include "utils/math.h"
#include "minecraft/sdk/render/screen_context.h"
#include "minecraft/sdk/render/material_ptr.h"
#include "minecraft/sdk/render/tessellator.h"
#include <vector>

class Draw3D {
public:
    Draw3D();
    ~Draw3D();

    [[nodiscard]] bool is_valid() const;

    void draw_line(const math::Vec3& from, const math::Vec3& to, const mce::Color& color);
    void draw_box(const math::Vec3& min, const math::Vec3& max, const mce::Color& color);
    void fill_box(const math::Vec3& min, const math::Vec3& max, const mce::Color& color);
    void flush();

private:
    struct ColorVertex {
        float x, y, z;
        float r, g, b, a;
    };

    ScreenContext* m_screenContext = nullptr;
    Tessellator* m_tessellator = nullptr;
    MaterialPtr* m_material = nullptr;
    math::Vec3 m_origin{};

    std::vector<ColorVertex> m_lineVerts;
    std::vector<ColorVertex> m_quadVerts;

    void push_line(const math::Vec3& from, const math::Vec3& to, const mce::Color& color);
    void push_quad(const math::Vec3& a, const math::Vec3& b, const math::Vec3& c, const math::Vec3& d, const mce::Color& color);
};
