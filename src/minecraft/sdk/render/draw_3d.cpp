#include "minecraft/sdk/render/draw_3d.h"
#include "minecraft/sdk/render/tessellator.h"
#include "minecraft/sdk/render/material_ptr.h"
#include "minecraft/sdk/render/mesh_helpers.h"
#include "minecraft/sdk/client/client_instance.h"

Draw3D::Draw3D() {
    m_screenContext = ScreenContext::s_instance3d;
    if (!m_screenContext) return;

    m_tessellator = m_screenContext->getTessellator();
    m_material = MaterialPtr::getUIColor();

    auto* ci = ClientInstance::get();
    if (!ci) return;
    auto* lr = ci->getLevelRenderer();
    if (!lr) return;
    auto* lrp = lr->getLevelRendererPlayer();
    if (!lrp) return;
    m_origin = lrp->getOrigin();
}

Draw3D::~Draw3D() {
    flush();
}

bool Draw3D::is_valid() const {
    return m_screenContext && m_tessellator && m_material;
}

void Draw3D::push_line(const math::Vec3& from, const math::Vec3& to, const mce::Color& color) {
    math::Vec3 a = from - m_origin;
    math::Vec3 b = to - m_origin;
    m_lineVerts.push_back({ a.x, a.y, a.z, color.r, color.g, color.b, color.a });
    m_lineVerts.push_back({ b.x, b.y, b.z, color.r, color.g, color.b, color.a });
}

void Draw3D::push_quad(const math::Vec3& a, const math::Vec3& b, const math::Vec3& c, const math::Vec3& d, const mce::Color& color) {
    math::Vec3 va = a - m_origin;
    math::Vec3 vb = b - m_origin;
    math::Vec3 vc = c - m_origin;
    math::Vec3 vd = d - m_origin;
    m_quadVerts.push_back({ va.x, va.y, va.z, color.r, color.g, color.b, color.a });
    m_quadVerts.push_back({ vb.x, vb.y, vb.z, color.r, color.g, color.b, color.a });
    m_quadVerts.push_back({ vc.x, vc.y, vc.z, color.r, color.g, color.b, color.a });
    m_quadVerts.push_back({ vd.x, vd.y, vd.z, color.r, color.g, color.b, color.a });
}

void Draw3D::draw_line(const math::Vec3& from, const math::Vec3& to, const mce::Color& color) {
    push_line(from, to, color);
}

void Draw3D::draw_box(const math::Vec3& min, const math::Vec3& max, const mce::Color& color) {
    math::Vec3 corners[8] = {
        { min.x, min.y, min.z }, { max.x, min.y, min.z },
        { max.x, min.y, max.z }, { min.x, min.y, max.z },
        { min.x, max.y, min.z }, { max.x, max.y, min.z },
        { max.x, max.y, max.z }, { min.x, max.y, max.z }
    };

    static constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };

    for (const auto& e : edges) {
        push_line(corners[e[0]], corners[e[1]], color);
    }
}

void Draw3D::fill_box(const math::Vec3& min, const math::Vec3& max, const mce::Color& color) {
    math::Vec3 c[8] = {
        { min.x, min.y, min.z }, { max.x, min.y, min.z },
        { max.x, min.y, max.z }, { min.x, min.y, max.z },
        { min.x, max.y, min.z }, { max.x, max.y, min.z },
        { max.x, max.y, max.z }, { min.x, max.y, max.z }
    };

    static constexpr int faces[6][4] = {
        {0,1,2,3}, {4,5,6,7},
        {0,1,5,4}, {3,2,6,7},
        {0,3,7,4}, {1,2,6,5}
    };

    for (const auto& f : faces) {
        push_quad(c[f[0]], c[f[1]], c[f[2]], c[f[3]], color);
        push_quad(c[f[3]], c[f[2]], c[f[1]], c[f[0]], color);
    }
}

void Draw3D::flush() {
    if (!is_valid()) {
        m_lineVerts.clear();
        m_quadVerts.clear();
        return;
    }

    auto* shaderColor = m_screenContext->getShaderColor();
    if (shaderColor) {
        *shaderColor = mce::Color{ 1.0f, 1.0f, 1.0f, 1.0f };
    }

    if (!m_quadVerts.empty()) {
        m_tessellator->begin(Primitive::Quad, static_cast<int>(m_quadVerts.size()));
        for (const auto& v : m_quadVerts) {
            m_tessellator->color(v.r, v.g, v.b, v.a);
            m_tessellator->vertex(v.x, v.y, v.z);
        }
        MeshHelpers::renderMeshImmediately(m_screenContext, m_tessellator, m_material);
        m_quadVerts.clear();
    }

    if (!m_lineVerts.empty()) {
        m_tessellator->begin(Primitive::LineList, static_cast<int>(m_lineVerts.size()));
        for (const auto& v : m_lineVerts) {
            m_tessellator->color(v.r, v.g, v.b, v.a);
            m_tessellator->vertex(v.x, v.y, v.z);
        }
        MeshHelpers::renderMeshImmediately(m_screenContext, m_tessellator, m_material);
        m_lineVerts.clear();
    }
}
