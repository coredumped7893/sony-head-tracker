// main_linux.cpp
// Linux CLI. Probes hidraw nodes for the Android Head Tracker, dumps its raw
// and decoded input reports, and streams filtered orientation over UDP.
#include "sony_head_tracker/bluetooth_recovery.hpp"
#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/hid_backend.hpp"
#include "sony_head_tracker/linux_support.hpp"
#include "sony_head_tracker/macos_support.hpp"
#include "sony_head_tracker/orientation.hpp"
#include "sony_head_tracker/output_udp.hpp"
#include "sony_head_tracker/version.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kExitNoPermission = 77; // EX_NOPERM, sysexits.h
constexpr int kExitNotFound = 2;
constexpr int kExitConnectFailed = 3;

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

unsigned parseUnsigned(std::string_view text) {
    const std::string value(text);
    char* end = nullptr;
    const auto parsed = std::strtoul(value.c_str(), &end, 10);
    return end == value.c_str() || *end != '\0' ? 0 : static_cast<unsigned>(parsed);
}

bool deadlineReached(std::chrono::steady_clock::time_point deadline, unsigned seconds) {
    return seconds != 0 && std::chrono::steady_clock::now() >= deadline;
}

bool waitInterruptibly(std::chrono::seconds duration,
                       std::chrono::steady_clock::time_point deadline,
                       unsigned seconds) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (!stopRequested && !deadlineReached(deadline, seconds) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !stopRequested && !deadlineReached(deadline, seconds);
}

int reportNoTracker(const std::vector<sony::DeviceInfo>& devices, std::wostream& out) {
    const auto denied = std::find_if(devices.begin(), devices.end(),
                                     [](const auto& device) { return device.accessDenied; });
    if (denied != devices.end()) {
        out << std::format(
            L"\nAndroid Head Tracker candidate found at {}, but access was denied. "
            L"Install the udev rule (see docs/LINUX.md), then reconnect the headset.\n",
            denied->path);
        return kExitNoPermission;
    }
    out << L"\nNo verified Android Head Tracker HID sensor was found.\n";
    return kExitNotFound;
}

int runProbe() {
    sony::HidBackend hid;
    const auto devices = hid.enumerate();
    for (const auto& device : devices) printDevice(device);
    if (verifiedDevice(devices) != devices.end()) {
        std::wcout << L"\nVerified Android head tracker found.\n";
        return 0;
    }
    return reportNoTracker(devices, std::wcout);
}

int runDump(int argc, char** argv) {
    unsigned seconds{};
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--seconds" && index + 1 < argc) {
            seconds = parseUnsigned(argv[++index]);
        } else {
            std::wcerr << L"Invalid dump option. Use 'help'.\n";
            return 1;
        }
    }
    sony::HidBackend hid;
    const auto devices = hid.enumerate();
    const auto selected = verifiedDevice(devices);
    if (selected == devices.end()) return reportNoTracker(devices, std::wcerr);
    for (const auto& device : devices) printDevice(device);
    if (!hid.connect(
            *selected,
            [](const auto& bytes) { std::wcout << sony::hexDump(bytes) << L'\n'; },
            [](sony::MotionSample sample) {
                std::wcout << std::format(
                    L"SAMPLE rotation=({:.6f},{:.6f},{:.6f}) rate={:.1f}pps",
                    sample.rotationVector.x, sample.rotationVector.y,
                    sample.rotationVector.z, sample.packetsPerSecond);
                if (sample.angularVelocity) {
                    std::wcout << std::format(
                        L" gyro=({:.6f},{:.6f},{:.6f})",
                        sample.angularVelocity->x, sample.angularVelocity->y,
                        sample.angularVelocity->z);
                }
                std::wcout << L'\n';
            })) {
        std::wcerr << L"Could not connect/configure the verified Android head tracker.\n";
        return kExitConnectFailed;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!stopRequested && !deadlineReached(deadline, seconds) && hid.connected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    hid.disconnect();
    return 0;
}

