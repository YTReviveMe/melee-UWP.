#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace melee::xbox {
namespace fs = std::filesystem;

struct Entry {
    fs::path path;
    bool directory;
};

struct Listing {
    std::vector<Entry> entries;
    std::string error;
};

struct StorageSetup {
    fs::path directory;
    std::string error;
};

fs::path internal_root();
fs::path active_root();
std::vector<fs::path> storage_roots();
StorageSetup setup_storage(const fs::path& root, bool internal);
StorageSetup activate_storage(const fs::path& root, bool internal);
bool is_within(const fs::path& root, const fs::path& path);
bool is_disc_image(const fs::path& path);
Listing list_directory(const fs::path& root, const fs::path& directory);
std::string utf8_path(const fs::path& path);
}
