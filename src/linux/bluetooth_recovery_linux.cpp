// bluetooth_recovery_linux.cpp
// Linux persistence of the last verified headset address under the XDG config
// directory. The address is stored in its normalized 12-hex form.
#include "sony_head_tracker/bluetooth_recovery.hpp"

#include "sony_head_tracker/linux_private_file.hpp"
#include "sony_head_tracker/linux_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include <pwd.h>
#include <unistd.h>

namespace sony {

namespace {

std::filesystem::path lastTrackerPath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        const auto* user = getpwuid(getuid());
        home = user ? user->pw_dir : nullptr;
    }
    const auto directory = linuxConfigDirectory(std::getenv("XDG_CONFIG_HOME"), home);
    if (directory.empty()) return {};
    return directory / "last-tracker";
}

} // namespace

bool saveLastVerifiedBluetoothAddress(std::wstring_view bluetoothAddress) {
    const auto normalized = normalizeBluetoothAddress(bluetoothAddress);
    if (normalized.empty()) return false;
    const auto path = lastTrackerPath();
    if (path.empty()) return false;
    return linux_platform::writePrivateFileAtomically(path, normalized + "\n");
}

std::wstring loadLastVerifiedBluetoothAddress() {
    const auto path = lastTrackerPath();
    if (path.empty()) return {};
    const auto content = linux_platform::readPrivateFile(path);
    std::wstring widened;
    for (const char byte : content) widened.push_back(static_cast<unsigned char>(byte));
    const auto normalized = normalizeBluetoothAddress(widened);
    return std::wstring(normalized.begin(), normalized.end());
}

} // namespace sony
