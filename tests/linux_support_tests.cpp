// linux_support_tests.cpp
// Pure Linux decisions: BlueZ recovery stages, stream-stall action, Bluetooth
// address normalization, and the XDG config directory.
#include "test_framework.hpp"
#include "ult_wear_descriptor.hpp"

#include "sony_head_tracker/hid_report_descriptor.hpp"
#include "sony_head_tracker/linux_support.hpp"
#include "sony_head_tracker/macos_support.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace sony;

TEST(recovery_step_follows_stage_when_tracker_missing) {
    CHECK(bluetoothRecoveryStep(false, true, 0) == BluetoothRecoveryStep::connect);
    CHECK(bluetoothRecoveryStep(false, true, 1) == BluetoothRecoveryStep::forceReconnect);
    CHECK(bluetoothRecoveryStep(false, true, 2) == BluetoothRecoveryStep::none);
    CHECK(bluetoothRecoveryStep(false, true, 100) == BluetoothRecoveryStep::none);
}

TEST(recovery_step_is_none_when_visible_or_address_unknown) {
    for (unsigned stage : {0u, 1u, 2u, 100u}) {
        CHECK(bluetoothRecoveryStep(true, true, stage) == BluetoothRecoveryStep::none);
        CHECK(bluetoothRecoveryStep(false, false, stage) == BluetoothRecoveryStep::none);
    }
}

TEST(recovery_stage_advances_only_when_bluez_connected) {
    CHECK(nextBluetoothRecoveryStage(0, true) == 1);
    CHECK(nextBluetoothRecoveryStage(1, true) == 2);
    CHECK(nextBluetoothRecoveryStage(2, true) == 2);
    CHECK(nextBluetoothRecoveryStage(0, false) == 0);
    CHECK(nextBluetoothRecoveryStage(1, false) == 1);
}

TEST(recovery_sequence_is_connect_then_force_reconnect_then_none) {
    std::vector<BluetoothRecoveryStep> steps;
    unsigned stage = 0;
    for (int i = 0; i < 4; ++i) {
        steps.push_back(bluetoothRecoveryStep(false, true, stage));
        stage = nextBluetoothRecoveryStage(stage, true);
    }
    CHECK(steps.size() == 4);
    if (steps.size() != 4) return;
    CHECK(steps[0] == BluetoothRecoveryStep::connect);
    CHECK(steps[1] == BluetoothRecoveryStep::forceReconnect);
    CHECK(steps[2] == BluetoothRecoveryStep::none);
    CHECK(steps[3] == BluetoothRecoveryStep::none);
}

TEST(stall_waits_until_timeout) {
    using namespace std::chrono_literals;
    CHECK(streamStallAction(4900ms, false) == StallAction::wait);
    CHECK(streamStallAction(0ms, false) == StallAction::wait);
}

TEST(stall_reopens_once_then_backs_off) {
    using namespace std::chrono_literals;
    CHECK(streamStallAction(5s, false) == StallAction::reopenHid);
    CHECK(streamStallAction(5s, true) == StallAction::backoff);
}

TEST(stall_loop_without_samples_reopens_exactly_once) {
    using namespace std::chrono_literals;
    bool reopenAttempted = false;
    int reopens = 0;
    for (int tick = 0; tick < 100; ++tick) {
        const auto action = streamStallAction(std::chrono::seconds(tick), reopenAttempted);
        if (action == StallAction::reopenHid) {
            ++reopens;
            reopenAttempted = true;
        }
    }
    CHECK(reopens == 1);
}

TEST(stall_honours_custom_timeout) {
    using namespace std::chrono_literals;
    CHECK(streamStallAction(1s, false, 2s) == StallAction::wait);
    CHECK(streamStallAction(2s, false, 2s) == StallAction::reopenHid);
}

TEST(address_normalizes_lower_and_upper_colon_forms) {
    CHECK(normalizeBluetoothAddress(L"a1:b2:c3:d4:e5:f6") == "A1B2C3D4E5F6");
    CHECK(normalizeBluetoothAddress(L"A1:B2:C3:D4:E5:F6") == "A1B2C3D4E5F6");
}

TEST(address_rejects_wrong_digit_count) {
    CHECK(normalizeBluetoothAddress(L"a1:b2:c3:d4:e5:f") == "");
    CHECK(normalizeBluetoothAddress(L"a1:b2:c3:d4:e5:f6:0") == "");
    CHECK(normalizeBluetoothAddress(L"") == "");
}

TEST(xdg_absolute_config_home_wins) {
    CHECK(linuxConfigDirectory("/xdg", "/home/u") == std::filesystem::path("/xdg/sony-head-tracker"));
}

TEST(xdg_relative_or_empty_falls_back_to_home) {
    const std::filesystem::path expected("/home/u/.config/sony-head-tracker");
    CHECK(linuxConfigDirectory("relative/dir", "/home/u") == expected);
    CHECK(linuxConfigDirectory("", "/home/u") == expected);
    CHECK(linuxConfigDirectory(nullptr, "/home/u") == expected);
}

TEST(xdg_without_any_base_is_empty) {
    CHECK(linuxConfigDirectory(nullptr, nullptr).empty());
    CHECK(linuxConfigDirectory("", "").empty());
    CHECK(linuxConfigDirectory("relative", nullptr).empty());
}

