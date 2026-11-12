// bluetooth_recovery_linux.cpp
// Linux persistence of the last verified headset address under the XDG config
// directory, stored in its normalized 12-hex form, and BlueZ recovery of that
// exact paired headset over sd-bus.
#include "sony_head_tracker/bluetooth_recovery.hpp"

#include "sony_head_tracker/linux_private_file.hpp"
#include "sony_head_tracker/linux_support.hpp"

#include "sony_head_tracker/macos_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <pwd.h>
#include <systemd/sd-bus.h>
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

struct BusCloser {
    void operator()(sd_bus* bus) const { sd_bus_flush_close_unref(bus); }
};

struct MessageUnref {
    void operator()(sd_bus_message* message) const { sd_bus_message_unref(message); }
};

struct BusError {
    sd_bus_error value{};
    BusError() = default;
    BusError(const BusError&) = delete;
    BusError& operator=(const BusError&) = delete;
    ~BusError() { sd_bus_error_free(&value); }
};

using BusPtr = std::unique_ptr<sd_bus, BusCloser>;
using MessagePtr = std::unique_ptr<sd_bus_message, MessageUnref>;

constexpr const char* kBluez = "org.bluez";
constexpr const char* kDeviceInterface = "org.bluez.Device1";
constexpr std::uint64_t kDisconnectTimeoutUsec = 5'000'000;
constexpr std::uint64_t kConnectTimeoutUsec = 10'000'000;

// Only the error name is logged: the message and object path can embed the address.
void logBusFailure(const wchar_t* operation, int code, const sd_bus_error& error) {
    std::wstring name;
    for (const char* c = error.name; c && *c; ++c) name.push_back(static_cast<unsigned char>(*c));
    std::wcerr << std::format(L"BlueZ {} failed: code={} error={}\n", operation, code,
                              name.empty() ? L"none" : name);
}

struct DeviceProperties {
    std::string address;
    bool paired{};
    bool connected{};
};

int readDeviceProperties(sd_bus_message* message, DeviceProperties& properties) {
    int r = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char* name = nullptr;
        r = sd_bus_message_read(message, "s", &name);
        if (r < 0) return r;
        const std::string_view key = name;
        int flag = 0;
        const char* text = nullptr;
        if (key == "Address") {
            r = sd_bus_message_read(message, "v", "s", &text);
            if (r >= 0) properties.address = text;
        } else if (key == "Paired") {
            r = sd_bus_message_read(message, "v", "b", &flag);
            properties.paired = flag != 0;
        } else if (key == "Connected") {
            r = sd_bus_message_read(message, "v", "b", &flag);
            properties.connected = flag != 0;
        } else {
            r = sd_bus_message_skip(message, "v");
        }
        if (r < 0) return r;
        r = sd_bus_message_exit_container(message);
        if (r < 0) return r;
    }
    if (r < 0) return r;
    r = sd_bus_message_exit_container(message);
    return r < 0 ? r : 0;
}

// Returns 1 and fills `properties` when the object is the target paired device.
int scanInterfaces(sd_bus_message* message, const std::string& target,
                   DeviceProperties& properties) {
    int r = sd_bus_message_enter_container(message, 'a', "{sa{sv}}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(message, 'e', "sa{sv}")) > 0) {
        const char* interfaceName = nullptr;
        r = sd_bus_message_read(message, "s", &interfaceName);
        if (r < 0) return r;
        if (std::string_view(interfaceName) == kDeviceInterface) {
            DeviceProperties candidate;
            r = readDeviceProperties(message, candidate);
            if (r < 0) return r;
            const auto address = normalizeBluetoothAddress(
                std::wstring(candidate.address.begin(), candidate.address.end()));
            if (candidate.paired && address == target) {
                properties = candidate;
                return 1;
            }
        } else {
            r = sd_bus_message_skip(message, "a{sv}");
            if (r < 0) return r;
        }
        r = sd_bus_message_exit_container(message);
        if (r < 0) return r;
    }
    if (r < 0) return r;
    r = sd_bus_message_exit_container(message);
    return r < 0 ? r : 0;
}

