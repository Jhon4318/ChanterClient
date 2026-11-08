#pragma once

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>

namespace utils {

inline void migrate_legacy_client_subdirectory(
    const std::filesystem::path& legacyDirectory,
    const std::filesystem::path& currentDirectory) {
    static std::mutex mutex;
    static std::set<std::filesystem::path> attempted;
    std::lock_guard<std::mutex> lock(mutex);

    if (!attempted.insert(currentDirectory).second) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(legacyDirectory, ec) || ec ||
        !std::filesystem::is_directory(legacyDirectory, ec) || ec) {
        return;
    }

    std::filesystem::create_directories(currentDirectory, ec);
    if (ec) {
        return;
    }

    std::filesystem::recursive_directory_iterator it(
        legacyDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        const auto source = it->path();
        const auto relative = source.lexically_relative(legacyDirectory);
        const auto destination = currentDirectory / relative;

        std::error_code entryError;
        if (it->is_directory(entryError) && !entryError) {
            std::filesystem::create_directories(destination, entryError);
        } else if (it->is_regular_file(entryError) && !entryError) {
            std::filesystem::create_directories(destination.parent_path(), entryError);
            if (!entryError && !std::filesystem::exists(destination, entryError) && !entryError) {
                std::filesystem::copy_file(
                    source, destination, std::filesystem::copy_options::none, entryError);
            }
        }

        it.increment(ec);
    }
}

inline std::filesystem::path client_data_directory(
    const std::filesystem::path& subdirectory,
    const std::filesystem::path& fallbackBase) {
    const wchar_t* appData = _wgetenv(L"APPDATA");
    if (appData == nullptr || *appData == L'\\0') {
        return fallbackBase / subdirectory;
    }

    const std::filesystem::path base(appData);
    const auto legacyDirectory = base / L"JhonClient" / subdirectory;
    const auto currentDirectory = base / L"ChanterClient" / subdirectory;
    migrate_legacy_client_subdirectory(legacyDirectory, currentDirectory);
    return currentDirectory;
}

} // namespace utils
