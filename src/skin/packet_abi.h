#pragma once

// ABI mirror for the Bedrock 1.26.60 packet read path.
// The slot and packet factory pattern are version-gated and validated before patching.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nonstd/expected.hpp>

namespace BedrockLog {
    enum class LogAreaID : unsigned int { All = 0 };
}

namespace Bedrock {

class LogLevel {
public:
    enum class Type : std::uint8_t { Verbose = 1, Info = 2, Warning = 4, Error = 8 };
    constexpr LogLevel(Type type) : type_(type) {}
    [[nodiscard]] constexpr Type getType() const { return type_; }
private:
    Type type_;
    bool log_in_publish_{false};
};

struct CallStack {
    struct Frame {
        std::size_t filename_hash{};
        std::string_view filename{};
        std::uint32_t line{};
    };
    struct Context {
        std::string value;
        std::optional<LogLevel> log_level;
        std::optional<::BedrockLog::LogAreaID> log_area;
    };
    struct FrameWithContext {
        Frame frame;
        std::optional<Context> context;
    };
    std::vector<FrameWithContext> frames;
};

template <typename ErrorType>
struct ErrorInfo {
    ErrorType error;
    CallStack call_stack;
    std::vector<ErrorInfo> branches;
};

template <typename T, typename E = std::error_code>
class Result : nonstd::expected<T, ErrorInfo<E>> {
    using Base = nonstd::expected<T, ErrorInfo<E>>;
public:
    using Base::Base;
    [[nodiscard]] const Base& asExpected() const { return *this; }
};

} // namespace Bedrock

enum class MinecraftPacketIds : int {
    PlayerList = 63,
    PlayerSkin = 93
};
enum class SerializationMode : int;

class BinaryStream;
namespace cereal { struct ReflectionCtx; }

class ReadOnlyBinaryStream {
public:
    virtual ~ReadOnlyBinaryStream() = default;

    [[nodiscard]] std::size_t getReadPointer() const { return read_pointer_; }
    [[nodiscard]] std::size_t getLength() const { return view_.size(); }
    [[nodiscard]] std::string_view getView() const { return view_; }

private:
    virtual Bedrock::Result<void> read(void* target, std::uint64_t count) = 0;

protected:
    std::string owned_buffer_;
    std::string_view view_;

private:
    std::size_t read_pointer_{0};
    bool has_overflowed_{false};
};

class Packet {
public:
    virtual ~Packet() = default;
    [[nodiscard]] virtual MinecraftPacketIds getId() const = 0;
    [[nodiscard]] virtual std::string_view getName() const = 0;
    [[nodiscard]] virtual std::size_t getMaxSize() const = 0;
    [[nodiscard]] virtual Bedrock::Result<void> checkSize(std::size_t packet_size, bool receiver_is_server) const = 0;
    virtual void writeWithSerializationMode(BinaryStream& stream, const cereal::ReflectionCtx& reflection,
                                             std::optional<SerializationMode> override_mode) const = 0;
    virtual void write(BinaryStream& stream, const cereal::ReflectionCtx& reflection) const = 0;
    virtual void write(BinaryStream& stream) const = 0;
    virtual Bedrock::Result<void> read(ReadOnlyBinaryStream& stream, const cereal::ReflectionCtx& reflection) = 0;
    virtual Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) = 0;
    [[nodiscard]] virtual bool disallowBatching() const = 0;
    [[nodiscard]] virtual bool isValid() const = 0;
    [[nodiscard]] virtual SerializationMode getSerializationMode() const = 0;
    virtual void setSerializationMode(SerializationMode mode) = 0;
    [[nodiscard]] virtual std::string toString() const = 0;

private:
    virtual Bedrock::Result<void> _read(ReadOnlyBinaryStream& stream, const cereal::ReflectionCtx& reflection) = 0;
    virtual Bedrock::Result<void> _read(ReadOnlyBinaryStream& stream) = 0;

    // The game owns this object. Keep the mirrored x64 object size to 0x30.
    std::byte opaque_[40]{};
};

static_assert(sizeof(Packet) == 0x30, "Bedrock 1.26.60 Packet ABI mirror changed");

namespace minecraft::skin {
    using CreatePacketFn = std::shared_ptr<Packet>(__fastcall*)(MinecraftPacketIds);
    using PacketReadFn = Bedrock::Result<void>(__fastcall*)(Packet*, ReadOnlyBinaryStream&, const cereal::ReflectionCtx&);
}
