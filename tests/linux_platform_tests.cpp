// linux_platform_tests.cpp
// Linux platform layer: private atomic file, last-tracker persistence under
// XDG_CONFIG_HOME, and the UDP wire format over real loopback sockets.
#include "test_framework.hpp"

#include "sony_head_tracker/bluetooth_recovery.hpp"
#include "sony_head_tracker/linux_private_file.hpp"
#include "sony_head_tracker/output_udp.hpp"
#include "sony_head_tracker/protocol.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("sony-head-tracker-linux-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};

struct ScopedXdgConfigHome {
    std::optional<std::string> previous;

    explicit ScopedXdgConfigHome(const std::filesystem::path& value) {
        if (const char* old = std::getenv("XDG_CONFIG_HOME")) previous = old;
        ::setenv("XDG_CONFIG_HOME", value.c_str(), 1);
    }

    ~ScopedXdgConfigHome() {
        if (previous) ::setenv("XDG_CONFIG_HOME", previous->c_str(), 1);
        else ::unsetenv("XDG_CONFIG_HOME");
    }
};

bool hasTemporaryResidue(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) return true;
    }
    return false;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

unsigned modeOf(const std::filesystem::path& path) {
    struct stat status{};
    if (::stat(path.c_str(), &status) != 0) return 0xFFFF;
    return status.st_mode & 0777;
}

struct LoopbackReceiver {
    int fd{-1};
    std::uint16_t port{};

    LoopbackReceiver() = default;
    LoopbackReceiver(const LoopbackReceiver&) = delete;
    LoopbackReceiver& operator=(const LoopbackReceiver&) = delete;
    ~LoopbackReceiver() { close(); }

    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

    bool bind(std::uint16_t requestedPort) {
        close();
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        const timeval timeout{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(requestedPort);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return false;
        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
        port = ntohs(address.sin_port);
        return true;
    }

    std::string receive() const {
        std::array<char, 2048> buffer{};
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count < 0) return {};
        return std::string(buffer.data(), static_cast<std::size_t>(count));
    }
};

bool bindConsecutivePorts(LoopbackReceiver& first, LoopbackReceiver& second) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (first.bind(0) && first.port < 65534 &&
            second.bind(static_cast<std::uint16_t>(first.port + 1))) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(linux_private_file_round_trip_is_private_and_has_no_residue) {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "state";
    CHECK(sony::linux_platform::writePrivateFileAtomically(file, "first"));
    CHECK(sony::linux_platform::writePrivateFileAtomically(file, "second"));
    CHECK(sony::linux_platform::readPrivateFile(file) == "second");
    CHECK(modeOf(temporary.path) == 0700);
    CHECK(modeOf(file) == 0600);
    CHECK(!hasTemporaryResidue(temporary.path));
}

TEST(linux_private_file_rejects_final_symlinks_and_invalid_parents) {
    TemporaryDirectory temporary;
    const auto target = temporary.path / "target";
    const auto link = temporary.path / "state";
    {
        std::ofstream output(target);
        output << "old";
    }
    CHECK(::symlink(target.c_str(), link.c_str()) == 0);
    CHECK(!sony::linux_platform::writePrivateFileAtomically(link, "new"));
    CHECK(readText(target) == "old");
    CHECK(sony::linux_platform::readPrivateFile(link).empty());

    const auto parentFile = temporary.path / "not-a-directory";
    {
        std::ofstream output(parentFile);
        output << "x";
    }
    CHECK(!sony::linux_platform::writePrivateFileAtomically(parentFile / "state", "new"));
}

TEST(linux_last_tracker_address_persists_privately_under_xdg_config_home) {
    TemporaryDirectory temporary;
    const ScopedXdgConfigHome xdg(temporary.path);

    CHECK(sony::loadLastVerifiedBluetoothAddress().empty());
    CHECK(!sony::saveLastVerifiedBluetoothAddress(L"bogus"));
    CHECK(sony::loadLastVerifiedBluetoothAddress().empty());

    CHECK(sony::saveLastVerifiedBluetoothAddress(L"a1:b2:c3:d4:e5:f6"));
    const auto directory = temporary.path / "sony-head-tracker";
    const auto file = directory / "last-tracker";
    CHECK(readText(file) == "A1B2C3D4E5F6\n");
    CHECK(modeOf(file) == 0600);
    CHECK(modeOf(directory) == 0700);
    CHECK(sony::loadLastVerifiedBluetoothAddress() == L"A1B2C3D4E5F6");
}

TEST(linux_udp_output_sends_core_serializer_bytes_on_both_ports) {
    LoopbackReceiver openTrack;
    LoopbackReceiver json;
    CHECK(bindConsecutivePorts(openTrack, json));
    if (openTrack.fd < 0 || json.fd < 0) return;

    sony::MotionSample sample;
    sample.euler = {12.5, -3.25, 4.0};
    sample.orientation = {0.5, 0.5, 0.5, 0.5};
    sample.resetCounter = 7;
    sample.packetsPerSecond = 50.0;

    sony::UdpOutput output;
    CHECK(output.open("127.0.0.1", openTrack.port));
    output.setDeviceLabel(L"Test");
    output.send(sample);

    const auto pose = sony::toOpenTrackPose(sample);
    CHECK(openTrack.receive() == std::string(reinterpret_cast<const char*>(pose.data()),
                                              pose.size() * sizeof(double)));
    CHECK(json.receive() == sony::toJson(sample, sony::jsonEscapeString("Test")));
    CHECK(output.packetsSent() == 1);
    CHECK(output.port() == openTrack.port);
}

TEST(linux_udp_output_rejects_unusable_ports_and_hosts) {
    sony::UdpOutput output;
    CHECK(!output.open("127.0.0.1", 65535));
    CHECK(!output.open("127.0.0.1", 0));
    CHECK(!output.open("not-an-address", 4242));
}
