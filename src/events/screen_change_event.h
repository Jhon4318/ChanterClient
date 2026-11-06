#pragma once

#include "events/event.h"
#include <string>

class ClientInstance;

struct ScreenChangeEvent : public Event {
    ClientInstance* clientInstance = nullptr;
    std::string previousScreen;
    std::string currentScreen;
    std::string overrideScreen;
    bool overridden = false;

    ScreenChangeEvent(ClientInstance* instance, std::string prev, std::string current)
        : clientInstance(instance), previousScreen(std::move(prev)), currentScreen(std::move(current)) {
    }

    [[nodiscard]] bool isOverridden() const {
        return overridden;
    }

    void setOverride(std::string newScreen) {
        overrideScreen = std::move(newScreen);
        overridden = true;
    }
};
