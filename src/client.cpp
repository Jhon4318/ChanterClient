#include "client.h"

#include "config/config_manager.h"
#include "events/client_instance_update_event.h"
#include "events/event_bus.h"
#include "minecraft/hooks/hook_manager.h"
#include "modules/module_manager.h"
#include "render/dx12/dx12_hook.h"
#include <utils/logger.h>

Client& Client::instance() {
    static Client client;
    return client;
}

void Client::initialize(HMODULE module) {
    m_module = module;

    Logger::instance().initialize("JhonClient", true, true);
    LOG_INFO("Initializing JhonClient for Minecraft Bedrock v26.60...");

    ModuleManager::instance().initialize();

    if (!minecraft::hooks::initialize()) {
        LOG_ERROR("Game hook initialization failed. No further hooks will be installed.");
        shutdown();
        FreeLibraryAndExitThread(m_module, 1);
        return;
    }

    if (!dx12_hook::start(m_module)) {
        LOG_ERROR("Failed to start the DirectX overlay. Shutting JhonClient down.");
        shutdown();
        FreeLibraryAndExitThread(m_module, 1);
        return;
    }

    EventBus::instance().subscribe<ClientInstanceUpdateEvent>([this](ClientInstanceUpdateEvent&) {
        if (!m_configLoaded) {
            m_configLoaded = true;
            LOG_INFO("Minecraft ClientInstance is ready. Loading default profile...");
            ConfigManager::instance().load_config("default");
        }
    });

    LOG_INFO("JhonClient initialized successfully. Press INSERT for the menu or END to unload.");
}

void Client::shutdown() {
    LOG_INFO("Shutting down JhonClient...");
    dx12_hook::stop();
    minecraft::hooks::shutdown();
    ModuleManager::instance().shutdown();
    LOG_INFO("JhonClient shutdown complete.");
    Logger::instance().shutdown();
}
