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

TEST(address_ignores_separator_style) {
    CHECK(normalizeBluetoothAddress(L"a1-b2-c3-d4-e5-f6") == "A1B2C3D4E5F6");
    CHECK(normalizeBluetoothAddress(L"a1b2c3d4e5f6") == "A1B2C3D4E5F6");
    CHECK(normalizeBluetoothAddress(L"a1:b2-c3:d4-e5:f6") == "A1B2C3D4E5F6");
}

TEST(address_skips_non_hex_characters_before_counting_digits) {
    CHECK(normalizeBluetoothAddress(L"g1:b2:c3:d4:e5:f6") == "");
    CHECK(normalizeBluetoothAddress(L"a1:b2:c3:d4:e5:f6g") == "A1B2C3D4E5F6");
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

namespace {

std::vector<DescriptorField> ultWearFields() { return ultWearDescriptor().fields; }

std::vector<std::uint8_t> ultWearReport() {
    return {0x01, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0xFF, 0x7F, 0x00, 0x00, 0x01, 0x80, 0x07};
}

} // namespace

TEST(feature_plan_on_ult_wear_sets_power_reporting_and_interval) {
    const auto fields = ultWearFields();
    const auto plan = planFeatureConfiguration(fields, 1);
    CHECK(plan.has_value());
    if (!plan) return;
    CHECK(plan->size() == 3);
    if (plan->size() != 3) return;
    std::vector<std::uint8_t> report{0x01, 0x00};
    const FeatureReportLayout layout{1, true};
    for (const auto& item : *plan) {
        CHECK(item.reportId == 1);
        CHECK(writeFeatureBits(report, layout, item.bitOffset, item.bitSize, item.value));
    }
    CHECK(report == (std::vector<std::uint8_t>{0x01, 0x07}));
    const auto at = [&](std::size_t offset) {
        return std::find_if(plan->begin(), plan->end(), [&](const auto& item) { return item.bitOffset == offset; });
    };
    CHECK(at(0) != plan->end() && at(0)->bitSize == 1 && at(0)->value == 1);
    CHECK(at(1) != plan->end() && at(1)->bitSize == 1 && at(1)->value == 1);
    CHECK(at(2) != plan->end() && at(2)->bitSize == 6 && at(2)->value == 1);
}

TEST(feature_plan_selector_value_is_logical_min_plus_array_index) {
    auto fields = ultWearFields();
    for (auto& field : fields) {
        if (field.feature && field.isArray) field.logicalMin = 1, field.logicalMax = 2;
    }
    const auto plan = planFeatureConfiguration(fields, 1);
    CHECK(plan.has_value());
    if (!plan || plan->size() != 3) return;
    CHECK((*plan)[0].value == 2);
    CHECK((*plan)[1].value == 2);
}

TEST(feature_plan_rejects_missing_field_bad_size_and_out_of_range_value) {
    const auto fields = ultWearFields();
    for (const auto missing : {kPowerFull, kReportingAllEvents, kReportInterval}) {
        auto trimmed = fields;
        std::erase_if(trimmed, [&](const auto& field) { return field.feature && field.usage == missing; });
        CHECK(!planFeatureConfiguration(trimmed, 1).has_value());
    }
    CHECK(!planFeatureConfiguration(fields, 64).has_value());
    CHECK(!planFeatureConfiguration(fields, -1).has_value());
    for (const auto size : {0, 65}) {
        auto bad = fields;
        for (auto& field : bad) {
            if (field.feature && field.usage == kReportInterval) field.bitSize = static_cast<std::uint16_t>(size);
        }
        CHECK(!planFeatureConfiguration(bad, 1).has_value());
    }
    CHECK(!planFeatureConfiguration({}, 1).has_value());
}

TEST(decode_ult_wear_report_scales_rotation_gyro_and_counter) {
    const auto fields = ultWearFields();
    const auto sample = decodeMotionSample(ultWearReport(), fields);
    CHECK(sample.has_value());
    if (!sample) return;
    CHECK_NEAR(sample->rotationVector.x, descriptorScale(16384, -32767, 32767, -314159264, 314159265, -8), 1e-9);
    CHECK_NEAR(sample->rotationVector.x, 1.57094, 1e-4);
    CHECK_NEAR(sample->rotationVector.y, 0.0, 1e-7);
    CHECK_NEAR(sample->rotationVector.z, -sample->rotationVector.x, 1e-4);
    CHECK(sample->angularVelocity.has_value());
    if (sample->angularVelocity) {
        CHECK_NEAR(sample->angularVelocity->x, 32.0, 1e-9);
        CHECK_NEAR(sample->angularVelocity->z, -32.0, 1e-9);
    }
    CHECK(!sample->acceleration.has_value());
    CHECK(sample->resetCounter == 7);
}

TEST(decode_rejects_wrong_id_truncated_and_empty_reports) {
    const auto fields = ultWearFields();
    auto wrongId = ultWearReport();
    wrongId[0] = 0x02;
    CHECK(!decodeMotionSample(wrongId, fields).has_value());
    const auto report = ultWearReport();
    CHECK(!decodeMotionSample(std::span(report).first(5), fields).has_value());
    CHECK(!decodeMotionSample(std::span<const std::uint8_t>{}, fields).has_value());
    CHECK(!decodeMotionSample(std::span(report).first(1), fields).has_value());
}

TEST(decode_truncated_after_rotation_drops_gyro_but_keeps_sample) {
    const auto fields = ultWearFields();
    const auto report = ultWearReport();
    const auto sample = decodeMotionSample(std::span(report).first(10), fields);
    CHECK(sample.has_value());
    if (sample) CHECK(!sample->angularVelocity.has_value());
}

TEST(decode_unnumbered_report_with_scalar_gyro_and_acceleration) {
    const auto field = [](std::uint16_t usage, std::uint32_t bitOffset, std::uint16_t count) {
        DescriptorField item;
        item.usagePage = kSensorPage;
        item.usage = usage;
        item.reportCount = count;
        item.bitSize = 8;
        item.logicalMin = -127;
        item.logicalMax = 127;
        item.physicalMin = -127;
        item.physicalMax = 127;
        item.bitOffset = bitOffset;
        return item;
    };
    const std::vector<DescriptorField> fields{
        field(kRotation, 0, 3), field(kAngularVelocityX, 24, 1), field(kAngularVelocityY, 32, 1),
        field(kAccelerationX, 40, 1)};
    const std::vector<std::uint8_t> report{1, 2, 3, 0x0A, 0xF6, 0x05};
    const auto sample = decodeMotionSample(report, fields);
    CHECK(sample.has_value());
    if (!sample) return;
    CHECK_NEAR(sample->rotationVector.z, 3.0, 1e-9);
    CHECK(sample->angularVelocity.has_value());
    if (sample->angularVelocity) {
        CHECK_NEAR(sample->angularVelocity->x, 10.0, 1e-9);
        CHECK_NEAR(sample->angularVelocity->y, -10.0, 1e-9);
    }
    CHECK(sample->acceleration.has_value());
    if (sample->acceleration) CHECK_NEAR(sample->acceleration->x, 5.0, 1e-9);
}
