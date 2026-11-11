#include "skin/skin_capture_manager.h"
#include "skin/packet_abi.h"

#include "minecraft/hooks/hook_memory.h"
#include "utils/logger.h"
#include "utils/client_paths.h"

#include <Windows.h>
#include <wincodec.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minecraft::skin {

Bedrock::Result<void> __fastcall hook_packet_read(Packet* self, ReadOnlyBinaryStream& stream,
                                                   const cereal::ReflectionCtx& reflection);

namespace {

constexpr std::size_t kPacketReadVtableSlot = 9;
constexpr std::uint32_t kMaxSkinDimension = 2048;
constexpr std::uint32_t kMaxSkinBytes = 16 * 1024 * 1024;
constexpr std::uint32_t kMaxStringBytes = 2 * 1024 * 1024;
constexpr std::uint32_t kMaxAnimationCount = 256;
constexpr std::uint32_t kMaxPersonaEntries = 128;

using namespace Microsoft::WRL;

struct HookRecord {
    void** slot = nullptr;
    void* original = nullptr;
    int packetId = -1;
};

struct SkinSnapshot {
    std::string uuid;
    std::string playerName;
    std::uint64_t actorUniqueId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> skinRgba;
    std::uint32_t capeWidth = 0;
    std::uint32_t capeHeight = 0;
    std::vector<std::uint8_t> capeRgba;
    std::string geometryJson;
};

std::mutex g_mutex;
std::mutex g_fileMutex;
std::atomic_bool g_initialized{ false };
std::atomic_bool g_captureEnabled{ false };
std::unordered_map<std::string, SkinSnapshot> g_onlinePlayers;
std::vector<CapturedSkin> g_skins;
std::vector<HookRecord> g_hooks;
std::vector<std::shared_ptr<Packet>> g_packetKeepAlive;
PacketReadFn g_originalPlayerList = nullptr;
PacketReadFn g_originalPlayerSkin = nullptr;
void** g_playerListVtable = nullptr;
void** g_playerSkinVtable = nullptr;
bool g_libraryLoaded = false;

class Cursor {
public:
    explicit Cursor(std::string_view bytes) : m_bytes(bytes) {}

    bool read_u8(std::uint8_t& out) {
        if (m_offset >= m_bytes.size()) return false;
        out = static_cast<std::uint8_t>(m_bytes[m_offset++]);
        return true;
    }

    bool read_u32(std::uint32_t& out) {
        if (remaining() < 4) return false;
        const auto* p = reinterpret_cast<const std::uint8_t*>(m_bytes.data() + m_offset);
        out = static_cast<std::uint32_t>(p[0]) |
              (static_cast<std::uint32_t>(p[1]) << 8) |
              (static_cast<std::uint32_t>(p[2]) << 16) |
              (static_cast<std::uint32_t>(p[3]) << 24);
        m_offset += 4;
        return true;
    }

