#include "client.h"

#include "config/config_manager.h"
#include "events/client_instance_update_event.h"
#include "events/event_bus.h"
#include "minecraft/hooks/hook_manager.h"
#include "modules/module_manager.h"
#include "render/dx12/dx12_hook.h"
#include <utils/logger.h>

#include <atomic>


namespace {
    LPTOP_LEVEL_EXCEPTION_FILTER g_previousExceptionFilter = nullptr;
    std::atomic_bool g_exceptionLogged{false};

    LONG WINAPI chanter_unhandled_exception_filter(EXCEPTION_POINTERS* exceptionInfo) {
        if (!g_exceptionLogged.exchange(true, std::memory_order_relaxed) &&
            exceptionInfo && exceptionInfo->ExceptionRecord) {
            const auto* record = exceptionInfo->ExceptionRecord;
            LOG_FATAL("Unhandled Windows exception: code=0x{:08X}, address={:p}.",
                static_cast<unsigned int>(record->ExceptionCode),
                record->ExceptionAddress);

            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                record->NumberParameters >= 2) {
                LOG_FATAL("Access violation: operation={}, memory address={:p}.",
                    static_cast<unsigned int>(record->ExceptionInformation[0]),
                    reinterpret_cast<void*>(record->ExceptionInformation[1]));
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

Client& Client::instance() {
    static Client client;
    return client;
}

void Client::initialize(HMODULE module) {
    m_module = module;

    Logger::instance().initialize("ChanterClient", true, true);
    g_exceptionLogged.store(false, std::memory_order_relaxed);
    g_previousExceptionFilter = SetUnhandledExceptionFilter(&chanter_unhandled_exception_filter);
    LOG_INFO("Initializing ChanterClient for Minecraft Bedrock v26.60...");

    ModuleManager::instance().initialize();

    if (!minecraft::hooks::initialize()) {
        LOG_ERROR("Game hook initialization failed. No further hooks will be installed.");
        shutdown();
        FreeLibraryAndExitThread(m_module, 1);
        return;
    }

    if (!dx12_hook::start(m_module)) {
        LOG_ERROR("Failed to start the DirectX overlay. Shutting ChanterClient down.");
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

    LOG_INFO("ChanterClient initialized successfully. Press INSERT for the menu or END to unload.");
}

void Client::shutdown() {
    LOG_INFO("Shutting down ChanterClient...");
    SetUnhandledExceptionFilter(g_previousExceptionFilter);
    g_previousExceptionFilter = nullptr;
    dx12_hook::stop();
    minecraft::hooks::shutdown();
    ModuleManager::instance().shutdown();
    LOG_INFO("ChanterClient shutdown complete.");
    Logger::instance().shutdown();
}
