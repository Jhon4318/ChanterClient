#pragma once

#include "events/event.h"

class LevelRenderer;
class ScreenContext;

struct LevelRenderEvent : public Event {
    LevelRenderer* levelRenderer = nullptr;
    ScreenContext* screenContext = nullptr;

    LevelRenderEvent(LevelRenderer* renderer, ScreenContext* context)
        : levelRenderer(renderer), screenContext(context) {
    }
};
