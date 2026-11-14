#include "minecraft/hooks/fishing_packet_hook.h"

#include "events/event_bus.h"
#include "events/fishing_packet_event.h"
#include "minecraft/hooks/hook_memory.h"
#include "skin/packet_abi.h"
#include "utils/logger.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minecraft::fishing {

Bedrock::Result<void> __fastcall hook_packet_read(
    Packet* self, ReadOnlyBinaryStream& stream,
    const cereal::ReflectionCtx& reflection);

namespace {

constexpr std::size_t kPacketReadVtableSlot = 9;
constexpr int kAddActorPacket = 13;
constexpr int kRemoveActorPacket = 14;
constexpr int kMoveActorAbsolutePacket = 18;
constexpr int kMoveActorDeltaPacket = 111;
constexpr int kSetActorMotionPacket = 40;

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
std::vector<HookRecord> g_hooks;
std::vector<std::shared_ptr<Packet>> g_packetKeepAlive;
std::unordered_map<void**, HookInfo> g_hookByVtable;
std::unordered_map<std::uint64_t, std::uint64_t> g_uniqueToRuntime;
std::unordered_map<std::uint64_t, math::Vec3> g_runtimePositions;
std::array<std::atomic_bool, 5> g_packetEntryLogged{};
std::array<std::atomic_bool, 5> g_packetReturnLogged{};
std::atomic_uint32_t g_addActorParseSamples{0};

std::size_t packet_trace_index(int packetId) {
    switch (packetId) {
    case kAddActorPacket: return 0;
    case kRemoveActorPacket: return 1;
    case kMoveActorAbsolutePacket: return 2;
    case kMoveActorDeltaPacket: return 3;
    case kSetActorMotionPacket: return 4;
    default: return g_packetEntryLogged.size();
    }
}

void trace_packet_phase(int packetId, std::size_t bodySize, bool returned) {
    const auto index = packet_trace_index(packetId);
    if (index >= g_packetEntryLogged.size()) return;
    auto& flag = returned ? g_packetReturnLogged[index] : g_packetEntryLogged[index];
    if (!flag.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO("[AutoFishing][Trace] Packet::read {} packet ID {} (body={} bytes).",
            returned ? "returned from" : "entered for", packetId, bodySize);
    }
}

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

    bool read_varint64(std::int64_t& value) {
        std::uint64_t encoded = 0;
        if (!read_varuint64(encoded)) return false;
        const std::uint64_t sign = 0ULL - (encoded & 1ULL);
        value = static_cast<std::int64_t>((encoded >> 1) ^ sign);
        return true;
    }

    bool read_string(std::string& value) {
        std::uint64_t size = 0;
        if (!read_varuint64(size) || size > 4096 || size > remaining()) return false;
        value.assign(m_data.data() + m_offset, static_cast<std::size_t>(size));
        m_offset += static_cast<std::size_t>(size);
        return true;
    }

    bool read_float(float& value) {
        if (remaining() < sizeof(float)) return false;
        std::memcpy(&value, m_data.data() + m_offset, sizeof(float));
        m_offset += sizeof(float);
        return true;
    }

    bool read_vec3(math::Vec3& value) {
        return read_float(value.x) && read_float(value.y) && read_float(value.z);
    }

    bool read_optional_float(float& value, bool& present) {
        std::uint8_t hasValue = 0;
        if (!read_u8(hasValue)) return false;
        present = hasValue != 0;
        return !present || read_float(value);
    }

private:
    [[nodiscard]] std::size_t remaining() const {
        return m_offset <= m_data.size() ? m_data.size() - m_offset : 0;
    }

    std::string_view m_data;
    std::size_t m_offset = 0;
};

void publish(FishingPacketEvent::Kind kind, std::uint64_t uniqueId,
              std::uint64_t runtimeId, const math::Vec3& position = {},
              const math::Vec3& velocity = {}) {
    FishingPacketEvent event(kind, uniqueId, runtimeId, position, velocity);
    EventBus::instance().publish(event);
}

