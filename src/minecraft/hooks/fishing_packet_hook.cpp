#include "minecraft/hooks/fishing_packet_hook.h"

#include "events/event_bus.h"
#include "events/fishing_packet_event.h"
#include "minecraft/hooks/hook_memory.h"
#include "skin/packet_abi.h"
#include "utils/logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minecraft::fishing {

Bedrock::Result<void> __fastcall hook_packet_read(
    Packet* self, ReadOnlyBinaryStream& stream,
    const cereal::ReflectionCtx& reflection);

namespace {
constexpr std::size_t kPacketReadWithReflectionVtableSlot = 8;
constexpr int kActorEventPacket = static_cast<int>(MinecraftPacketIds::ActorEvent);
constexpr std::uint8_t kFishHookHooktimeEvent = 13; // Bedrock 1.26.60 / protocol 2225
using PacketReadFn = minecraft::skin::PacketReadFn;

struct HookRecord {
    void** slot = nullptr;
    void* original = nullptr;
};

struct HookInfo {
    void* original = nullptr;
    int packetId = -1;
};

std::mutex g_mutex;
std::atomic_bool g_initialized{false};
std::atomic_bool g_readTraceLogged{false};
std::vector<HookRecord> g_hooks;
std::vector<std::shared_ptr<Packet>> g_packetKeepAlive;
std::vector<std::pair<void**, HookInfo>> g_hookByVtable;

bool read_vtable(Packet* packet, void*** output) noexcept {
    if (!packet || !output) return false;
    __try {
        *output = *reinterpret_cast<void***>(packet);
        return *output != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

class PacketCursor {
public:
    explicit PacketCursor(std::string_view data) : m_data(data) {}

    bool read_u8(std::uint8_t& value) {
        if (m_offset >= m_data.size()) return false;
        value = static_cast<std::uint8_t>(m_data[m_offset++]);
        return true;
    }

    bool read_varuint64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            std::uint8_t byte = 0;
            if (!read_u8(byte)) return false;
            if (shift == 63 && (byte & 0xFEU) != 0) return false;
            value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0) return true;
        }
        return false;
    }

private:
    std::string_view m_data;
    std::size_t m_offset = 0;
};

void publish_bite(std::uint64_t runtimeId) {
    FishingPacketEvent event(FishingPacketEvent::Kind::Bite, 0, runtimeId, {}, {});
    EventBus::instance().publish(event);
}

void process_packet(int packetId, std::string_view body) {
    if (packetId != kActorEventPacket) return;

    // ActorEventPacket: target runtime ID (varuint64), event ID (uint8), data (varint32).
    PacketCursor cursor(body);
    std::uint64_t runtimeId = 0;
    std::uint8_t eventId = 0;
    if (!cursor.read_varuint64(runtimeId) || !cursor.read_u8(eventId) || runtimeId == 0) {
        return;
    }
    if (eventId != kFishHookHooktimeEvent) return;

    LOG_INFO("[AutoFishing][Trace] Server FishHookHooktime event received (runtime ID={}).",
        runtimeId);
    publish_bite(runtimeId);
}

bool patch_packet_vtable(const std::shared_ptr<Packet>& packet, int expectedId) {
    if (!packet || static_cast<int>(packet->getId()) != expectedId) return false;

    void** vtable = nullptr;
    if (!read_vtable(packet.get(), &vtable) ||
        !vtable || !vtable[kPacketReadWithReflectionVtableSlot]) {
        return false;
    }

    for (const auto& entry : g_hookByVtable) {
        if (entry.first == vtable) return entry.second.packetId == expectedId;
    }

    void** slot = &vtable[kPacketReadWithReflectionVtableSlot];
    void* original = *slot;
    if (!original) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    *slot = reinterpret_cast<void*>(&hook_packet_read);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);

    g_hooks.push_back({slot, original});
    g_hookByVtable.push_back({vtable, HookInfo{original, expectedId}});
    g_packetKeepAlive.push_back(packet);
    return true;
}

} // namespace

Bedrock::Result<void> __fastcall hook_packet_read(
    Packet* self, ReadOnlyBinaryStream& stream,
    const cereal::ReflectionCtx& reflection) {
    void** incomingVtable = nullptr;
    if (!read_vtable(self, &incomingVtable)) return Bedrock::Result<void>{};

    HookInfo info{};
    {
        std::lock_guard lock(g_mutex);
        bool found = false;
        for (const auto& entry : g_hookByVtable) {
            if (entry.first == incomingVtable) {
                info = entry.second;
                found = info.original != nullptr;
                break;
            }
        }
        if (!found) return Bedrock::Result<void>{};
    }

    if (!g_readTraceLogged.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO("[AutoFishing][Trace] ActorEventPacket::read hook entered.");
    }

    std::string body;
    try {
        const auto view = stream.getView();
        const auto offset = stream.getReadPointer();
        if (offset <= view.size()) {
            body.assign(view.data() + offset, view.size() - offset);
        }
    } catch (...) {
        body.clear();
    }

    const auto original = reinterpret_cast<PacketReadFn>(info.original);
    Bedrock::Result<void> result = original(self, stream, reflection);
    if (!result.asExpected().has_value() || body.empty()) return result;

    try {
        process_packet(info.packetId, body);
    } catch (...) {
        LOG_WARN("[AutoFishing] Ignored a malformed ActorEventPacket.");
    }
    return result;
}

bool initialize() {
    if (g_initialized.load(std::memory_order_acquire)) return true;

    constexpr std::string_view factoryPattern =
        "56 48 83 EC 20 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 8D 0D ? ? ? ? "
        "48 63 04 81 48 01 C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 20 5E";

    const std::uintptr_t factoryAddress = minecraft::hook_memory::find_signature(factoryPattern);
    if (!factoryAddress) {
        LOG_ERROR("[AutoFishing] Bedrock packet factory was not found.");
        return false;
    }

    const auto factory = reinterpret_cast<minecraft::skin::CreatePacketFn>(factoryAddress);
    auto packet = factory(MinecraftPacketIds::ActorEvent);
    if (!packet || static_cast<int>(packet->getId()) != kActorEventPacket) {
        LOG_ERROR("[AutoFishing] ActorEventPacket factory validation failed.");
        return false;
    }

    std::lock_guard lock(g_mutex);
    if (!patch_packet_vtable(packet, kActorEventPacket)) {
        LOG_ERROR("[AutoFishing] Could not hook ActorEventPacket::read.");
        return false;
    }

    g_initialized.store(true, std::memory_order_release);
    LOG_INFO("[AutoFishing] Fish-bite packet hook installed (packet ID=27, event ID=13, read slot=8).");
    return true;
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    for (auto& hook : g_hooks) {
        if (!hook.slot || !hook.original) continue;
        DWORD oldProtect = 0;
        if (!VirtualProtect(hook.slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            continue;
        }
        *hook.slot = hook.original;
        DWORD ignored = 0;
        VirtualProtect(hook.slot, sizeof(void*), oldProtect, &ignored);
    }

    g_hooks.clear();
    g_hookByVtable.clear();
    g_packetKeepAlive.clear();
    g_initialized.store(false, std::memory_order_release);
    g_readTraceLogged.store(false, std::memory_order_relaxed);
    LOG_INFO("[AutoFishing] Fish-bite packet hook removed.");
}

} // namespace minecraft::fishing