namespace {

constexpr std::string_view kLiveUevent =
    "DRIVER=hid-generic\n"
    "HID_ID=0005:0000054C:00000F1E\n"
    "HID_NAME=ULT WEAR\n"
    "HID_PHYS=aa:bb:cc:dd:ee:ff\n"
    "HID_UNIQ=11:22:33:44:55:66\n"
    "MODALIAS=hid:b0005g0001v0000054Cp00000F1E\n";

const std::string kMarkerText = "#AndroidHeadTracker#1.0";

std::vector<std::uint8_t> featureReport(std::string_view text) {
    std::vector<std::uint8_t> report{0x02};
    report.insert(report.end(), text.begin(), text.end());
    report.insert(report.end(), {0x00, 0xFF, 0xFF});
    return report;
}

ParsedReportDescriptor ultWearDescriptor() {
    return parseReportDescriptor(kUltWear).value_or(ParsedReportDescriptor{});
}

DeviceInfo describe(std::uint16_t vendor, std::uint16_t product, std::string marker, bool accessDenied = false) {
    HidUevent uevent;
    uevent.vendorId = vendor;
    uevent.productId = product;
    uevent.name = L"Some Headset";
    uevent.uniq = L"11:22:33:44:55:66";
    return describeHidrawDevice(L"/dev/hidraw3", L"0005:1234:5678.0001", ultWearDescriptor(), uevent,
                                std::move(marker), accessDenied);
}

} // namespace

TEST(uevent_live_shape_yields_identity) {
    const auto uevent = parseHidUevent(kLiveUevent);
    CHECK(uevent.has_value());
    if (!uevent) return;
    CHECK(uevent->bus == 0x0005);
    CHECK(uevent->vendorId == 0x054C);
    CHECK(uevent->productId == 0x0F1E);
    CHECK(uevent->name == L"ULT WEAR");
    CHECK(uevent->uniq == L"11:22:33:44:55:66");
}

TEST(uevent_without_uniq_has_empty_uniq) {
    const auto uevent = parseHidUevent("HID_ID=0005:0000054C:00000F1E\nHID_NAME=ULT WEAR\n");
    CHECK(uevent.has_value());
    if (uevent) CHECK(uevent->uniq.empty());
}

TEST(uevent_uniq_is_not_taken_from_phys) {
    const auto uevent = parseHidUevent("HID_ID=0005:0000054C:00000F1E\nHID_PHYS=aa:bb:cc:dd:ee:ff\n");
    CHECK(uevent.has_value());
    if (uevent) CHECK(uevent->uniq.empty());
}

TEST(uevent_without_valid_hid_id_is_rejected) {
    CHECK(!parseHidUevent("").has_value());
    CHECK(!parseHidUevent("HID_NAME=ULT WEAR\n").has_value());
    CHECK(!parseHidUevent("HID_ID=0005:0000054C\n").has_value());
    CHECK(!parseHidUevent("HID_ID=0005:0000054C:0000ZZZZ\n").has_value());
    CHECK(!parseHidUevent("HID_ID=:0000054C:00000F1E\n").has_value());
    CHECK(!parseHidUevent("HID_ID=0005:0000054C:00000F1E:1\n").has_value());
}

TEST(uevent_handles_missing_trailing_newline_and_keeps_low_16_bits) {
    const auto uevent = parseHidUevent("HID_NAME=X\nHID_ID=0003:12345678:9ABCDEF0");
    CHECK(uevent.has_value());
    if (!uevent) return;
    CHECK(uevent->bus == 0x0003);
    CHECK(uevent->vendorId == 0x5678);
    CHECK(uevent->productId == 0xDEF0);
}

TEST(marker_is_extracted_up_to_padding) {
    CHECK(markerFromFeatureReport(featureReport(kMarkerText)) == kMarkerText);
}

TEST(marker_runs_to_end_without_padding) {
    const std::string text = "xx" + kMarkerText;
    const std::vector<std::uint8_t> report(text.begin(), text.end());
    CHECK(markerFromFeatureReport(report) == kMarkerText);
}

TEST(marker_absent_or_empty_yields_empty_string) {
    CHECK(markerFromFeatureReport(featureReport("SomethingElse")).empty());
    CHECK(markerFromFeatureReport({}).empty());
}

TEST(descriptor_and_marker_verify_any_vid_pid) {
    const auto device = describe(0x1234, 0x5678, kMarkerText);
    CHECK(device.vendorId == 0x1234);
    CHECK(device.productId == 0x5678);
    CHECK(device.usagePage == 0x20);
    CHECK(device.usage == 0xE1);
    CHECK(device.inputReportBytes == 14);
    CHECK(device.featureReportBytes == 40);
    CHECK(!device.fields.empty());
    CHECK(device.androidHeadTracker);
    CHECK(!device.accessDenied);
    CHECK(isVerifiedAndroidTracker(device));
}

TEST(identity_fields_come_from_uevent) {
    const auto device = describe(0x1234, 0x5678, kMarkerText);
    CHECK(device.path == L"/dev/hidraw3");
    CHECK(device.instanceId == L"0005:1234:5678.0001");
    CHECK(device.product == L"Some Headset");
    CHECK(device.bluetoothName == L"Some Headset");
    CHECK(device.bluetoothAddress == L"11:22:33:44:55:66");
    CHECK(device.sensorDescription == kMarkerText);
}

TEST(descriptor_without_marker_is_not_verified) {
    const auto device = describe(0x1234, 0x5678, "");
    CHECK(device.usagePage == 0x20 && device.usage == 0xE1);
    CHECK(!device.androidHeadTracker);
    CHECK(!isVerifiedAndroidTracker(device));
}

TEST(access_denied_candidate_is_flagged_and_unverified) {
    const auto device = describe(0x054C, 0x0F1E, "", true);
    CHECK(device.accessDenied);
    CHECK(!isVerifiedAndroidTracker(device));
}