// The adapter is not assumed: a host can have several, so the device is found
// by address rather than by a constructed object path.
bool findPairedDevice(sd_bus* bus, const std::string& target, std::string& objectPath,
                      bool& connected) {
    BusError error;
    sd_bus_message* raw = nullptr;
    int r = sd_bus_call_method(bus, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &error.value, &raw, "");
    MessagePtr reply(raw);
    if (r < 0) {
        logBusFailure(L"GetManagedObjects", r, error.value);
        return false;
    }
    r = sd_bus_message_enter_container(reply.get(), 'a', "{oa{sa{sv}}}");
    while (r >= 0 && (r = sd_bus_message_enter_container(reply.get(), 'e', "oa{sa{sv}}")) > 0) {
        const char* path = nullptr;
        r = sd_bus_message_read(reply.get(), "o", &path);
        if (r < 0) break;
        DeviceProperties properties;
        r = scanInterfaces(reply.get(), target, properties);
        if (r > 0) {
            objectPath = path;
            connected = properties.connected;
            return true;
        }
        if (r < 0) break;
        r = sd_bus_message_exit_container(reply.get());
    }
    if (r < 0) std::wcerr << std::format(L"BlueZ device list could not be parsed: code={}\n", r);
    return false;
}

bool readConnected(sd_bus* bus, const std::string& objectPath) {
    BusError error;
    int connected = 0;
    const int r = sd_bus_get_property_trivial(bus, kBluez, objectPath.c_str(), kDeviceInterface,
                                              "Connected", &error.value, 'b', &connected);
    return r >= 0 && connected != 0;
}

int callDeviceMethod(sd_bus* bus, const std::string& objectPath, const char* member,
                     const wchar_t* label, std::uint64_t timeoutUsec) {
    BusError error;
    sd_bus_message* raw = nullptr;
    int r = sd_bus_message_new_method_call(bus, &raw, kBluez, objectPath.c_str(),
                                           kDeviceInterface, member);
    MessagePtr request(raw);
    if (r >= 0) r = sd_bus_call(bus, request.get(), timeoutUsec, &error.value, nullptr);
    if (r < 0) logBusFailure(label, r, error.value);
    return r;
}

} // namespace

BluetoothRecoveryResult recoverPairedBluetoothHid(
    std::wstring_view bluetoothAddress,
    std::wstring_view,
    bool forceBasebandReconnect,
    const CancellationFlag* cancellation) {
    BluetoothRecoveryResult result;
    const auto target = normalizeBluetoothAddress(bluetoothAddress);
    if (target.empty()) return result;

    sd_bus* rawBus = nullptr;
    const int openCode = sd_bus_open_system(&rawBus);
    BusPtr bus(rawBus);
    if (openCode < 0) {
        std::wcerr << std::format(L"BlueZ system bus could not be opened: code={}\n", openCode);
        return result;
    }
    // Calls passing timeout 0 would otherwise wait the ~25 s sd-bus default, uncancellable.
    sd_bus_set_method_call_timeout(bus.get(), kDisconnectTimeoutUsec);

    std::string objectPath;
    bool connectedNow{};
    if (!findPairedDevice(bus.get(), target, objectPath, connectedNow)) return result;
    result.pairedDeviceFound = true;
    result.wasConnected = connectedNow;

    const auto cancelled = [&] { return cancellation && cancellation->stopRequested(); };
    const auto isConnected = [&] { return readConnected(bus.get(), objectPath); };

    if (forceBasebandReconnect && result.wasConnected) {
        result.forcedBasebandReconnect = true;
        result.closeStatus = callDeviceMethod(bus.get(), objectPath, "Disconnect",
                                              L"Disconnect", kDisconnectTimeoutUsec);
        if (result.closeStatus < 0) return result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!cancelled() && isConnected() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (cancelled()) {
            result.cancelled = true;
            return result;
        }
        if (isConnected()) {
            std::wcerr << L"BlueZ headset is still connected after Disconnect; skipping reconnect\n";
            return result;
        }
    }

    if (!isConnected()) {
        result.openStatus = callDeviceMethod(bus.get(), objectPath, "Connect", L"Connect",
                                             kConnectTimeoutUsec);
    }

    const auto waitResult = waitForBluetoothConnection(
        result.openStatus >= 0, isConnected,
        [](std::chrono::milliseconds slice) { std::this_thread::sleep_for(slice); }, cancelled,
        [] { return std::chrono::steady_clock::now(); });
    result.connectionTimedOut = waitResult == BluetoothConnectionWaitResult::timedOut;
    result.cancelled = waitResult == BluetoothConnectionWaitResult::cancelled;
    result.connected = bluetoothConnectionConfirmed(waitResult, isConnected());

    // The kernel creates the hidraw node shortly after BlueZ reports connected.
    for (int slice = 0; result.connected && slice < 10 && !cancelled(); ++slice) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return result;
}

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
