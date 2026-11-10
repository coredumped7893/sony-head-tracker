// linux_support_tests.cpp
// Pure Linux decisions: BlueZ recovery stages, stream-stall action, Bluetooth
// address normalization, and the XDG config directory.
#include "test_framework.hpp"

#include "sony_head_tracker/linux_support.hpp"

#include <chrono>
#include <filesystem>
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