    bool read_varuint32(std::uint32_t& out) {
        out = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            std::uint8_t byte = 0;
            if (!read_u8(byte)) return false;
            if (shift == 28 && (byte & 0xF0U) != 0) return false;
            out |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0) return true;
        }
        return false;
    }

    bool read_varint64(std::uint64_t& out) {
        std::uint64_t encoded = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            std::uint8_t byte = 0;
            if (!read_u8(byte)) return false;
            if (shift == 63 && (byte & 0xFEU) != 0) return false;
            encoded |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0) {
                const std::uint64_t signMask = 0ULL - (encoded & 1ULL);
                const std::int64_t decoded = static_cast<std::int64_t>((encoded >> 1) ^ signMask);
                out = static_cast<std::uint64_t>(decoded);
                return true;
            }
        }
        return false;
    }

    bool read_string(std::string& out) {
        std::uint32_t length = 0;
        if (!read_varuint32(length) || length > kMaxStringBytes || length > remaining()) return false;
        out.assign(m_bytes.data() + m_offset, length);
        m_offset += length;
        return true;
    }

    bool read_uuid(std::string& out) {
        if (remaining() < 16) return false;

        // Bedrock serializes mce::UUID as two little-endian uint64 values.
        // Emit the conventional UUID hex order so a pasted UUID can be filtered.
        std::uint64_t mostSignificant = 0;
        std::uint64_t leastSignificant = 0;
        for (unsigned i = 0; i < 8; ++i) {
            mostSignificant |= static_cast<std::uint64_t>(
                static_cast<unsigned char>(m_bytes[m_offset++])) << (i * 8);
        }
        for (unsigned i = 0; i < 8; ++i) {
            leastSignificant |= static_cast<std::uint64_t>(
                static_cast<unsigned char>(m_bytes[m_offset++])) << (i * 8);
        }

        static constexpr char hex[] = "0123456789abcdef";
        out.clear();
        out.reserve(32);
        const auto append64 = [&](std::uint64_t value) {
            for (int shift = 60; shift >= 0; shift -= 4) {
                out.push_back(hex[(value >> shift) & 0xFU]);
            }
        };
        append64(mostSignificant);
        append64(leastSignificant);
        return true;
    }

    bool read_image(std::uint32_t& width, std::uint32_t& height,
                    std::vector<std::uint8_t>& pixels, bool allowEmpty = false) {
        if (!read_u32(width) || !read_u32(height)) return false;

        std::uint32_t length = 0;
        if (!read_varuint32(length)) return false;

        if (width == 0 || height == 0) {
            if (allowEmpty && width == 0 && height == 0 && length == 0) {
                pixels.clear();
                return true;
            }
            return false;
        }
        if (width > kMaxSkinDimension || height > kMaxSkinDimension) return false;

        const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4ULL;
        if (expected > kMaxSkinBytes || length != expected || length > remaining()) return false;

        const auto* first = reinterpret_cast<const std::uint8_t*>(m_bytes.data() + m_offset);
        pixels.assign(first, first + length);
        m_offset += length;
        return true;
    }

    bool skip_image() {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t length = 0;
        if (!read_u32(width) || !read_u32(height) || !read_varuint32(length)) return false;

        if (width == 0 || height == 0) {
            return width == 0 && height == 0 && length == 0;
        }
        if (width > kMaxSkinDimension || height > kMaxSkinDimension) return false;

        const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4ULL;
        if (expected > kMaxSkinBytes || length != expected || length > remaining()) return false;
        m_offset += length;
        return true;
    }

    bool skip(std::size_t count) {
        if (count > remaining()) return false;
        m_offset += count;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const {
        return m_offset <= m_bytes.size() ? m_bytes.size() - m_offset : 0;
    }

private:
    std::string_view m_bytes;
    std::size_t m_offset = 0;
};

std::string normalize_uuid(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return c == '-' || c == '{' || c == '}' || std::isspace(c);
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::filesystem::path get_skin_directory() {
    return utils::client_data_directory(L"skins", std::filesystem::temp_directory_path());
}

std::string sanitize_folder_name(std::string value) {
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            ch = '_';
        }
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) value.pop_back();
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (value.empty() || value == "." || value == "..") value = "Player";
    if (value.size() > 64) value.resize(64);

    std::string upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
        upper == "COM1" || upper == "COM2" || upper == "COM3" || upper == "COM4" ||
        upper == "COM5" || upper == "COM6" || upper == "COM7" || upper == "COM8" ||
        upper == "COM9" || upper == "LPT1" || upper == "LPT2" || upper == "LPT3" ||
        upper == "LPT4" || upper == "LPT5" || upper == "LPT6" || upper == "LPT7" ||
        upper == "LPT8" || upper == "LPT9") {
        value.insert(value.begin(), '_');
    }
    return value;
}

