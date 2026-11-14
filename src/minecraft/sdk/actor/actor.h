#pragma once

#include "utils/math.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <minecraft/sdk/components/AABBShapeComponent.h>
#include <minecraft/sdk/components/RenderPositionComponent.h>
#include <minecraft/sdk/components/ActorHeadRotationComponent.h>
#include <minecraft/sdk/components/RuntimeIDComponent.h>
#include "minecraft/sdk/entity/entity_context.h"
#include "minecraft/sdk/components/StateVectorComponent.h"
#include "minecraft/sdk/components/ActorRotationComponent.h"
#include <minecraft/sdk/components/ActorDataFlagComponent.h>
#include <minecraft/sdk/components/ActorGameTypeComponent.h>
#include <minecraft/sdk/components/ActorTypeComponent.h>
#include <minecraft/sdk/components/ActorOwnerComponent.h>
#include <minecraft/sdk/components/ActorUniqueIDComponent.h>
#include <minecraft/sdk/components/FlagComponents.h>

// Exact component hashes for the Minecraft Bedrock 1.26.60.5 ECS layout.
// They are used instead of EnTT's locally generated type IDs: the native
// registry was populated with Bedrock's hashed component identifiers.
namespace minecraft::actor_component_hash {
    inline constexpr std::uint32_t Owner = 0x85B93800;
    inline constexpr std::uint32_t Type = 0x4F6BA419;
    inline constexpr std::uint32_t UniqueId = 0x18F957AF;
    inline constexpr std::uint32_t RuntimeId = 0xFC0DBBB5;
    inline constexpr std::uint32_t StateVector = 0x1B5D5238;
    inline constexpr std::uint32_t Rotation = 0x75DF36B7;
    inline constexpr std::uint32_t AabbShape = 0xBAC1B3CF;
}

class Actor {
public:
    virtual ~Actor() = default;

    EntityContext* getEntityContext() const {
        if (!this) return nullptr;
        return reinterpret_cast<EntityContext*>(reinterpret_cast<std::uintptr_t>(this) + 0x8);
    }

