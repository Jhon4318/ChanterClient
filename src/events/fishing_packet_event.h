#pragma once

#include "events/event.h"
#include "utils/math.h"

#include <cstdint>

struct FishingPacketEvent : public Event {
    enum class Kind {
        Spawn,
        Move,
        Motion,
        Remove,
        Bite
    };

    Kind kind = Kind::Spawn;
    std::uint64_t uniqueId = 0;
    std::uint64_t runtimeId = 0;
    math::Vec3 position{};
    math::Vec3 velocity{};

    FishingPacketEvent(Kind eventKind, std::uint64_t unique,
                       std::uint64_t runtime, const math::Vec3& pos,
                       const math::Vec3& motion)
        : kind(eventKind), uniqueId(unique), runtimeId(runtime),
          position(pos), velocity(motion) {
    }
};
