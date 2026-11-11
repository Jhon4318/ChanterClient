#pragma once

#include <filesystem>
#include <string>
#include <vector>

class ConfigManager {
public:
    static ConfigManager& instance();
    std::filesystem::path get_config_directory() const;

    bool save_config(const std::string& configName = "default");
    bool save_active_config();
    bool load_config(const std::string& configName = "default");
    bool delete_config(const std::string& configName);

    std::vector<std::string> get_available_configs() const;
    std::string active_config() const;
    bool config_exists(const std::string& configName) const;
    bool is_valid_config_name(const std::string& configName) const;

private:
    ConfigManager() = default;
    void ensure_directory_exists() const;
    std::string m_activeConfig = "default";
};
