#pragma once

#include "events/event.h"

class Actor;
class ClientInstance;
class LocalPlayer;

struct ActorEntityTickEvent : public Event {
    ClientInstance* clientInstance = nullptr;
    LocalPlayer* localPlayer = nullptr;
    Actor* tickingActor = nullptr;

    ActorEntityTickEvent(ClientInstance* instance, LocalPlayer* player, Actor* actor)
        : clientInstance(instance), localPlayer(player), tickingActor(actor) {
    }
};