bool read_serialized_skin(Cursor& cursor, SkinSnapshot& snapshot) {
    std::string skinId;
    std::string resourcePatch;
    if (!cursor.read_string(skinId) || !cursor.read_string(resourcePatch)) return false;

    if (!cursor.read_image(snapshot.width, snapshot.height, snapshot.skinRgba)) return false;

    std::uint32_t animationCount = 0;
    if (!cursor.read_varuint32(animationCount) || animationCount > kMaxAnimationCount) return false;
    for (std::uint32_t i = 0; i < animationCount; ++i) {
        std::uint32_t animationType = 0;
        std::uint32_t expression = 0;
        if (!cursor.skip_image() || !cursor.read_varuint32(animationType) ||
            !cursor.skip(4) || !cursor.read_varuint32(expression)) {
            return false;
        }
    }

    if (!cursor.read_image(snapshot.capeWidth, snapshot.capeHeight, snapshot.capeRgba, true)) return false;

    std::string geometryVersion;
    std::string animationData;
    std::string capeId;
    std::string fullSkinId;
    if (!cursor.read_string(snapshot.geometryJson) ||
        !cursor.read_string(geometryVersion) ||
        !cursor.read_string(animationData) ||
        !cursor.read_string(capeId) ||
        !cursor.read_string(fullSkinId)) {
        return false;
    }

    std::uint8_t armSize = 0;
    std::uint32_t skinColor = 0;
    if (!cursor.read_u8(armSize) || !cursor.read_u32(skinColor)) return false;

    std::uint32_t personaPieceCount = 0;
    if (!cursor.read_varuint32(personaPieceCount) || personaPieceCount > kMaxPersonaEntries) return false;
    for (std::uint32_t i = 0; i < personaPieceCount; ++i) {
        std::string pieceId;
        std::string productId;
        std::uint32_t pieceType = 0;
        std::string packUuid;
        std::uint8_t isDefault = 0;
        if (!cursor.read_string(pieceId) || !cursor.read_u32(pieceType) ||
            !cursor.read_uuid(packUuid) || !cursor.read_u8(isDefault) ||
            !cursor.read_string(productId)) {
            return false;
        }
    }

    std::uint32_t tintCount = 0;
    if (!cursor.read_varuint32(tintCount) || tintCount > kMaxPersonaEntries) return false;
    for (std::uint32_t i = 0; i < tintCount; ++i) {
        std::string pieceType;
        if (!cursor.read_string(pieceType)) return false;
        for (unsigned color = 0; color < 4; ++color) {
            std::uint32_t argb = 0;
            if (!cursor.read_u32(argb)) return false;
        }
    }

    // Premium, persona, cape-on-classic, primary-user and override-appearance flags.
    for (unsigned i = 0; i < 5; ++i) {
        std::uint8_t flag = 0;
        if (!cursor.read_u8(flag)) return false;
    }

    std::string trustedFlag;
    std::string profileHash;
    if (!cursor.read_string(trustedFlag) || !cursor.read_string(profileHash)) return false;

    return !snapshot.skinRgba.empty();
}

void merge_player_snapshot(SkinSnapshot snapshot) {
    std::lock_guard lock(g_mutex);
    auto found = g_onlinePlayers.find(snapshot.uuid);
    if (found != g_onlinePlayers.end()) {
        if (snapshot.playerName.empty() || snapshot.playerName == "Unknown") {
            snapshot.playerName = found->second.playerName;
        }
        if (snapshot.actorUniqueId == 0) {
            snapshot.actorUniqueId = found->second.actorUniqueId;
        }
        if (snapshot.skinRgba.empty() && !found->second.skinRgba.empty()) {
            snapshot.width = found->second.width;
            snapshot.height = found->second.height;
            snapshot.skinRgba = found->second.skinRgba;
            snapshot.capeWidth = found->second.capeWidth;
            snapshot.capeHeight = found->second.capeHeight;
            snapshot.capeRgba = found->second.capeRgba;
            snapshot.geometryJson = found->second.geometryJson;
        }
    }
    if (snapshot.playerName.empty()) snapshot.playerName = "Unknown";
    g_onlinePlayers.insert_or_assign(snapshot.uuid, std::move(snapshot));
}

