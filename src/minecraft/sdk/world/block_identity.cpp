#include "minecraft/sdk/world/block_identity.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace minecraft::sdk::block_identity {
namespace {

// Bedrock SDK layout for this build:
// Block::mComponents occupies 0x08..0x67; Block::mBlockType is at +0x68.
// BlockType::mNameInfo starts at +0x90; NameInfo::mFullName.mStr is at +0xE8.
// The MSVC x64 std::string object stores size/capacity at +0x10/+0x18.
constexpr std::size_t kBlockTypePointerOffset = 0x68;
constexpr std::size_t kFullTypeNameStringOffset = 0xE8;
constexpr std::size_t kStringSizeOffset = 0x10;
constexpr std::size_t kStringCapacityOffset = 0x18;
constexpr std::size_t kSmallStringCapacity = 15;
constexpr std::size_t kMaxTypeNameLength = 127;

__declspec(noinline) bool guarded_copy_string(
    const void* block,
    char* output,
    std::size_t outputCapacity,
    std::size_t* outputLength) noexcept
{
    if (block == nullptr || output == nullptr || outputLength == nullptr ||
        reinterpret_cast<std::uintptr_t>(block) < 0x10000 ||
        outputCapacity <= kMaxTypeNameLength) {
        return false;
    }

    __try {
        const auto* blockBytes = static_cast<const std::uint8_t*>(block);
        const auto* blockType = *reinterpret_cast<void* const*>(
            blockBytes + kBlockTypePointerOffset);
        if (blockType == nullptr ||
            reinterpret_cast<std::uintptr_t>(blockType) < 0x10000) {
            return false;
        }

        const auto* stringObject = static_cast<const std::uint8_t*>(blockType) +
            kFullTypeNameStringOffset;
        const std::size_t length = *reinterpret_cast<const std::size_t*>(
            stringObject + kStringSizeOffset);
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(
            stringObject + kStringCapacityOffset);

        if (length == 0 || length > kMaxTypeNameLength ||
            length > capacity || capacity > 4096) {
            return false;
        }

        const char* source = capacity > kSmallStringCapacity
            ? *reinterpret_cast<const char* const*>(stringObject)
            : reinterpret_cast<const char*>(stringObject);
        if (source == nullptr ||
            reinterpret_cast<std::uintptr_t>(source) < 0x10000) {
            return false;
        }

        std::memcpy(output, source, length);
        output[length] = '\0';
        *outputLength = length;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

bool try_get_type_name(const void* block, std::string& out) {
    char buffer[128]{};
    std::size_t length = 0;
    if (!guarded_copy_string(block, buffer, sizeof(buffer), &length)) {
        return false;
    }

    out.assign(buffer, length);
    return !out.empty();
}

}
