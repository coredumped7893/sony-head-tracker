// linux_support.hpp
// Pure decisions used by the Linux platform layer. These helpers intentionally
// contain no Linux system headers so they can be covered by hardware-free CI.
#pragma once

#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_report_descriptor.hpp"
#include "sony_head_tracker/hid_usages.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

struct HidUevent {
    std::uint16_t bus{};
    std::uint16_t vendorId{};
    std::uint16_t productId{};
    std::wstring name;
    std::wstring uniq; // the headset's Bluetooth address; HID_PHYS is the adapter's
};

namespace detail {

inline std::optional<std::uint32_t> parseHex(std::string_view text) {
    std::uint32_t value{};
    const auto end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value, 16);
    if (error != std::errc{} || stop != end) return std::nullopt;
    return value;
}

inline std::wstring widen(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const auto character : text) result.push_back(static_cast<unsigned char>(character));
    return result;
}

} // namespace detail

// Parses the KEY=value lines of a hidraw `device/uevent`. HID_ID is
// BBBB:VVVVVVVV:PPPPPPPP in hex; anything else returns nullopt.
inline std::optional<HidUevent> parseHidUevent(std::string_view text) {
    std::optional<HidUevent> result;
    std::wstring name, uniq;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (key == "HID_NAME") {
            name = detail::widen(value);
        } else if (key == "HID_UNIQ") {
            uniq = detail::widen(value);
        } else if (key == "HID_ID") {
            const auto firstColon = value.find(':');
            const auto secondColon = firstColon == std::string_view::npos
                ? std::string_view::npos : value.find(':', firstColon + 1);
            if (secondColon == std::string_view::npos) return std::nullopt;
            const auto bus = detail::parseHex(value.substr(0, firstColon));
            const auto vendor = detail::parseHex(value.substr(firstColon + 1, secondColon - firstColon - 1));
            const auto product = detail::parseHex(value.substr(secondColon + 1));
            if (!bus || !vendor || !product) return std::nullopt;
            result = HidUevent{static_cast<std::uint16_t>(*bus), static_cast<std::uint16_t>(*vendor),
                               static_cast<std::uint16_t>(*product), {}, {}};
        }
    }
    if (!result) return std::nullopt;
    result->name = std::move(name);
    result->uniq = std::move(uniq);
    return result;
}

// Mirrors the macOS marker scan: from the marker to the first 0x00/0xFF or the
// end of the report; empty when the marker is absent.
inline std::string markerFromFeatureReport(std::span<const std::uint8_t> report) {
    const auto begin = reinterpret_cast<const char*>(report.data());
    const std::string_view text(begin, report.size());
    const auto marker = text.find(kMarker);
    if (marker == std::string_view::npos) return {};
    const auto tail = text.substr(marker);
    const auto end = tail.find_first_of(std::string_view("\0\xff", 2));
    return std::string(tail.substr(0, end));
}

inline DeviceInfo describeHidrawDevice(std::wstring path, std::wstring instanceId,
                                       const ParsedReportDescriptor& descriptor, const HidUevent& uevent,
                                       std::string sensorDescription, bool accessDenied) {
    DeviceInfo device;
    device.path = std::move(path);
    device.instanceId = std::move(instanceId);
    device.product = uevent.name;
    device.bluetoothName = uevent.name;
    device.bluetoothAddress = uevent.uniq;
    device.usagePage = descriptor.usagePage;
    device.usage = descriptor.usage;
    device.vendorId = uevent.vendorId;
    device.productId = uevent.productId;
    device.inputReportBytes = descriptor.inputReportBytes;
    device.featureReportBytes = descriptor.featureReportBytes;
    device.fields = descriptor.fields;
    device.androidHeadTracker = sensorDescription.starts_with(kMarker);
    device.sensorDescription = std::move(sensorDescription);
    device.accessDenied = accessDenied;
    return device;
}

} // namespace sony