int runBridge(int argc, char** argv) {
    std::uint16_t port = 4242;
    unsigned seconds{};
    sony::FilterConfig config;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--port" && index + 1 < argc) {
            const auto value = parseUnsigned(argv[++index]);
            if (value < 1 || value > 65534) {
                std::wcerr << L"--port must be between 1 and 65534.\n";
                return 1;
            }
            port = static_cast<std::uint16_t>(value);
        } else if (option == "--seconds" && index + 1 < argc) {
            seconds = parseUnsigned(argv[++index]);
        } else if (option == "--smoothing" && index + 1 < argc) {
            std::string value(argv[++index]);
            char* end = nullptr;
            const auto parsed = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0') return 1;
            config.smoothing = std::clamp(parsed, 0.01, 1.0);
        } else if (option == "--invert" && index + 1 < argc) {
            const std::string axes(argv[++index]);
            config.axes.sign = {1.0, 1.0, 1.0};
            for (const auto axis : axes) {
                const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(axis)));
                if (lower == 'x') config.axes.sign[0] = -1.0;
                else if (lower == 'y') config.axes.sign[1] = -1.0;
                else if (lower == 'z') config.axes.sign[2] = -1.0;
                else if (axes != "none") return 1;
            }
        } else if (option == "--axis-map" && index + 1 < argc) {
            const std::string map(argv[++index]);
            if (map.size() != 3) return 1;
            std::array<bool, 3> used{};
            for (unsigned output = 0; output < 3; ++output) {
                const auto axis = static_cast<char>(std::tolower(static_cast<unsigned char>(map[output])));
                const unsigned source = axis == 'x' ? 0 : axis == 'y' ? 1 : axis == 'z' ? 2 : 3;
                if (source > 2 || used[source]) return 1;
                used[source] = true;
                config.axes.source[output] = source;
            }
        } else {
            std::wcerr << L"Invalid bridge option. Use 'help'.\n";
            return 1;
        }
    }

    sony::UdpOutput udp;
    if (!udp.open("127.0.0.1", port)) {
        std::wcerr << L"Could not open UDP output.\n";
        return 4;
    }
    sony::OrientationFilter filter(config);
    std::mutex filterMutex;
    sony::HidBackend hid;
    std::atomic_bool sampleReceived{};
    sony::DeviceInfo lastDevice;
    bool accessDenied{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

    std::wcout << std::format(
        L"Streaming head tracking:\n  OpenTrack -> UDP 127.0.0.1:{}\n"
        L"  JSON      -> UDP 127.0.0.1:{}\n"
        L"Recenter: kill -USR1 {}\nStop: Ctrl+C\n",
        port, port + 1, static_cast<long>(getpid()));

    const auto output = [&](sony::MotionSample sample) {
        sampleReceived = true;
        std::lock_guard lock(filterMutex);
        auto filtered = filter.process(std::move(sample));
        udp.send(filtered);
        std::wcout << std::format(L"\rYPR {:7.2f} {:7.2f} {:7.2f}  {:5.1f} pps   ",
                                  filtered.euler.yaw, filtered.euler.pitch,
                                  filtered.euler.roll, filtered.packetsPerSecond)
                   << std::flush;
    };

    const auto connect = [&]() {
        const auto devices = hid.enumerate();
        const auto selected = verifiedDevice(devices);
        if (selected == devices.end()) {
            accessDenied = std::any_of(devices.begin(), devices.end(),
                                       [](const auto& device) { return device.accessDenied; });
            if (accessDenied) reportNoTracker(devices, std::wcerr);
            return false;
        }
        accessDenied = false;
        if (!selected->bluetoothAddress.empty() &&
            !sony::saveLastVerifiedBluetoothAddress(selected->bluetoothAddress)) {
            std::wcerr << L"Warning: could not persist the verified tracker identity.\n";
        }
        udp.setDeviceLabel(selected->product);
        if (!selected->product.empty()) std::wcout << L"Tracking headset: " << selected->product << L'\n';
        lastDevice = *selected;
        return hid.connect(lastDevice, {}, output);
    };

    const bool initiallyConnected = connect();
    if (!initiallyConnected && sony::loadLastVerifiedBluetoothAddress().empty()) {
        if (accessDenied) return kExitNoPermission;
        std::wcerr << L"No verified Android Head Tracker was found.\n";
        return kExitConnectFailed;
    }
    if (!initiallyConnected) {
        std::wcerr << L"The previously verified tracker is not yet visible; entering reconnect backoff.\n";
    }

    std::size_t backoffIndex{};
    bool waitingForFirstSample{initiallyConnected};
    bool reopenAttempted{};
    auto configuredAt = std::chrono::steady_clock::now();

    const auto handleStall = [&] {
        const auto action = sony::streamStallAction(
            std::chrono::steady_clock::now() - configuredAt, reopenAttempted);
        if (action == sony::StallAction::reopenHid) {
            std::wcerr << L"\nNo valid sample within 5 s of configuration; reopening the hidraw node once.\n";
            reopenAttempted = true;
            hid.disconnect();
            hid.connect(lastDevice, {}, output);
            configuredAt = std::chrono::steady_clock::now();
        } else if (action == sony::StallAction::backoff) {
            std::wcerr << L"\nStill no valid sample after reopening; falling back to reconnect backoff.\n";
            hid.disconnect();
        }
    };

    const auto reconnectWithBackoff = [&] {
        hid.disconnect();
        const auto delay = sony::reconnectBackoffSeconds(backoffIndex);
        if (backoffIndex < 4) ++backoffIndex;
        std::wcerr << std::format(L"\nDisconnected; reconnecting in {} second(s).\n", delay);
        if (!waitInterruptibly(std::chrono::seconds(delay), deadline, seconds)) return;
        sampleReceived = false;
        if (!connect()) return;
        waitingForFirstSample = true;
        reopenAttempted = false;
        configuredAt = std::chrono::steady_clock::now();
        std::wcerr << L"Tracker configured; waiting for the first valid sample.\n";
    };

    while (!stopRequested && !deadlineReached(deadline, seconds)) {
        if (recenterRequested.exchange(false)) {
            std::lock_guard lock(filterMutex);
            filter.recenter();
            std::wcerr << L"\nRecenter requested.\n";
        }
        if (waitingForFirstSample && sampleReceived) {
            waitingForFirstSample = false;
            reopenAttempted = false;
            backoffIndex = 0;
            std::wcerr << L"\nFirst valid sample received.\n";
        } else if (waitingForFirstSample && hid.connected()) {
            handleStall();
        }
        if (!hid.connected()) reconnectWithBackoff();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    hid.disconnect();
    std::wcout << std::format(L"\nStopped after {} UDP sample(s).\n", udp.packetsSent());
    return 0;
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
    if (command == "dump") return runDump(argc, argv);
    if (command == "bridge") return runBridge(argc, argv);
    std::wcerr << L"Unknown command. Use 'help'.\n";
    return 1;
}
