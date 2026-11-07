#pragma once

#include "utils/math.h"

#include <cstdint>

class Actor;

namespace minecraft::sdk {
    struct BlockPosition {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
    };
}

enum class HandSlot : std::uint8_t {
    MainHand = 0,
    OffHand = 1
};

// Bedrock's InteractionResult is a one-byte pair of bit fields.
struct InteractionResult {
    bool mSuccess : 1;
    bool mSwing : 1;
};
static_assert(sizeof(InteractionResult) == 1);

class GameMode {
public:
    virtual ~GameMode() = default; // 0
    virtual bool startDestroyBlock(const void* pos, std::uint8_t face, bool& isDestroyedOut) = 0; // 1
    virtual bool destroyBlock(const void* pos, std::uint8_t face) = 0; // 2
    virtual void continueDestroyBlock(const void* pos, std::uint8_t face, const Vec3& playerPos, bool& out) = 0; // 3
    virtual void stopDestroyBlock(const void* pos) = 0; // 4
    virtual void startBuildBlock(const void* pos, std::uint8_t face) = 0; // 5
    virtual bool buildBlock(const void* pos, std::uint8_t face, bool isAuthed) = 0; // 6
    virtual void continueBuildBlock(const void* pos, std::uint8_t face) = 0; // 7
    virtual void stopBuildBlock() = 0; // 8
    virtual void tick() = 0; // 9
    virtual float getPickRange(const void* inputMode, bool isVR) = 0; // 10
    virtual bool useItem(void* itemStack) = 0; // 11
    virtual bool useItemAsAttack(void* itemStack) = 0; // 12
    virtual InteractionResult useItemOn(
        void* itemStack,
        const minecraft::sdk::BlockPosition& pos,
        std::uint8_t face,
        const Vec3& hitPosition,
        HandSlot handSlot,
        const void* targetBlock,
        bool isFirstEvent) = 0; // 13
    virtual bool interact(Actor* target, const Vec3& location) = 0; // 14
    virtual void attack(Actor* target) = 0; // 15
    virtual void releaseUsingItem() = 0; // 16
};
