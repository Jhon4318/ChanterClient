#pragma once

#include <string>
#include <string_view>

enum class TextColor {
    Black,
    DarkBlue,
    DarkGreen,
    DarkAqua,
    DarkRed,
    DarkPurple,
    Gold,
    Gray,
    DarkGray,
    Blue,
    Green,
    Aqua,
    Red,
    LightPurple,
    Yellow,
    White,
    Obfuscated,
    Bold,
    Strikethrough,
    Underline,
    Italic,
    Reset
};

constexpr const char* color_code(TextColor color) {
    switch (color) {
    case TextColor::Black:         return "\u00A70";
    case TextColor::DarkBlue:      return "\u00A71";
    case TextColor::DarkGreen:     return "\u00A72";
    case TextColor::DarkAqua:      return "\u00A73";
    case TextColor::DarkRed:       return "\u00A74";
    case TextColor::DarkPurple:    return "\u00A75";
    case TextColor::Gold:          return "\u00A76";
    case TextColor::Gray:          return "\u00A77";
    case TextColor::DarkGray:      return "\u00A78";
    case TextColor::Blue:          return "\u00A79";
    case TextColor::Green:         return "\u00A7a";
    case TextColor::Aqua:          return "\u00A7b";
    case TextColor::Red:           return "\u00A7c";
    case TextColor::LightPurple:   return "\u00A7d";
    case TextColor::Yellow:        return "\u00A7e";
    case TextColor::White:         return "\u00A7f";
    case TextColor::Obfuscated:    return "\u00A7k";
    case TextColor::Bold:          return "\u00A7l";
    case TextColor::Strikethrough: return "\u00A7m";
    case TextColor::Underline:     return "\u00A7n";
    case TextColor::Italic:        return "\u00A7o";
    case TextColor::Reset:         return "\u00A7r";
    default:                       return "\u00A7r";
    }
}

inline std::string color(TextColor col, std::string_view text) {
    std::string result;
    result.reserve(text.size() + 6);
    result += color_code(col);
    result += text;
    result += "\u00A7r";
    return result;
}

namespace Color {
    inline constexpr const char* Black         = "\u00A70";
    inline constexpr const char* DarkBlue      = "\u00A71";
    inline constexpr const char* DarkGreen     = "\u00A72";
    inline constexpr const char* DarkAqua      = "\u00A73";
    inline constexpr const char* DarkRed       = "\u00A74";
    inline constexpr const char* DarkPurple    = "\u00A75";
    inline constexpr const char* Gold          = "\u00A76";
    inline constexpr const char* Gray          = "\u00A77";
    inline constexpr const char* DarkGray      = "\u00A78";
    inline constexpr const char* Blue          = "\u00A79";
    inline constexpr const char* Green         = "\u00A7a";
    inline constexpr const char* Aqua          = "\u00A7b";
    inline constexpr const char* Red           = "\u00A7c";
    inline constexpr const char* LightPurple   = "\u00A7d";
    inline constexpr const char* Yellow        = "\u00A7e";
    inline constexpr const char* White         = "\u00A7f";
    inline constexpr const char* Obfuscated    = "\u00A7k";
    inline constexpr const char* Bold          = "\u00A7l";
    inline constexpr const char* Strikethrough = "\u00A7m";
    inline constexpr const char* Underline     = "\u00A7n";
    inline constexpr const char* Italic        = "\u00A7o";
    inline constexpr const char* Reset         = "\u00A7r";
}
