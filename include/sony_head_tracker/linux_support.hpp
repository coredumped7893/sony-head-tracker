// linux_support.hpp
// Pure decisions used by the Linux platform layer. These helpers intentionally
// contain no Linux system headers so they can be covered by hardware-free CI.
#pragma once

#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_report_descriptor.hpp"
#include "sony_head_tracker/hid_usages.hpp"
#include "sony_head_tracker/macos_support.hpp"
#include "sony_head_tracker/types.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct FeatureAssignment {
    std::uint8_t reportId{};
    std::size_t bitOffset{};
    std::size_t bitSize{};
    std::uint64_t value{};
};

// Power Full, Reporting All Events and the report interval, as bit-level writes.
// Selector fields are arrays: the value written is logicalMin + the usage's index.
inline std::optional<std::vector<FeatureAssignment>> planFeatureConfiguration(
    std::span<const DescriptorField> fields, std::int64_t reportIntervalRaw) {
    std::vector<FeatureAssignment> plan;
    const auto add = [&](std::uint16_t usage, bool selector, std::int64_t raw) {
        const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& candidate) {
            return candidate.feature && candidate.usagePage == kSensorPage && candidate.usage == usage &&
                   candidate.isArray == selector;
        });
        if (field == fields.end() || field->bitSize == 0 || field->bitSize > 64) return false;
        if (selector) raw = field->logicalMin + field->arrayIndex;
        if (raw < field->logicalMin || raw > field->logicalMax) return false;
        plan.push_back({field->reportId, field->bitOffset, field->bitSize, static_cast<std::uint64_t>(raw)});
        return true;
    };
    if (!add(kPowerFull, true, 0) || !add(kReportingAllEvents, true, 0) ||
        !add(kReportInterval, false, reportIntervalRaw)) {
        return std::nullopt;
    }
    return plan;
}

// Decodes one hidraw input report. Numbered reports arrive with the report ID
// as the first byte. packetsPerSecond and receivedAt are left to the caller.
inline std::optional<MotionSample> decodeMotionSample(
    std::span<const std::uint8_t> report, std::span<const DescriptorField> fields) {
    const bool prefixed = std::any_of(fields.begin(), fields.end(), [](const auto& field) {
        return !field.feature && field.reportId != 0;
    });
    if (prefixed && report.empty()) return std::nullopt;
    const auto reportId = prefixed ? report[0] : std::uint8_t{};
    const FeatureReportLayout layout{reportId, prefixed};

    MotionSample sample;
    Vec3 gyro{};
    Vec3 accel{};
    bool gotRotation{};
    bool gotGyro{};
    bool gotAccel{};
    for (const auto& field : fields) {
        if (field.feature || field.constant || field.usagePage != kSensorPage || field.reportId != reportId) continue;
        const auto raw = [&](std::size_t index) -> std::optional<std::int64_t> {
            const auto bits = readFeatureBits(report, layout, field.bitOffset + index * field.bitSize, field.bitSize);
            if (!bits) return std::nullopt;
            auto value = static_cast<std::int64_t>(*bits);
            if (field.logicalMin < 0 && field.bitSize < 64 && ((*bits >> (field.bitSize - 1)) & 1u)) {
                value -= std::int64_t{1} << field.bitSize;
            }
            return value;
        };
        const auto scaled = [&](std::size_t index) -> std::optional<double> {
            const auto value = raw(index);
            if (!value) return std::nullopt;
            return descriptorScale(*value, field.logicalMin, field.logicalMax, field.physicalMin,
                                   field.physicalMax, field.unitExponent);
        };
        const auto readVector = [&](Vec3& out) {
            if (field.reportCount < 3) return false;
            const auto x = scaled(0);
            const auto y = scaled(1);
            const auto z = scaled(2);
            if (!x || !y || !z) return false;
            out = {*x, *y, *z};
            return true;
        };
        const auto scalar = [&](Vec3& out, std::uint16_t first) {
            const auto value = scaled(0);
            if (!value) return false;
            out[field.usage - first] = *value;
            return true;
        };
        const auto usage = field.usage;
        if (usage == kRotation) {
            gotRotation = readVector(sample.rotationVector);
        } else if (usage == kAngularVelocity || usage == kAngularVelocityVector) {
            gotGyro = readVector(gyro);
        } else if (usage == kAccelerationVector) {
            gotAccel = readVector(accel);
        } else if (usage >= kAngularVelocityX && usage <= kAngularVelocityZ) {
            gotGyro = scalar(gyro, kAngularVelocityX) || gotGyro;
        } else if (usage >= kAccelerationX && usage <= kAccelerationZ) {
            gotAccel = scalar(accel, kAccelerationX) || gotAccel;
        } else if (usage == kResetCounter) {
            if (const auto counter = raw(0)) sample.resetCounter = static_cast<std::uint8_t>(*counter);
        }
    }
    if (!gotRotation) return std::nullopt;
    if (gotGyro) sample.angularVelocity = gyro;
    if (gotAccel) sample.acceleration = accel;
    return sample;
}

} // namespace sony