void remove_online_player(const std::string& uuid) {
    std::lock_guard lock(g_mutex);
    g_onlinePlayers.erase(uuid);
}

bool write_png(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
               const std::vector<std::uint8_t>& rgba) {
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4ULL;
    if (width == 0 || height == 0 || expected != rgba.size() || expected > kMaxSkinBytes) return false;

    std::vector<std::uint8_t> bgra(rgba.size());
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        bgra[i] = rgba[i + 2];
        bgra[i + 1] = rgba[i + 1];
        bgra[i + 2] = rgba[i];
        bgra[i + 3] = rgba[i + 3];
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = comResult == S_OK || comResult == S_FALSE;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixelFormat);
    if (SUCCEEDED(hr) && pixelFormat == GUID_WICPixelFormat32bppBGRA) {
        hr = frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data());
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    if (uninitializeCom) CoUninitialize();
    return SUCCEEDED(hr);
}

std::uint32_t next_snapshot_index(const std::filesystem::path& playerDirectory) {
    std::uint32_t maximum = 0;
    std::error_code ec;
    if (!std::filesystem::exists(playerDirectory, ec) || ec) return 1;

    for (const auto& entry : std::filesystem::directory_iterator(playerDirectory, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec) || ec) continue;

        const std::string name = entry.path().filename().string();
        std::uint32_t parsed = 0;
        const auto result = std::from_chars(name.data(), name.data() + name.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == name.data() + name.size()) {
            maximum = std::max(maximum, parsed);
        }
    }
    return maximum == std::numeric_limits<std::uint32_t>::max() ? maximum : maximum + 1;
}

void load_saved_library() {
    std::lock_guard fileLock(g_fileMutex);
    if (g_libraryLoaded) return;

    std::vector<CapturedSkin> loaded;
    const auto root = get_skin_directory();
    std::error_code ec;
    if (std::filesystem::exists(root, ec) && !ec) {
        for (const auto& playerEntry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!playerEntry.is_directory(ec) || ec) continue;

            for (const auto& snapshotEntry : std::filesystem::directory_iterator(playerEntry.path(), ec)) {
                if (ec) break;
                if (!snapshotEntry.is_directory(ec) || ec) continue;

                const auto metadataPath = snapshotEntry.path() / "metadata.json";
                const auto skinPath = snapshotEntry.path() / "skin.png";
                if (!std::filesystem::is_regular_file(metadataPath, ec) || ec ||
                    !std::filesystem::is_regular_file(skinPath, ec) || ec) {
                    continue;
                }

                try {
                    std::ifstream input(metadataPath);
                    nlohmann::json metadata;
                    input >> metadata;

                    CapturedSkin saved;
                    saved.uuid = metadata.value("uuid", std::string{});
                    saved.player_name = metadata.value("player_name", std::string("Unknown"));
                    saved.index = metadata.value("index", 0U);
                    saved.width = metadata.value("width", 0U);
                    saved.height = metadata.value("height", 0U);
                    saved.has_cape = metadata.value("has_cape", false);
                    saved.has_geometry = metadata.value("has_geometry", false);
                    saved.file_path = skinPath;
                    if (!saved.uuid.empty()) loaded.push_back(std::move(saved));
                } catch (...) {
                    // Ignore incomplete or manually edited metadata files.
                }
            }
        }
    }

    std::sort(loaded.begin(), loaded.end(), [](const CapturedSkin& left, const CapturedSkin& right) {
        if (left.player_name != right.player_name) return left.player_name < right.player_name;
        return left.index < right.index;
    });

    {
        std::lock_guard lock(g_mutex);
        g_skins = std::move(loaded);
    }
    g_libraryLoaded = true;
}

