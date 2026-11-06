#pragma once

#include <cstdint>
#include <string_view>

namespace utils::hash {

    // Compile time 64 bit FNV-1a string hashing
    constexpr std::uint64_t fnv1a_64(const char* str) noexcept {
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        while (*str) {
            hash ^= static_cast<std::uint8_t>(*str++);
            hash *= 0x100000001B3ULL;
        }
        return hash;
    }

    constexpr std::uint64_t fnv1a_64(std::string_view str) noexcept {
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        for (char c : str) {
            hash ^= static_cast<std::uint8_t>(c);
            hash *= 0x100000001B3ULL;
        }
        return hash;
    }

    inline namespace literals {
        constexpr std::uint64_t operator""_hash(const char* str, std::size_t len) noexcept {
            return fnv1a_64(std::string_view{str, len});
        }
    }

} // namespace utils::hash
