#pragma once

#include "events/event.h"

class Actor;
class LocalPlayer;

struct ActorSetRotEvent : public Event {
    Actor* actor = nullptr;
    LocalPlayer* localPlayer = nullptr;

    ActorSetRotEvent(Actor* targetActor, LocalPlayer* player = nullptr)
        : actor(targetActor), localPlayer(player) {
    }

    [[nodiscard]] bool isLocalPlayer() const {
        return localPlayer != nullptr;
    }
};