void record_player_list(std::string_view body) {
    Cursor cursor(body);
    std::uint32_t count = 0;
    if (!cursor.read_varuint32(count) || count > 4096) return;

    // Protocol 2225 (Bedrock 1.26.60): each entry starts with a variant selector,
    // then an Action byte and UUID. Add entries carry PlayFab ID before Platform Online ID.
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t variant = 0;
        std::uint8_t action = 0;
        std::string uuid;
        if (!cursor.read_varuint32(variant) || !cursor.read_u8(action) || !cursor.read_uuid(uuid)) return;

        if (action == 1 && variant == 0) {
            remove_online_player(uuid);
            continue;
        }
        if (action != 0 || variant != 1) return;

        SkinSnapshot snapshot;
        snapshot.uuid = uuid;
        std::string xuid;
        std::string playFabId;
        std::string platformOnlineId;
        std::uint32_t buildPlatform = 0;
        if (!cursor.read_varint64(snapshot.actorUniqueId) ||
            !cursor.read_string(snapshot.playerName) ||
            !cursor.read_string(xuid) ||
            !cursor.read_string(playFabId) ||
            !cursor.read_string(platformOnlineId) ||
            !cursor.read_u32(buildPlatform)) {
            return;
        }

        if (!read_serialized_skin(cursor, snapshot)) {
            merge_player_snapshot(std::move(snapshot));
            return;
        }

        // PlayerListAddEntry tail: IsTeacher, IsHost, IsSubClient and PlayerColor.
        for (unsigned flagIndex = 0; flagIndex < 3; ++flagIndex) {
            std::uint8_t flag = 0;
            if (!cursor.read_u8(flag)) return;
        }
        std::uint32_t playerColor = 0;
        if (!cursor.read_u32(playerColor)) return;

        merge_player_snapshot(std::move(snapshot));
    }
}

void record_player_skin(std::string_view body) {
    Cursor cursor(body);
    SkinSnapshot snapshot;
    if (!cursor.read_uuid(snapshot.uuid) || !read_serialized_skin(cursor, snapshot)) return;

    std::string newSkinName;
    std::string oldSkinName;
    if (!cursor.read_string(newSkinName) || !cursor.read_string(oldSkinName)) return;

    merge_player_snapshot(std::move(snapshot));
}

bool read_packet_vtable(Packet* packet, void*** output) noexcept {
    if (packet == nullptr || output == nullptr) return false;
    __try {
        *output = *reinterpret_cast<void***>(packet);
        return *output != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

void** get_vtable(Packet* packet) noexcept {
    void** table = nullptr;
    return read_packet_vtable(packet, &table) ? table : nullptr;
}

bool patch_packet_vtable(const std::shared_ptr<Packet>& packet, int expectedId) {
    if (!packet || static_cast<int>(packet->getId()) != expectedId) return false;
    void** vtable = get_vtable(packet.get());
    if (!vtable || !vtable[kPacketReadVtableSlot]) return false;
    if (vtable[kPacketReadVtableSlot] == reinterpret_cast<void*>(&hook_packet_read)) return true;

    if (expectedId == static_cast<int>(MinecraftPacketIds::PlayerList)) {
        g_playerListVtable = vtable;
    } else if (expectedId == static_cast<int>(MinecraftPacketIds::PlayerSkin)) {
        g_playerSkinVtable = vtable;
    }

    DWORD oldProtect = 0;
    void** slot = &vtable[kPacketReadVtableSlot];
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

    void* original = *slot;
    *slot = reinterpret_cast<void*>(&hook_packet_read);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);

    g_hooks.push_back({ slot, original, expectedId });
    if (expectedId == static_cast<int>(MinecraftPacketIds::PlayerList)) {
        g_originalPlayerList = reinterpret_cast<PacketReadFn>(original);
    } else if (expectedId == static_cast<int>(MinecraftPacketIds::PlayerSkin)) {
        g_originalPlayerSkin = reinterpret_cast<PacketReadFn>(original);
    }
    g_packetKeepAlive.push_back(packet);
    return true;
}

} // namespace