    template <typename T>
    T* getComponentByHash(std::uint32_t componentHash) const {
        EntityContext* context = getEntityContext();
        if (!context || !componentHash) return nullptr;

        __try {
            auto& registry = context->getRegistry();
            const EntityId entity = context->mEntity;
            if (!registry.valid(entity)) return nullptr;
            auto* storage = registry.storage(componentHash);
            if (!storage || !storage->contains(entity)) return nullptr;
            return static_cast<T*>(storage->value(entity));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    template <typename T>
    bool hasComponent() const {
        EntityContext* context = getEntityContext();
        if (!context) return false;
        __try {
            auto& registry = context->getRegistry();
            return registry.valid(context->mEntity) && registry.all_of<T>(context->mEntity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <typename T>
    T* getComponent() const {
        EntityContext* context = getEntityContext();
        if (!context) return nullptr;
        __try {
            auto& registry = context->getRegistry();
            if (!registry.valid(context->mEntity)) return nullptr;
            return registry.try_get<T>(context->mEntity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    template <typename T>
    void addComponent() {
        EntityContext* context = getEntityContext();
        if (!context) return;
        __try {
            auto& registry = context->getRegistry();
            if (registry.valid(context->mEntity)) registry.get_or_emplace<T>(context->mEntity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    template <typename T>
    void removeComponent() {
        EntityContext* context = getEntityContext();
        if (!context) return;
        __try {
            auto& registry = context->getRegistry();
            if (registry.valid(context->mEntity)) registry.remove<T>(context->mEntity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    std::uint64_t getUniqueId() const {
        if (auto* unique = getComponentByHash<ActorUniqueIDComponent>(minecraft::actor_component_hash::UniqueId)) {
            return unique->mUniqueID;
        }
        return 0;
    }

    ActorType getActorType() const {
        if (auto* comp = getComponentByHash<ActorTypeComponent>(minecraft::actor_component_hash::Type)) {
            return comp->mType;
        }
        return ActorType::Undefined;
    }

    GameType getGameType() const {
        if (auto* comp = getComponent<ActorGameTypeComponent>()) {
            return comp->mGameType;
        }
        return GameType::Undefined;
    }

    void setGameType(GameType gameType) {
        if (auto* comp = getComponent<ActorGameTypeComponent>()) {
            comp->mGameType = gameType;
        }
    }

    bool getFlag(ActorFlags flag) const {
        if (auto* flags = getComponent<ActorDataFlagComponent>()) {
            return flags->getFlag(flag);
        }
        return false;
    }

    void setFlag(ActorFlags flag, bool value) {
        if (auto* flags = getComponent<ActorDataFlagComponent>()) {
            flags->setFlag(flag, value);
        }
    }

    math::Vec3 getVelocity() const {
        auto* stateVector = getComponentByHash<StateVectorComponent>(minecraft::actor_component_hash::StateVector);
        return stateVector ? stateVector->mPosDelta : math::Vec3{ 0.0f, 0.0f, 0.0f };
    }

    void setVelocity(const math::Vec3& velocity) {
        if (auto* stateVector = getComponentByHash<StateVectorComponent>(minecraft::actor_component_hash::StateVector)) {
            stateVector->mPosDelta = velocity;
        }
    }

    math::ViewAngles getRotation() const {
        if (auto* rot = getComponentByHash<ActorRotationComponent>(minecraft::actor_component_hash::Rotation)) {
            return rot->mRotation;
        }
        return { 0.0f, 0.0f };
    }

    void setRotation(const math::ViewAngles& angles) {
        if (auto* rot = getComponentByHash<ActorRotationComponent>(minecraft::actor_component_hash::Rotation)) {
            rot->mRotation = angles;
            rot->mOldRotation = angles;
        }
    }

    float getHeadYaw() const {
        if (auto* headRot = getComponent<ActorHeadRotationComponent>()) {
            return headRot->mHeadYaw.x;
        }
        return getRotation().yaw;
    }

    void setHeadYaw(float headYaw) {
        if (auto* headRot = getComponent<ActorHeadRotationComponent>()) {
            headRot->mHeadYaw = { headYaw, headYaw };
        }
    }

    std::uint64_t getRuntimeId() const {
        if (auto* runtime = getComponentByHash<RuntimeIDComponent>(minecraft::actor_component_hash::RuntimeId)) {
            return runtime->mRuntimeId;
        }
        return 0;
    }

    AABBShapeComponent* getAABB() const {
        return getComponentByHash<AABBShapeComponent>(minecraft::actor_component_hash::AabbShape);
    }

    math::Vec3 getPosition() const {
        if (auto* stateVector = getComponentByHash<StateVectorComponent>(minecraft::actor_component_hash::StateVector)) {
            if (stateVector->mPos.x != 0.0f || stateVector->mPos.y != 0.0f || stateVector->mPos.z != 0.0f) {
                return stateVector->mPos;
            }
        }
        if (auto* aabb = getAABB()) {
            if (aabb->mMin.x != 0.0f || aabb->mMin.y != 0.0f || aabb->mMin.z != 0.0f) {
                return {
                    (aabb->mMin.x + aabb->mMax.x) * 0.5f,
                    aabb->mMin.y,
                    (aabb->mMin.z + aabb->mMax.z) * 0.5f
                };
            }
        }
        return { 0.0f, 0.0f, 0.0f };
    }

    math::Vec3 getRenderPosition() const {
        if (auto* renderPos = getComponent<RenderPositionComponent>()) {
            if (renderPos->mPosition.x != 0.0f || renderPos->mPosition.y != 0.0f || renderPos->mPosition.z != 0.0f) {
                return renderPos->mPosition;
            }
        }
        return getPosition();
    }

    [[nodiscard]] bool isAlive() const {
        return !hasComponent<IsDeadFlagComponent>();
    }

    [[nodiscard]] bool isPlayer() const {
        return getActorType() == ActorType::Player;
    }

    [[nodiscard]] static std::vector<Actor*> get_all(Actor* referenceActor) {
        constexpr std::size_t kMaxActors = 4096;
        std::array<Actor*, kMaxActors> buffer{};
        const std::size_t count = collect_actor_refs(referenceActor, buffer.data(), buffer.size());
        return std::vector<Actor*>(buffer.begin(), buffer.begin() + count);
    }

private:
    // Keep SEH in a POD-only helper. The registry's native storage uses
    // Bedrock's component hash, not EnTT's compile-time component type ID.
    static std::size_t collect_actor_refs(
        Actor* referenceActor, Actor** output, std::size_t capacity) noexcept {
        if (!referenceActor || !output || capacity == 0) return 0;
        __try {
            EntityContext* context = referenceActor->getEntityContext();
            if (!context) return 0;
            auto& registry = context->getRegistry();
            auto* storage = registry.storage(minecraft::actor_component_hash::Owner);
            if (!storage) return 0;

            const std::size_t size = storage->size();
            if (size > 16384) return 0;
            const auto* entities = storage->data();
            if (!entities) return 0;

            std::size_t written = 0;
            for (std::size_t i = 0; i < size && written < capacity; ++i) {
                const EntityId entity = entities[i];
                if (!registry.valid(entity) || !storage->contains(entity)) continue;
                auto* owner = static_cast<ActorOwnerComponent*>(storage->value(entity));
                if (!owner || !owner->mActor || owner->mActor == referenceActor) continue;
                output[written++] = owner->mActor;
            }
            return written;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
};
