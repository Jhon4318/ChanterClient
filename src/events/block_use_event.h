#pragma once

#include "events/event.h"

#include <cstdint>
#include <string>
#include <utility>

struct BlockUseEvent final : Event {
    std::string blockType;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint8_t face = 0;
    std::uint8_t hand = 0;
    bool firstEvent = false;

    BlockUseEvent(std::string type, std::int32_t blockX, std::int32_t blockY,
                  std::int32_t blockZ, std::uint8_t blockFace,
                  std::uint8_t handSlot, bool isFirstEvent)
        : blockType(std::move(type)),
          x(blockX), y(blockY), z(blockZ), face(blockFace),
          hand(handSlot), firstEvent(isFirstEvent) {}
};
