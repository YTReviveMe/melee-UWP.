#include "storage.hpp"
#include "storage_api.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <system_error>

#ifdef MELEE_XBOX_UWP
#include <future>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#endif

namespace melee::xbox {
namespace {
fs::path selected_root = LR"(D:\developmentfiles\Melee)";
std::string settings_path = R"(D:\developmentfiles\Melee\settings\)";
std::string saves_path = R"(D:\developmentfiles\Melee\saves\)";

#ifdef MELEE_XBOX_UWP
winrt::Windows::Storage::StorageFolder removable_folder(const fs::path& path) {
    using namespace winrt::Windows::Storage;
    const auto devices = KnownFolders::RemovableDevices().GetFoldersAsync().get();
    for (const auto& device : devices) {
        const fs::path root(device.Path().c_str());
        if (!is_within(root, path)) continue;
        auto folder = device;
        auto part = path.begin();
        for (auto root_part = root.begin(); root_part != root.end(); ++root_part) ++part;
        for (; part != path.end(); ++part)
            folder = folder.GetFolderAsync(part->wstring()).get();
        return folder;
    }
    throw winrt::hresult_error(winrt::hresult{static_cast<int32_t>(0x80004005u)},
                               L"The selected USB drive is unavailable.");
}

void log_usb_error(const char* operation, const fs::path& path,
                   const winrt::hresult_error& error) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "USB %s %s failed (0x%08X): %s",
                 operation, utf8_path(path).c_str(),
                 static_cast<unsigned int>(error.code().value),
                 winrt::to_string(error.message()).c_str());
}
#endif

void set_root(const fs::path& root) {
    selected_root = root;
    settings_path = utf8_path(root / "settings") + "\\";
    saves_path = utf8_path(root / "saves") + "\\";
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool writable(const fs::path& directory, const char* extension) {
    const auto path = directory / ("melee-probe-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + extension);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.put('1');
    file.close();
    const bool ready = file.good();
    std::error_code ec;
    fs::remove(path, ec);
    return ready && !ec;
}

bool copy_tree(const fs::path& source, const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(source, ec)) return !ec;
    if (!fs::is_directory(source, ec)) return false;
    for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const auto& entry = *it;
        if (entry.is_symlink(ec)) {
            it.disable_recursion_pending();
            continue;
        }
        const auto destination = target / entry.path().lexically_relative(source);
        if (entry.is_directory(ec)) {
            fs::create_directories(destination, ec);
        } else if (entry.is_regular_file(ec)) {
            fs::create_directories(destination.parent_path(), ec);
            if (!ec) fs::copy_file(entry.path(), destination, fs::copy_options::skip_existing, ec);
        }
        if (ec) break;
    }
    return !ec;
}

void migrate_local_data() {
#ifdef MELEE_XBOX_UWP
    const auto migrated = internal_root() / "settings" / "migration.cfg";
    std::error_code ec;
    if (fs::exists(migrated, ec)) return;
    char* local = SDL_GetPrefPath(nullptr, "melee-pc");
    if (!local) return;
    const fs::path source(local);
    SDL_free(local);
    for (fs::directory_iterator it(source, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto extension = lowercase(utf8_path(it->path().extension()));
        if (extension == ".db" || extension == ".log" || extension == ".tmp" ||
            extension == ".lock") continue;
        fs::copy_file(it->path(), internal_root() / "settings" / it->path().filename(),
                      fs::copy_options::skip_existing, ec);
        if (ec) break;
    }
    if (ec) return;
    for (const char* region : {"USA", "EUR", "JAP"})
        if (!copy_tree(source / region, internal_root() / "saves" / region)) return;
    if (!copy_tree(source / "music", internal_root() / "music")) return;
    std::ofstream marker(migrated);
    marker << '1';
#endif
}

void sort_entries(Listing& listing) {
    std::sort(listing.entries.begin(), listing.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        return lowercase(utf8_path(a.path.filename())) < lowercase(utf8_path(b.path.filename()));
    });
}
}

#ifdef MELEE_XBOX_UWP
extern "C" int melee_xbox_prepare_internal_storage(void) {
    if (!setup_storage(internal_root(), true).error.empty()) return 0;
    migrate_local_data();
    std::ifstream marker(internal_root() / "settings" / "storage-root.cfg");
    std::string external;
    std::getline(marker, external);
    if (!external.empty()) activate_storage(fs::path(external), false);
    return 1;
}

extern "C" const char* melee_xbox_settings_directory(void) {
    return settings_path.c_str();
}

extern "C" const char* melee_xbox_saves_directory(void) {
    return saves_path.c_str();
}
#endif

fs::path internal_root() { return LR"(D:\developmentfiles\Melee)"; }
fs::path active_root() { return selected_root; }

std::string utf8_path(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::vector<fs::path> storage_roots() {
    std::vector<fs::path> roots{internal_root()};
#ifdef MELEE_XBOX_UWP
    auto external = std::async(std::launch::async, [] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::vector<fs::path> paths;
        const auto folders = winrt::Windows::Storage::KnownFolders::RemovableDevices()
                                 .GetFoldersAsync().get();
        for (const auto& folder : folders)
            paths.emplace_back(folder.Path().c_str());
        return paths;
    }).get();
    for (auto& path : external) {
        if (path.is_absolute() && std::find(roots.begin(), roots.end(), path) == roots.end())
            roots.push_back(std::move(path));
    }
#endif
    return roots;
}