void process_packet(int packetId, std::string_view body) {
    PacketCursor cursor(body);

    if (packetId == kAddActorPacket) {
        std::int64_t uniqueIdSigned = 0;
        std::uint64_t runtimeId = 0;
        std::string actorType;
        math::Vec3 position{};
        math::Vec3 velocity{};

        if (!cursor.read_varint64(uniqueIdSigned) ||
            !cursor.read_varuint64(runtimeId) ||
            !cursor.read_string(actorType) ||
            !cursor.read_vec3(position) ||
            !cursor.read_vec3(velocity)) {
            const auto sample = g_addActorParseSamples.fetch_add(1, std::memory_order_relaxed);
            if (sample < 5) {
                LOG_WARN("[AutoFishing][Trace] Could not parse AddActor body ({} bytes).", body.size());
            }
            return;
        }

        const auto sample = g_addActorParseSamples.fetch_add(1, std::memory_order_relaxed);
        if (sample < 5 || actorType == "minecraft:fishing_hook") {
            LOG_INFO("[AutoFishing][Trace] AddActor parsed: type='{}', runtimeId={}, pos=({:.2f}, {:.2f}, {:.2f}).",
                actorType, runtimeId, position.x, position.y, position.z);
        }
        if (actorType != "minecraft:fishing_hook" || runtimeId == 0) return;

        const auto uniqueId = static_cast<std::uint64_t>(uniqueIdSigned);
        {
            std::lock_guard lock(g_mutex);
            g_uniqueToRuntime.insert_or_assign(uniqueId, runtimeId);
            g_runtimePositions.insert_or_assign(runtimeId, position);
        }

        publish(FishingPacketEvent::Kind::Spawn, uniqueId, runtimeId, position, velocity);
        return;
    }

    if (packetId == kMoveActorAbsolutePacket) {
        std::uint64_t runtimeId = 0;
        std::uint8_t header = 0;
        math::Vec3 position{};

        if (!cursor.read_varuint64(runtimeId) ||
            !cursor.read_u8(header) ||
            !cursor.read_vec3(position) ||
            runtimeId == 0) {
            return;
        }

        {
            std::lock_guard lock(g_mutex);
            g_runtimePositions.insert_or_assign(runtimeId, position);
        }
        publish(FishingPacketEvent::Kind::Move, 0, runtimeId, position, {});
        return;
    }

    if (packetId == kMoveActorDeltaPacket) {
        std::uint64_t runtimeId = 0;
        if (!cursor.read_varuint64(runtimeId) || runtimeId == 0) return;

        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool hasX = false, hasY = false, hasZ = false;
        if (!cursor.read_optional_float(x, hasX) ||
            !cursor.read_optional_float(y, hasY) ||
            !cursor.read_optional_float(z, hasZ)) {
            return;
        }
        if (!hasX && !hasY && !hasZ) return;

        math::Vec3 position{};
        {
            std::lock_guard lock(g_mutex);
            const auto found = g_runtimePositions.find(runtimeId);
            if (found == g_runtimePositions.end()) return;
            position = found->second;
            if (hasX) position.x = x;
            if (hasY) position.y = y;
            if (hasZ) position.z = z;
            found->second = position;
        }
        publish(FishingPacketEvent::Kind::Move, 0, runtimeId, position, {});
        return;
    }

    if (packetId == kSetActorMotionPacket) {
        std::uint64_t runtimeId = 0;
        math::Vec3 velocity{};

        if (!cursor.read_varuint64(runtimeId) ||
            !cursor.read_vec3(velocity) ||
            runtimeId == 0) {
            return;
        }

        publish(FishingPacketEvent::Kind::Motion, 0, runtimeId, {}, velocity);
        return;
    }

    if (packetId == kRemoveActorPacket) {
        std::int64_t uniqueIdSigned = 0;
        if (!cursor.read_varint64(uniqueIdSigned)) return;

        const auto uniqueId = static_cast<std::uint64_t>(uniqueIdSigned);
        std::uint64_t runtimeId = 0;
        {
            std::lock_guard lock(g_mutex);
            const auto found = g_uniqueToRuntime.find(uniqueId);
            if (found == g_uniqueToRuntime.end()) return;
            runtimeId = found->second;
            g_uniqueToRuntime.erase(found);
            g_runtimePositions.erase(runtimeId);
        }

        publish(FishingPacketEvent::Kind::Remove, uniqueId, runtimeId);
    }
}

