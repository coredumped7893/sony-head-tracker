// main_linux.cpp
// Linux CLI. Currently probes hidraw nodes for the Android Head Tracker; dump
// and bridge are added in the following port tasks.
#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_backend.hpp"
#include "sony_head_tracker/macos_support.hpp"
#include "sony_head_tracker/version.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitNoPermission = 77; // EX_NOPERM, sysexits.h
constexpr int kExitNotFound = 2;

std::atomic_bool stopRequested{};
std::atomic_bool recenterRequested{};

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) stopRequested = true;
    if (signal == SIGUSR1) recenterRequested = true;
}

void installSignals() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGUSR1, signalHandler);
}

void printUsage() {
    std::wcout
        << L"Sony Head Tracker for Linux " << sony::kVersion << L"\n"
        << L"Usage:\n"
        << L"  sony-head-tracker-linux probe\n"
        << L"  sony-head-tracker-linux dump [--seconds N]\n"
        << L"  sony-head-tracker-linux bridge [--port 4242] [--seconds N]\n"
        << L"                                  [--axis-map YXZ] [--invert XZ]\n"
        << L"                                  [--smoothing 0.18]\n"
        << L"  sony-head-tracker-linux help\n"
        << L"  sony-head-tracker-linux version\n";
}

void printDevice(const sony::DeviceInfo& device) {
    std::wcout << std::format(
        L"HID {}\n  manufacturer={} product={} transport=Linux hidraw\n"
        L"  usage=0x{:04X}:0x{:04X}, VID/PID={:04X}:{:04X}, input={} feature={}\n"
        L"  description={}\n  access={}\n  verified Android tracker={}\n",
        device.path, device.manufacturer, device.product,
        device.usagePage, device.usage, device.vendorId, device.productId,
        device.inputReportBytes, device.featureReportBytes,
        std::wstring(device.sensorDescription.begin(), device.sensorDescription.end()),
        device.accessDenied ? L"denied" : L"ok",
        device.androidHeadTracker ? L"yes" : L"no");
    for (const auto& field : device.fields) {
        std::wcout << std::format(
            L"    {} report={} usage=0x{:04X}:0x{:04X} count={} bits={} "
            L"logical={}..{} physical={}..{} exponent={}\n",
            field.feature ? L"feature" : L"input", field.reportId,
            field.usagePage, field.usage, field.reportCount, field.bitSize,
            field.logicalMin, field.logicalMax, field.physicalMin,
            field.physicalMax, field.unitExponent);
    }
}

std::vector<sony::DeviceInfo>::const_iterator verifiedDevice(
    const std::vector<sony::DeviceInfo>& devices) {
    return std::find_if(devices.begin(), devices.end(),
                        [](const auto& device) { return sony::isVerifiedAndroidTracker(device); });
}

int runProbe() {
    sony::HidBackend hid;
    const auto devices = hid.enumerate();
    for (const auto& device : devices) printDevice(device);
    if (verifiedDevice(devices) != devices.end()) {
        std::wcout << L"\nVerified Android head tracker found.\n";
        return 0;
    }
    const auto denied = std::find_if(devices.begin(), devices.end(),
                                     [](const auto& device) { return device.accessDenied; });
    if (denied != devices.end()) {
        std::wcout << std::format(
            L"\nAndroid Head Tracker candidate found at {}, but access was denied. "
            L"Install the udev rule (see docs/LINUX.md), then reconnect the headset.\n",
            denied->path);
        return kExitNoPermission;
    }
    std::wcout << L"\nNo verified Android Head Tracker HID sensor was found.\n";
    return kExitNotFound;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "help";
    if (command == "help" || command == "--help" || command == "-h") {
        printUsage();
        return 0;
    }
    if (command == "version" || command == "--version") {
        std::wcout << L"sony-head-tracker " << sony::kVersion << L"\n";
        return 0;
    }
    installSignals();
    if (command == "probe") return runProbe();
    if (command == "dump" || command == "bridge") {
        std::wcerr << L"'" << std::wstring(command.begin(), command.end()) << L"' is not implemented yet.\n";
        return 1;
    }
    std::wcerr << L"Unknown command. Use 'help'.\n";
    return 1;
}
