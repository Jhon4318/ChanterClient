#pragma once

#include <memory>
#include <cstdint>

struct RenderMaterialInfo;

class MaterialPtr {
public:
    std::shared_ptr<RenderMaterialInfo> self;

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr& other) : self(other.self) {}
    MaterialPtr& operator=(const MaterialPtr& other) { self = other.self; return *this; }

    [[nodiscard]] void* info() const { return reinterpret_cast<void*>(self.get()); }

    static MaterialPtr* getUIColor();
};
