#include "modules/module_manager.h"
#include "input/input_manager.h"
#include "events/key_event.h"
#include "modules/tools/ContainerTransfer.h"
#include "events/event_bus.h"
#include "minecraft/sdk/client/client_instance.h"
#include "utils/logger.h"
#include "ui/notifications.h"

ModuleManager& ModuleManager::instance() {
    static ModuleManager manager;
    return manager;
}

void ModuleManager::initialize() {
    std::unique_lock lock(m_mutex);
    if (m_initialized) {
        return;
    }

    LOG_INFO("Initializing ModuleManager...");

    auto watermark = std::make_unique<Watermark>();
    watermark->set_enabled(true);
    m_modules.push_back(std::move(watermark));

    auto skinStealer = std::make_unique<SkinStealer>();
    m_modules.push_back(std::move(skinStealer));

    auto containerTransfer = std::make_unique<ContainerTransfer>();
    m_modules.push_back(std::move(containerTransfer));

    rebuild_categories();

    EventBus::instance().subscribe<KeyEvent>([this](KeyEvent& event) {
        if (InputManager::instance().is_binding()) return;
        if (ClientInstance::isClientUiOpen() || ClientInstance::isGameUiOpen()) return;

        std::shared_lock moduleLock(m_mutex);
        for (const auto& module : m_modules) {
            if (!module || module->name() == "SkinStealer" || module->keybind() != event.key) continue;

            const bool wasEnabled = module->enabled();
            if (module->hold_to_enable()) {
                if (event.action == KeyAction::Press)   module->set_enabled(true);
                if (event.action == KeyAction::Release) module->set_enabled(false);
            }
            else if (event.action == KeyAction::Press) {
                module->toggle();
            }
            if (wasEnabled != module->enabled()) {
                ui::notifications::push(module->name(),
                    module->enabled() ? "Feature enabled by hotkey." : "Feature disabled by hotkey.",
                    module->enabled() ? ui::notifications::Level::Success : ui::notifications::Level::Info);
            }
        }
    });

    LOG_INFO("Registered {} modules across {} categories.", m_modules.size(), m_categories.size());
    m_initialized = true;
}

void ModuleManager::shutdown() {
    std::unique_lock lock(m_mutex);
    LOG_INFO("Shutting down ModuleManager...");
    for (const auto& module : m_modules) {
        if (module && module->enabled()) {
            module->set_enabled(false);
        }
    }
    m_categories.clear();
    m_modules.clear();
    m_initialized = false;
}

void ModuleManager::on_imgui_render() {
    std::shared_lock lock(m_mutex);
    for (const auto& module : m_modules) {
        if (module && module->enabled()) {
            module->on_imgui_render();
        }
    }
}

std::vector<Module*> ModuleManager::modules() const {
    std::shared_lock lock(m_mutex);
    std::vector<Module*> result;
    result.reserve(m_modules.size());
    for (const auto& mod : m_modules) {
        if (mod) result.push_back(mod.get());
    }
    return result;
}

std::vector<std::string> ModuleManager::categories() const {
    std::shared_lock lock(m_mutex);
    return m_categories;
}

std::vector<Module*> ModuleManager::modules_in_category(const std::string& category) const {
    std::shared_lock lock(m_mutex);
    std::vector<Module*> result;

    for (const auto& module : m_modules) {
        if (module && module->category() == category) {
            result.push_back(module.get());
        }
    }

    return result;
}

void ModuleManager::add_module(std::unique_ptr<Module> module) {
    if (module) {
        m_modules.push_back(std::move(module));
    }
}

void ModuleManager::rebuild_categories() {
    m_categories.clear();

    constexpr std::array<const char*, 2> defaultCategories{
        "Visual",
        "Configs"
    };

    for (const char* category : defaultCategories) {
        m_categories.emplace_back(category);
    }

    for (const auto& module : m_modules) {
        if (!module) {
            continue;
        }

        const std::string& category = module->category();
        if (std::find(m_categories.begin(), m_categories.end(), category) == m_categories.end()) {
            m_categories.push_back(category);
        }
    }
}
