#include "config/config_manager.h"
#include "modules/module_manager.h"
#include "utils/logger.h"
#include "utils/client_paths.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace {
std::string trim_name(std::string value) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [&](unsigned char ch) { return !isSpace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [&](unsigned char ch) { return !isSpace(ch); }).base(), value.end());
    return value;
}
std::string lower_name(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
}

ConfigManager& ConfigManager::instance() {
    static ConfigManager manager;
    return manager;
}
std::filesystem::path ConfigManager::get_config_directory() const {
    return utils::client_data_directory(L"configs", std::filesystem::current_path());
}
void ConfigManager::ensure_directory_exists() const {
    std::filesystem::create_directories(get_config_directory());
}
bool ConfigManager::is_valid_config_name(const std::string& configName) const {
    const std::string name = trim_name(configName);
    if (name.empty() || name.size() > 48 || name == "." || name == "..") return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == ' ' || ch == '_' || ch == '-';
    });
}
bool ConfigManager::config_exists(const std::string& configName) const {
    if (!is_valid_config_name(configName)) return false;
    try { return std::filesystem::is_regular_file(get_config_directory() / (trim_name(configName) + ".json")); }
    catch (...) { return false; }
}
std::string ConfigManager::active_config() const { return m_activeConfig; }
bool ConfigManager::save_active_config() { return save_config(m_activeConfig); }

bool ConfigManager::save_config(const std::string& configName) {
    const std::string name = trim_name(configName);
    if (!is_valid_config_name(name)) {
        LOG_WARN("Rejected invalid config profile name.");
        return false;
    }
    try {
        ensure_directory_exists();
        const std::filesystem::path filePath = get_config_directory() / (name + ".json");
        std::filesystem::path temporaryPath = filePath;
        temporaryPath += ".tmp";

        nlohmann::json root = nlohmann::json::object();
        nlohmann::json modulesJson = nlohmann::json::object();
        for (Module* module : ModuleManager::instance().modules()) {
            if (!module) continue;
            nlohmann::json modData;
            modData["enabled"] = module->enabled();
            modData["keybind"] = module->keybind();
            modData["hold_to_enable"] = module->hold_to_enable();
            nlohmann::json settingsJson = nlohmann::json::object();
            for (const auto& setting : module->settings()) {
                if (setting) settingsJson[setting->name()] = setting->serialize();
            }
            modData["settings"] = std::move(settingsJson);
            modulesJson[module->name()] = std::move(modData);
        }
        root["modules"] = std::move(modulesJson);

        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                LOG_ERROR("Failed to open temporary file for config profile '{}'.", name);
                return false;
            }
            file << root.dump(4);
            file.flush();
            if (!file.good()) {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(temporaryPath, ignored);
                LOG_ERROR("Failed while writing config profile '{}'.", name);
                return false;
            }
        }
        if (!MoveFileExW(temporaryPath.c_str(), filePath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            LOG_ERROR("Could not replace config profile '{}' (Win32 error {}).", name, error);
            return false;
        }
        m_activeConfig = name;
        LOG_INFO("Config profile '{}' saved successfully.", name);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception occurred while saving config '{}': {}", name, e.what());
        return false;
    }
}

bool ConfigManager::load_config(const std::string& configName) {
    const std::string name = trim_name(configName);
    if (!is_valid_config_name(name)) {
        LOG_WARN("Rejected invalid config profile name.");
        return false;
    }
    try {
        const std::filesystem::path filePath = get_config_directory() / (name + ".json");
        if (!std::filesystem::exists(filePath)) {
            LOG_WARN("Config profile '{}' does not exist.", name);
            return false;
        }
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file: '{}'.", name);
            return false;
        }
        nlohmann::json root;
        file >> root;
        if (!root.contains("modules") || !root["modules"].is_object()) {
            LOG_ERROR("Invalid config file structure for profile: '{}'.", name);
            return false;
        }

        const auto modulesJson = root["modules"];
        const auto modules = ModuleManager::instance().modules();

        // Apply values before enabling modules so on_enable() sees selected-profile settings.
        for (Module* module : modules) {
            if (!module || !modulesJson.contains(module->name())) continue;
            const auto& modData = modulesJson[module->name()];
            if (!modData.is_object()) continue;
            if (modData.contains("keybind") && modData["keybind"].is_number_integer())
                module->set_keybind(modData["keybind"].get<int>());
            if (modData.contains("hold_to_enable") && modData["hold_to_enable"].is_boolean())
                module->set_hold_to_enable(modData["hold_to_enable"].get<bool>());
            if (modData.contains("settings") && modData["settings"].is_object()) {
                const auto& settingsJson = modData["settings"];
                for (const auto& setting : module->settings()) {
                    if (setting && settingsJson.contains(setting->name()))
                        setting->deserialize(settingsJson[setting->name()]);
                }
            }
        }
        for (Module* module : modules) {
            if (!module || !modulesJson.contains(module->name())) continue;
            const auto& modData = modulesJson[module->name()];
            if (modData.is_object() && modData.contains("enabled") && modData["enabled"].is_boolean())
                module->set_enabled(modData["enabled"].get<bool>());
        }
        m_activeConfig = name;
        LOG_INFO("Config profile '{}' loaded successfully.", name);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception occurred while loading config '{}': {}", name, e.what());
        return false;
    }
}

bool ConfigManager::delete_config(const std::string& configName) {
    const std::string name = trim_name(configName);
    if (!is_valid_config_name(name) || lower_name(name) == "default") {
        LOG_WARN("Invalid or protected config profile deletion requested.");
        return false;
    }
    try {
        const std::filesystem::path filePath = get_config_directory() / (name + ".json");
        if (!std::filesystem::exists(filePath)) return false;
        if (!std::filesystem::remove(filePath)) return false;
        LOG_INFO("Deleted config profile: '{}'.", name);
        if (lower_name(m_activeConfig) == lower_name(name)) {
            m_activeConfig = "default";
            if (config_exists("default")) load_config("default");
            else save_config("default");
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to delete config profile '{}': {}", name, e.what());
        return false;
    }
}

std::vector<std::string> ConfigManager::get_available_configs() const {
    std::vector<std::string> configs;
    try {
        ensure_directory_exists();
        for (const auto& entry : std::filesystem::directory_iterator(get_config_directory())) {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                configs.push_back(entry.path().stem().string());
        }
        std::sort(configs.begin(), configs.end(), [](const std::string& a, const std::string& b) {
            return lower_name(a) < lower_name(b);
        });
    } catch (const std::exception& e) {
        LOG_WARN("Could not enumerate config profiles: {}", e.what());
    }
    return configs;
}