bool patch_packet_vtable(const std::shared_ptr<Packet>& packet, int expectedId) {
    if (!packet || static_cast<int>(packet->getId()) != expectedId) return false;

    void** vtable = nullptr;
    if (!read_vtable(packet.get(), &vtable) ||
        !vtable || !vtable[kPacketReadVtableSlot]) {
        return false;
    }

    if (g_hookByVtable.contains(vtable)) {
        return g_hookByVtable[vtable].packetId == expectedId;
    }

    void** slot = &vtable[kPacketReadVtableSlot];
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
    g_hookByVtable.emplace(vtable, HookInfo{original, expectedId});
    g_packetKeepAlive.push_back(packet);
    return true;
}

} // namespace

Bedrock::Result<void> __fastcall hook_packet_read(
    Packet* self, ReadOnlyBinaryStream& stream,
    const cereal::ReflectionCtx& reflection) {

    void** incomingVtable = nullptr;
    if (!read_vtable(self, &incomingVtable)) {
        return Bedrock::Result<void>{};
    }

    HookInfo info{};
    {
        std::lock_guard lock(g_mutex);
        const auto found = g_hookByVtable.find(incomingVtable);
        if (found == g_hookByVtable.end() || !found->second.original) {
            return Bedrock::Result<void>{};
        }
        info = found->second;
    }

    // Emit before inspecting the stream or calling the original packet handler.
    trace_packet_phase(info.packetId, 0, false);

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

    trace_packet_phase(info.packetId, body.size(), true);

    try {
        process_packet(info.packetId, body);
    } catch (...) {
        LOG_WARN("[AutoFishing] Ignored a malformed fishing-related packet.");
    }
    return result;
}

bool initialize() {
    if (g_initialized.load(std::memory_order_acquire)) return true;

    constexpr std::string_view factoryPattern =
        "56 48 83 EC 20 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 8D 0D ? ? ? ? "
        "48 63 04 81 48 01 C8 FF E0 0F 57 C0 0F 11 06 48 89 F0 48 83 C4 20 5E";

    const std::uintptr_t factoryAddress =
        minecraft::hook_memory::find_signature(factoryPattern);
    if (!factoryAddress) {
        LOG_ERROR("[AutoFishing] Bedrock packet factory was not found.");
        return false;
    }

    const auto factory = reinterpret_cast<minecraft::skin::CreatePacketFn>(factoryAddress);
    constexpr std::array<int, 5> packetIds{
        kAddActorPacket,
        kRemoveActorPacket,
        kMoveActorAbsolutePacket,
        kMoveActorDeltaPacket,
        kSetActorMotionPacket
    };

    std::vector<std::shared_ptr<Packet>> packets;
    packets.reserve(packetIds.size());
    for (const int id : packetIds) {
        auto packet = factory(static_cast<MinecraftPacketIds>(id));
        if (!packet || static_cast<int>(packet->getId()) != id) {
            LOG_ERROR("[AutoFishing] Packet factory validation failed for packet {}.", id);
            return false;
        }
        packets.push_back(std::move(packet));
    }

    std::lock_guard lock(g_mutex);
    for (std::size_t i = 0; i < packetIds.size(); ++i) {
        if (!patch_packet_vtable(packets[i], packetIds[i])) {
            LOG_ERROR("[AutoFishing] Could not hook packet {}.", packetIds[i]);
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
            g_hookByVtable.clear();
            g_packetKeepAlive.clear();
            g_uniqueToRuntime.clear();
            g_runtimePositions.clear();
            return false;
        }
    }

    g_initialized.store(true, std::memory_order_release);
    LOG_INFO("[AutoFishing] Bedrock fishing packet hooks installed (13, 14, 18, 111, 40).");
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
    g_uniqueToRuntime.clear();
    g_runtimePositions.clear();
    g_initialized.store(false, std::memory_order_release);
    LOG_INFO("[AutoFishing] Fishing packet hooks removed.");
}

} // namespace minecraft::fishing
