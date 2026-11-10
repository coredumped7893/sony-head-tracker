// linux_support.hpp
// Pure decisions used by the Linux platform layer. These helpers intentionally
// contain no Linux system headers so they can be covered by hardware-free CI.
#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace sony {

enum class BluetoothRecoveryStep { none, connect, forceReconnect };

inline BluetoothRecoveryStep bluetoothRecoveryStep(bool trackerVisible, bool haveAddress, unsigned stage) {
    if (trackerVisible || !haveAddress) return BluetoothRecoveryStep::none;
    if (stage == 0) return BluetoothRecoveryStep::connect;
    if (stage == 1) return BluetoothRecoveryStep::forceReconnect;
    return BluetoothRecoveryStep::none;
}

// Advances only when BlueZ reports connected while the HID collection is still
// absent, so a powered-off headset keeps retrying the same stage.
inline unsigned nextBluetoothRecoveryStage(unsigned stage, bool bluezConnected) {
    return bluezConnected ? std::min(stage + 1, 2u) : stage;
}

enum class StallAction { wait, reopenHid, backoff };

// Consulted only while waiting for the first sample after configuring; the
// caller clears reopenAttempted when a valid sample arrives.
inline StallAction streamStallAction(
    std::chrono::steady_clock::duration sinceConfigured,
    bool reopenAttempted,
    std::chrono::steady_clock::duration timeout = std::chrono::seconds(5)) {
    if (sinceConfigured < timeout) return StallAction::wait;
    return reopenAttempted ? StallAction::backoff : StallAction::reopenHid;
}

inline std::string normalizeBluetoothAddress(std::wstring_view value) {
    std::string result;
    result.reserve(12);
    for (const auto character : value) {
        if (character >= L'0' && character <= L'9') {
            result.push_back(static_cast<char>(character));
        } else if (character >= L'a' && character <= L'f') {
            result.push_back(static_cast<char>(character - L'a' + 'A'));
        } else if (character >= L'A' && character <= L'F') {
            result.push_back(static_cast<char>(character));
        }
    }
    return result.size() == 12 ? result : std::string{};
}

// The XDG spec says relative XDG_CONFIG_HOME values must be ignored.
inline std::filesystem::path linuxConfigDirectory(const char* xdgConfigHome, const char* home) {
    if (xdgConfigHome && *xdgConfigHome) {
        const std::filesystem::path base(xdgConfigHome);
        if (base.is_absolute()) return base / "sony-head-tracker";
    }
    if (home && *home) return std::filesystem::path(home) / ".config" / "sony-head-tracker";
    return {};
}

} // namespace sony
