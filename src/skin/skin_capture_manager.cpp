#include "skin/skin_capture_manager.h"
#include "skin/packet_abi.h"

#include "minecraft/hooks/hook_memory.h"
#include "utils/logger.h"

#include <Windows.h>
#include <wincodec.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
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

using namespace Microsoft::WRL;

struct HookRecord {
    void** slot = nullptr;
    void* original = nullptr;
    int packetId = -1;
};

std::mutex g_mutex;
std::mutex g_fileMutex;
std::atomic_bool g_initialized{false};
std::atomic_bool g_captureEnabled{false};
std::atomic_bool g_targetFilterEnabled{false};
std::string g_targetUuid;
std::unordered_map<std::string, std::string> g_playerNames;
std::vector<CapturedSkin> g_skins;
std::vector<HookRecord> g_hooks;
std::vector<std::shared_ptr<Packet>> g_packetKeepAlive;
PacketReadFn g_originalPlayerList = nullptr;
PacketReadFn g_originalPlayerSkin = nullptr;

class Cursor {
public:
    explicit Cursor(std::string_view bytes) : m_bytes(bytes) {}

    bool read_u8(std::uint8_t& out) {
        if (m_offset >= m_bytes.size()) return false;
        out = static_cast<std::uint8_t>(m_bytes[m_offset++]);
        return true;
    }

