#pragma once

#include "events/event.h"

class ClientInstance;

struct MouseGrabEvent : public Event {
    ClientInstance* clientInstance = nullptr;

    explicit MouseGrabEvent(ClientInstance* instance)
        : clientInstance(instance) {
    }
};
