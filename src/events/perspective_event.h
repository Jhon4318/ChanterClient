#pragma once

#include "events/event.h"

enum class PerspectiveView : int {
    FirstPerson = 0,
    ThirdPersonBack = 1,
    ThirdPersonFront = 2
};

struct PerspectiveEvent : public Event {
    int perspective = 0;
    int overridePerspective = -1;

    explicit PerspectiveEvent(int currentPerspective)
        : perspective(currentPerspective) {
    }

    [[nodiscard]] PerspectiveView view() const {
        return static_cast<PerspectiveView>(perspective);
    }

    [[nodiscard]] bool isFirstPerson() const {
        return perspective == 0;
    }

    [[nodiscard]] bool isThirdPerson() const {
        return perspective > 0;
    }

    [[nodiscard]] bool isOverridden() const {
        return overridePerspective >= 0;
    }

    void setOverride(int newPerspective) {
        overridePerspective = newPerspective;
    }

    void setOverride(PerspectiveView newPerspective) {
        overridePerspective = static_cast<int>(newPerspective);
    }
};

using PerspectiveViewEvent = PerspectiveEvent;