    bool read_u32(std::uint32_t& out) {
        if (m_bytes.size() - m_offset < 4) return false;
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

    bool read_varuint64() {
        for (unsigned i = 0; i < 10; ++i) {
            std::uint8_t byte = 0;
            if (!read_u8(byte)) return false;
            if (i == 9 && (byte & 0xFEU) != 0) return false;
            if ((byte & 0x80U) == 0) return true;
        }
        return false;
    }

    bool read_string(std::string& out) {
        std::uint32_t length = 0;
        if (!read_varuint32(length) || length > 1024 * 1024 || length > remaining()) return false;
        out.assign(m_bytes.data() + m_offset, length);
        m_offset += length;
        return true;
    }

    bool read_uuid(std::string& out) {
        if (remaining() < 16) return false;
        static constexpr char hex[] = "0123456789abcdef";
        out.clear();
        out.reserve(32);
        for (std::size_t i = 0; i < 16; ++i) {
            const auto b = static_cast<unsigned char>(m_bytes[m_offset++]);
            out.push_back(hex[(b >> 4) & 0xF]);
            out.push_back(hex[b & 0xF]);
        }
        return true;
    }

    bool read_image(std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& pixels) {
        if (!read_u32(width) || !read_u32(height)) return false;
        if (width == 0 || height == 0 || width > kMaxSkinDimension || height > kMaxSkinDimension) return false;
        const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4ULL;
        if (expected > kMaxSkinBytes) return false;

        std::uint32_t length = 0;
        if (!read_varuint32(length) || length != expected || length > remaining()) return false;
        const auto* first = reinterpret_cast<const std::uint8_t*>(m_bytes.data() + m_offset);
        pixels.assign(first, first + length);
        m_offset += length;
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
        return c == '-' || c == '{' || c == '}' || c == ' ';
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::filesystem::path get_skin_directory() {
    const wchar_t* appData = _wgetenv(L"APPDATA");
    const std::filesystem::path base = (appData && *appData)
        ? std::filesystem::path(appData)
        : std::filesystem::temp_directory_path();
    return base / L"JhonClient" / L"skins";
}

bool write_png(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
               const std::vector<std::uint8_t>& rgba) {
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4ULL;
    if (expected != rgba.size() || expected > kMaxSkinBytes) return false;

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

void record_player_list(std::string_view body) {
    Cursor cursor(body);
    std::uint32_t count = 0;
    if (!cursor.read_varuint32(count) || count > 4096) return;

    // The player-list packet can batch many entries. We record the first Add entry
    // in each packet; later PlayerSkin packets are paired with this UUID/name table.
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t action = 0;
        if (!cursor.read_u8(action)) return;

        std::string uuid;
        if (!cursor.read_uuid(uuid)) return;
        if (action == 1) continue; // Remove entry in protocol 2225.
        if (action != 0) return;

        if (!cursor.read_varuint64()) return; // ActorUniqueID
        std::string name;
        if (!cursor.read_string(name)) return;

        std::lock_guard lock(g_mutex);
        g_playerNames[uuid] = name;
        return;
    }
}

void record_player_skin(std::string_view body) {
    Cursor cursor(body);
    std::string uuid;
    std::string skinId;
    std::string resourcePatch;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    if (!cursor.read_uuid(uuid) ||
        !cursor.read_string(skinId) ||
        !cursor.read_string(resourcePatch) ||
        !cursor.read_image(width, height, pixels)) {
        return;
    }

    if (!g_captureEnabled.load(std::memory_order_acquire)) return;

    std::string target;
    const bool filter = g_targetFilterEnabled.load(std::memory_order_acquire);
    {
        std::lock_guard lock(g_mutex);
        target = g_targetUuid;
    }
    if (filter && normalize_uuid(target) != uuid) return;

    std::string name = "Unknown";
    {
        std::lock_guard lock(g_mutex);
        const auto found = g_playerNames.find(uuid);
        if (found != g_playerNames.end() && !found->second.empty()) name = found->second;
    }

    std::lock_guard fileLock(g_fileMutex);
    const auto directory = get_skin_directory();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        LOG_ERROR("[SkinStealer] Failed to create skin directory: {}", ec.message());
        return;
    }

    const auto fileName = std::wstring(L"skin_") + std::wstring(uuid.begin(), uuid.end()) + L".png";
    const auto file = directory / fileName;
    if (!write_png(file, width, height, pixels)) {
        LOG_INFO("[SkinStealer] Skin packet received for {}, but the image format could not be exported as PNG.", uuid);
        return;
    }

    CapturedSkin entry{ uuid, name, file, width, height };
    {
        std::lock_guard lock(g_mutex);
        const auto found = std::find_if(g_skins.begin(), g_skins.end(), [&](const CapturedSkin& s) {
            return s.uuid == uuid;
        });
        if (found == g_skins.end()) g_skins.push_back(std::move(entry));
        else *found = std::move(entry);
    }
    LOG_INFO("[SkinStealer] Saved {} ({}) {}x{} to {}", name, uuid, width, height, file.string());
}

void** get_vtable(Packet* packet) {
    return packet ? *reinterpret_cast<void***>(packet) : nullptr;
}

bool patch_packet_vtable(const std::shared_ptr<Packet>& packet, int expectedId) {
    if (!packet || static_cast<int>(packet->getId()) != expectedId) return false;
    void** vtable = get_vtable(packet.get());
    if (!vtable || !vtable[kPacketReadVtableSlot]) return false;
    if (vtable[kPacketReadVtableSlot] == reinterpret_cast<void*>(&hook_packet_read)) return true;

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
    const int packetId = self ? static_cast<int>(self->getId()) : -1;
    PacketReadFn original = nullptr;
    if (packetId == static_cast<int>(MinecraftPacketIds::PlayerList)) original = g_originalPlayerList;
    if (packetId == static_cast<int>(MinecraftPacketIds::PlayerSkin)) original = g_originalPlayerSkin;
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
    if (!result.asExpected().has_value() || body.empty()) return result;

    try {
        if (packetId == static_cast<int>(MinecraftPacketIds::PlayerList)) record_player_list(body);
        else if (packetId == static_cast<int>(MinecraftPacketIds::PlayerSkin)) record_player_skin(body);
    } catch (...) {
        LOG_ERROR("[SkinStealer] Exception while processing a Bedrock player-skin packet.");
    }
    return result;
}

bool initialize() {
    if (g_initialized.load(std::memory_order_acquire)) return true;

    // These patterns are from the Bedrock 1.26.60 Windows release builds documented by
    // EndstoneMC/Spyglass. Verify both packet IDs before changing any vtable entry.
    constexpr std::string_view pattern_126605 =
        "56 48 83 EC 20 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 8D 0D ? ? ? ? "
        "48 63 04 81 48 01 C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 20 5E";
    constexpr std::string_view pattern_1266030 =
        "56 48 83 EC 30 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 8D 0D ? ? ? ? "
        "48 63 04 81 48 01 C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 30 5E";

    std::uintptr_t factoryAddress = minecraft::hook_memory::find_signature(pattern_126605);
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
    g_playerNames.clear();
    g_initialized.store(false, std::memory_order_release);
    LOG_INFO("[SkinStealer] Player packet hooks removed.");
}

void set_capture_enabled(bool enabled) {
    g_captureEnabled.store(enabled, std::memory_order_release);
}

void set_target_filter(bool enabled, std::string uuid) {
    std::lock_guard lock(g_mutex);
    g_targetUuid = normalize_uuid(std::move(uuid));
    g_targetFilterEnabled.store(enabled, std::memory_order_release);
}

std::vector<CapturedSkin> captured_skins() {
    std::lock_guard lock(g_mutex);
    return g_skins;
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