StorageSetup setup_storage(const fs::path& root, bool internal) {
    StorageSetup setup{root, {}};
    if (internal) {
        std::error_code ec;
        fs::create_directories(root, ec);
        if (!ec) {
            for (const char* name : {"cache", "textures", "music", "saves", "settings"}) {
                fs::create_directories(root / name, ec);
                if (ec) break;
            }
        }
        if (ec) setup.error = "Could not set up Melee folders: " + ec.message();
        return setup;
    }
#ifdef MELEE_XBOX_UWP
    try {
        return std::async(std::launch::async, [root] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            using namespace winrt::Windows::Storage;
            auto drive = removable_folder(root);
            auto melee = drive.CreateFolderAsync(L"Melee", CreationCollisionOption::OpenIfExists).get();
            for (const wchar_t* name : {L"textures", L"music", L"saves", L"settings"})
                melee.CreateFolderAsync(name, CreationCollisionOption::OpenIfExists).get();
            return StorageSetup{fs::path(melee.Path().c_str()), {}};
        }).get();
    } catch (const winrt::hresult_error& e) {
        log_usb_error("setup", root, e);
        setup.error = "Could not set up USB Melee folder: " + winrt::to_string(e.message());
    }
#endif
    return setup;
}

StorageSetup activate_storage(const fs::path& root, bool internal) {
    auto setup = setup_storage(root, internal);
    if (!setup.error.empty()) return setup;
    const auto& directory = setup.directory;
    if (!writable(directory / "settings", ".cfg") ||
        !writable(directory / "saves", ".gci")) {
        setup.error = "Saves and settings cannot be written to " + utf8_path(directory);
        return setup;
    }
    if (directory != selected_root) {
        if (!copy_tree(selected_root / "settings", directory / "settings") ||
            !copy_tree(selected_root / "saves", directory / "saves")) {
            setup.error = "Could not copy saves and settings to " + utf8_path(directory);
            return setup;
        }
    }
    const auto marker_path = internal_root() / "settings" / "storage-root.cfg";
    if (internal) {
        std::error_code ec;
        fs::remove(marker_path, ec);
    } else {
        std::ofstream marker(marker_path, std::ios::trunc);
        marker << utf8_path(root);
        if (!marker) {
            setup.error = "Could not remember the selected USB drive.";
            return setup;
        }
    }
    set_root(directory);
    return setup;
}

bool is_within(const fs::path& root, const fs::path& path) {
    if (!root.is_absolute() || !path.is_absolute()) return false;
    auto r = root.lexically_normal();
    auto p = path.lexically_normal();
    auto ri = r.begin(), pi = p.begin();
    for (; ri != r.end(); ++ri, ++pi) {
        if (pi == p.end()) return false;
#ifdef _WIN32
        if (lowercase(utf8_path(*ri)) != lowercase(utf8_path(*pi))) return false;
#else
        if (*ri != *pi) return false;
#endif
    }
    return true;
}

bool is_disc_image(const fs::path& path) {
    const auto ext = lowercase(utf8_path(path.extension()));
    return ext == ".iso" || ext == ".gcm" || ext == ".ciso" || ext == ".rvz" ||
           ext == ".gcz" || ext == ".wia";
}

Listing list_directory(const fs::path& root, const fs::path& directory) {
    Listing listing;
    if (!is_within(root, directory)) {
        listing.error = "Folder is outside the selected storage device.";
        return listing;
    }
#ifdef MELEE_XBOX_UWP
    if (!is_within(internal_root(), root)) {
        try {
            listing = std::async(std::launch::async, [directory] {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                Listing listing;
                auto folder = removable_folder(directory);
                for (const auto& child : folder.GetFoldersAsync().get())
                    listing.entries.push_back({fs::path(child.Path().c_str()), true});
                for (const auto& file : folder.GetFilesAsync().get()) {
                    fs::path path(file.Path().c_str());
                    if (is_disc_image(path)) listing.entries.push_back({std::move(path), false});
                }
                return listing;
            }).get();
        } catch (const winrt::hresult_error& e) {
            log_usb_error("browse", directory, e);
            listing.error = winrt::to_string(e.message());
        }
        sort_entries(listing);
        return listing;
    }
#endif
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    if (ec) {
        listing.error = ec.message();
        return listing;
    }
    for (size_t count = 0; it != end && count < 2048; it.increment(ec), ++count) {
        if (ec) break;
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status)) continue;
        const bool folder = fs::is_directory(status);
        if (folder || (fs::is_regular_file(status) && is_disc_image(it->path())))
            listing.entries.push_back({it->path(), folder});
    }
    if (ec) listing.error = ec.message();
    else if (it != end) listing.error = "Showing the first 2048 entries.";
    sort_entries(listing);
    return listing;
}
}
