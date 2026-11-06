#pragma once

#include <cstdint>
#include "minecraft/bedrock_memory.h"

class Tessellator;

namespace mce {
    struct Color {
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    };
}

class ScreenContext {
public:
    [[nodiscard]] mce::Color* getShaderColor() const {
        if (!this) return nullptr;
        __try {
            const auto addr = reinterpret_cast<std::uintptr_t>(this) + minecraft::BedrockOffsets::screenContext_shaderColor;
            if (addr < 0x10000) return nullptr;
            return *reinterpret_cast<mce::Color**>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    [[nodiscard]] Tessellator* getTessellator() const {
        if (!this) return nullptr;
        __try {
            const auto addr = reinterpret_cast<std::uintptr_t>(this) + minecraft::BedrockOffsets::screenContext_tessellator;
            if (addr < 0x10000) return nullptr;
            return *reinterpret_cast<Tessellator**>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static inline ScreenContext* s_instance3d = nullptr;
};
