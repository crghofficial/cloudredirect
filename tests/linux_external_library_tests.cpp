#include "autocloud_scan.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    auto pattern = (fs::temp_directory_path() / "cloudredirect-library-XXXXXX").string();
    const char* fixture = mkdtemp(pattern.data());
    assert(fixture);
    const fs::path root(fixture);
    const auto steam = root / "Steam";
    const auto external = root / "External Drive";
    const auto offline = root / "Unavailable Drive";
    fs::create_directories(steam / "config");
    fs::create_directories(steam / "steamapps/compatdata/101/pfx");
    fs::create_directories(steam / "steamapps/compatdata/202/pfx");
    fs::create_directories(external / "steamapps/compatdata/202/pfx");

    std::ofstream(steam / "config/libraryfolders.vdf")
        << "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\" \"" << offline.string()
        << "\"\n\t}\n\t\"2\"\n\t{\n\t\t\"path\" \"" << external.string() << "\"\n\t}\n}\n";
    std::ofstream(steam / "steamapps/appmanifest_101.acf") << "\"installdir\" \"Internal Game\"\n";
    std::ofstream(external / "steamapps/appmanifest_202.acf") << "\"installdir\" \"External Game\"\n";

    const auto internal = AutoCloudScan::GetRootTokenDirectories(steam.string(), 101, 42);
    const auto moved = AutoCloudScan::GetRootTokenDirectories(steam.string(), 202, 42);
    assert(internal.at("%WinMyDocuments%") ==
           (steam / "steamapps/compatdata/101/pfx/drive_c/users/steamuser/My Documents/").string());
    assert(moved.at("%WinMyDocuments%") ==
           (external / "steamapps/compatdata/202/pfx/drive_c/users/steamuser/My Documents/").string());
    assert(moved.at("%GameInstall%") == (external / "steamapps/common/External Game/").string());
    assert(AutoCloudScan::IsAppInstalled(steam.string(), 202));

    fs::rename(external / "steamapps/compatdata/202/pfx", external / "retired-prefix");
    const auto remaining = AutoCloudScan::GetRootTokenDirectories(steam.string(), 202, 42);
    assert(remaining.at("%WinMyDocuments%") ==
           (steam / "steamapps/compatdata/202/pfx/drive_c/users/steamuser/My Documents/").string());

    fs::rename(external, root / "Unmounted");
    assert(!AutoCloudScan::IsAppInstalled(steam.string(), 202));
    fs::remove_all(root);
}