Bedrock::Result<void> __fastcall hook_packet_read(Packet* self, ReadOnlyBinaryStream& stream,
                                                   const cereal::ReflectionCtx& reflection) {
    void** incomingVtable = nullptr;
    if (!read_packet_vtable(self, &incomingVtable)) {
        static std::atomic_bool warnedInvalidPacket{ false };
        if (!warnedInvalidPacket.exchange(true, std::memory_order_acq_rel)) {
            LOG_WARN("[SkinStealer] Ignored a packet-read call with an unreadable packet/vtable pointer.");
        }
        return Bedrock::Result<void>{};
    }

    int packetId = -1;
    PacketReadFn original = nullptr;
    if (incomingVtable == g_playerListVtable) {
        packetId = static_cast<int>(MinecraftPacketIds::PlayerList);
        original = g_originalPlayerList;
    } else if (incomingVtable == g_playerSkinVtable) {
        packetId = static_cast<int>(MinecraftPacketIds::PlayerSkin);
        original = g_originalPlayerSkin;
    }
    if (!original) return Bedrock::Result<void>{};

    std::string body;
    try {
        const auto view = stream.getView();
        const auto offset = stream.getReadPointer();
        if (offset <= view.size()) body.assign(view.data() + offset, view.size() - offset);
    } catch (...) {
        body.clear();
    }

    Bedrock::Result<void> result = original(self, stream, reflection);
    if (!result.asExpected().has_value() || body.empty() ||
        !g_captureEnabled.load(std::memory_order_acquire)) {
        return result;
    }

    try {
        if (packetId == static_cast<int>(MinecraftPacketIds::PlayerList)) {
            record_player_list(body);
        } else if (packetId == static_cast<int>(MinecraftPacketIds::PlayerSkin)) {
            record_player_skin(body);
        }
    } catch (...) {
        LOG_ERROR("[SkinStealer] Exception while processing a Bedrock player-skin packet.");
    }
    return result;
}

bool initialize() {
    if (g_initialized.load(std::memory_order_acquire)) return true;

    constexpr std::string_view pattern_126605 =
        "56 48 83 EC 20 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 8D 0D ? ? ? ? "
        "48 63 04 81 48 01 C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 20 5E";
    constexpr std::string_view pattern_1266021 =
        "56 48 83 EC 30 48 89 CE 81 FA ? 01 00 00 0F 87 ? ? ? ? "
        "89 D0 48 8D 0D ? ? ? ? 48 63 04 81 48 01 "
        "C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 30 5E";
    constexpr std::string_view pattern_1266030 =
        "56 48 83 EC 30 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 8D 0D ? ? ? ? "
        "48 63 04 81 48 01 C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 30 5E";

    std::uintptr_t factoryAddress = minecraft::hook_memory::find_signature(pattern_126605);
    if (!factoryAddress) factoryAddress = minecraft::hook_memory::find_signature(pattern_1266021);
    if (!factoryAddress) factoryAddress = minecraft::hook_memory::find_signature(pattern_1266030);
    if (!factoryAddress) {
        LOG_ERROR("[SkinStealer] Player packet factory signature not found; skin capture is unavailable on this client build.");
        return false;
    }

    const auto factory = reinterpret_cast<CreatePacketFn>(factoryAddress);
    auto playerList = factory(MinecraftPacketIds::PlayerList);
    auto playerSkin = factory(MinecraftPacketIds::PlayerSkin);
    if (!playerList || !playerSkin ||
        static_cast<int>(playerList->getId()) != 63 ||
        static_cast<int>(playerSkin->getId()) != 93) {
        LOG_ERROR("[SkinStealer] Packet factory validation failed; no packet hooks were installed.");
        return false;
    }

    std::lock_guard lock(g_mutex);
    const bool listOk = patch_packet_vtable(playerList, 63);
    const bool skinOk = patch_packet_vtable(playerSkin, 93);
    if (!listOk || !skinOk) {
        for (auto& hook : g_hooks) {
            if (hook.slot && hook.original) {
                DWORD oldProtect = 0;
                if (VirtualProtect(hook.slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    *hook.slot = hook.original;
                    DWORD ignored = 0;
                    VirtualProtect(hook.slot, sizeof(void*), oldProtect, &ignored);
                }
            }
        }
        g_hooks.clear();
        g_packetKeepAlive.clear();
        g_originalPlayerList = nullptr;
        g_originalPlayerSkin = nullptr;
        g_playerListVtable = nullptr;
        g_playerSkinVtable = nullptr;
        LOG_ERROR("[SkinStealer] Could not install the player packet hooks. Capture disabled.");
        return false;
    }

    g_initialized.store(true, std::memory_order_release);
    LOG_INFO("[SkinStealer] Bedrock PlayerList (63) and PlayerSkin (93) packet hooks installed.");
    return true;
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    g_captureEnabled.store(false, std::memory_order_release);
    for (auto& hook : g_hooks) {
        if (!hook.slot || !hook.original) continue;
        DWORD oldProtect = 0;
        if (VirtualProtect(hook.slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *hook.slot = hook.original;
            DWORD ignored = 0;
            VirtualProtect(hook.slot, sizeof(void*), oldProtect, &ignored);
        }
    }
    g_hooks.clear();
    g_packetKeepAlive.clear();
    g_originalPlayerList = nullptr;
    g_originalPlayerSkin = nullptr;
    g_playerListVtable = nullptr;
    g_playerSkinVtable = nullptr;
    g_onlinePlayers.clear();
    g_initialized.store(false, std::memory_order_release);
    LOG_INFO("[SkinStealer] Player packet hooks removed.");
}

void set_capture_enabled(bool enabled) {
    g_captureEnabled.store(enabled, std::memory_order_release);
}

std::vector<OnlinePlayer> online_players() {
    std::vector<OnlinePlayer> result;
    std::lock_guard lock(g_mutex);
    result.reserve(g_onlinePlayers.size());
    for (const auto& [uuid, skin] : g_onlinePlayers) {
        OnlinePlayer player;
        player.uuid = uuid;
        player.player_name = skin.playerName.empty() ? "Unknown" : skin.playerName;
        player.actor_unique_id = skin.actorUniqueId;
        player.width = skin.width;
        player.height = skin.height;
        player.has_skin = !skin.skinRgba.empty();
        player.has_cape = !skin.capeRgba.empty();
        player.has_geometry = !skin.geometryJson.empty();
        result.push_back(std::move(player));
    }
    std::sort(result.begin(), result.end(), [](const OnlinePlayer& left, const OnlinePlayer& right) {
        return left.player_name < right.player_name;
    });
    return result;
}

std::vector<CapturedSkin> captured_skins() {
    load_saved_library();
    std::lock_guard lock(g_mutex);
    return g_skins;
}

bool save_player_skin(const std::string& requestedUuid) {
    load_saved_library();

    const std::string uuid = normalize_uuid(requestedUuid);
    SkinSnapshot snapshot;
    {
        std::lock_guard lock(g_mutex);
        const auto found = g_onlinePlayers.find(uuid);
        if (found == g_onlinePlayers.end() || found->second.skinRgba.empty()) return false;
        snapshot = found->second;
    }

    std::lock_guard fileLock(g_fileMutex);
    const auto root = get_skin_directory();
    const auto playerDirectory = root /
        (sanitize_folder_name(snapshot.playerName) + "_" + sanitize_folder_name(uuid));

    std::error_code ec;
    std::filesystem::create_directories(playerDirectory, ec);
    if (ec) {
        LOG_ERROR("[SkinStealer] Failed to create the player skin directory: {}", ec.message());
        return false;
    }

    std::uint32_t index = next_snapshot_index(playerDirectory);
    std::filesystem::path snapshotDirectory;
    bool created = false;
    for (unsigned attempt = 0; attempt < 10000 && index != 0; ++attempt, ++index) {
        snapshotDirectory = playerDirectory / std::format("{:03}", index);
        ec.clear();
        if (std::filesystem::create_directory(snapshotDirectory, ec)) {
            created = true;
            break;
        }
        if (ec && ec != std::errc::file_exists) {
            LOG_ERROR("[SkinStealer] Failed to create a numbered snapshot folder: {}", ec.message());
            return false;
        }
    }
    if (!created) {
        LOG_ERROR("[SkinStealer] Could not allocate a new snapshot number for {}.", snapshot.playerName);
        return false;
    }

    const auto skinPath = snapshotDirectory / "skin.png";
    if (!write_png(skinPath, snapshot.width, snapshot.height, snapshot.skinRgba)) {
        std::filesystem::remove_all(snapshotDirectory, ec);
        LOG_ERROR("[SkinStealer] Failed to export the skin image for {}.", snapshot.playerName);
        return false;
    }

    bool hasCape = false;
    if (!snapshot.capeRgba.empty() && snapshot.capeWidth > 0 && snapshot.capeHeight > 0) {
        const auto capePath = snapshotDirectory / "cape.png";
        if (!write_png(capePath, snapshot.capeWidth, snapshot.capeHeight, snapshot.capeRgba)) {
            std::filesystem::remove_all(snapshotDirectory, ec);
            LOG_ERROR("[SkinStealer] Failed to export the cape image for {}.", snapshot.playerName);
            return false;
        }
        hasCape = true;
    }

    bool hasGeometry = false;
    if (!snapshot.geometryJson.empty()) {
        std::ofstream geometryFile(snapshotDirectory / "geometry.json", std::ios::binary);
        geometryFile.write(snapshot.geometryJson.data(),
                           static_cast<std::streamsize>(snapshot.geometryJson.size()));
        if (!geometryFile.good()) {
            std::filesystem::remove_all(snapshotDirectory, ec);
            LOG_ERROR("[SkinStealer] Failed to export geometry data for {}.", snapshot.playerName);
            return false;
        }
        hasGeometry = true;
    }

    nlohmann::json metadata = {
        {"uuid", snapshot.uuid},
        {"player_name", snapshot.playerName},
        {"index", index},
        {"width", snapshot.width},
        {"height", snapshot.height},
        {"has_cape", hasCape},
        {"has_geometry", hasGeometry}
    };
    {
        std::ofstream metadataFile(snapshotDirectory / "metadata.json", std::ios::binary);
        metadataFile << metadata.dump(2);
        if (!metadataFile.good()) {
            std::filesystem::remove_all(snapshotDirectory, ec);
            LOG_ERROR("[SkinStealer] Failed to write snapshot metadata for {}.", snapshot.playerName);
            return false;
        }
    }

    CapturedSkin saved;
    saved.uuid = snapshot.uuid;
    saved.player_name = snapshot.playerName;
    saved.file_path = skinPath;
    saved.width = snapshot.width;
    saved.height = snapshot.height;
    saved.index = index;
    saved.has_cape = hasCape;
    saved.has_geometry = hasGeometry;
    {
        std::lock_guard lock(g_mutex);
        g_skins.push_back(saved);
        std::sort(g_skins.begin(), g_skins.end(), [](const CapturedSkin& left, const CapturedSkin& right) {
            if (left.player_name != right.player_name) return left.player_name < right.player_name;
            return left.index < right.index;
        });
    }

    LOG_INFO("[SkinStealer] Saved {} snapshot #{:03} ({}x{}) to {}.",
        snapshot.playerName, index, snapshot.width, snapshot.height, snapshotDirectory.string());
    return true;
}

std::filesystem::path skins_directory() {
    return get_skin_directory();
}

void open_skins_directory() {
    const auto directory = get_skin_directory();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace minecraft::skin
