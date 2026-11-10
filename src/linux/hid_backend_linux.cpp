// hid_backend_linux.cpp
// Linux hidraw discovery. Candidates are found by descriptor (0x20:0xE1) and the
// sensor-description marker, never by model or VID/PID. Nothing here writes a
// feature report.
#include "sony_head_tracker/hid_backend.hpp"

#include "sony_head_tracker/hid_report_descriptor.hpp"
#include "sony_head_tracker/hid_usages.hpp"
#include "sony_head_tracker/linux_support.hpp"
#include "sony_head_tracker/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sony {

struct HidBackend::Context {
    int fd{-1};
};

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxDescriptorBytes = 4096;

std::vector<std::uint8_t> readBinary(const fs::path& path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(limit);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(limit));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return bytes;
}

std::string readText(const fs::path& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> sensorDescriptionReportIds(const ParsedReportDescriptor& descriptor) {
    std::vector<std::uint8_t> ids;
    for (const auto& field : descriptor.fields) {
        if (!field.feature || field.usagePage != kSensorPage || field.usage != kSensorDescription) continue;
        if (std::find(ids.begin(), ids.end(), field.reportId) == ids.end()) ids.push_back(field.reportId);
    }
    return ids;
}

std::string readMarker(int fd, const ParsedReportDescriptor& descriptor) {
    std::vector<std::uint8_t> report(std::max<std::size_t>(descriptor.featureReportBytes, 2));
    for (const auto reportId : sensorDescriptionReportIds(descriptor)) {
        report.assign(report.size(), 0);
        report[0] = reportId;
        const int length = ioctl(fd, HIDIOCGFEATURE(report.size()), report.data());
        if (length <= 0) continue;
        auto marker = markerFromFeatureReport(
            std::span<const std::uint8_t>(report.data(), static_cast<std::size_t>(length)));
        if (!marker.empty()) return marker;
    }
    return {};
}

} // namespace

HidBackend::HidBackend() : context_(std::make_unique<Context>()) {}

HidBackend::~HidBackend() { disconnect(); }

std::vector<DeviceInfo> HidBackend::enumerate(bool) {
    std::error_code error;
    std::vector<fs::path> nodes;
    for (const auto& entry : fs::directory_iterator("/sys/class/hidraw", error)) nodes.push_back(entry.path());
    std::sort(nodes.begin(), nodes.end());

    std::vector<DeviceInfo> devices;
    for (const auto& node : nodes) {
        const auto name = node.filename().string();
        const auto descriptor = parseReportDescriptor(readBinary(node / "device/report_descriptor", kMaxDescriptorBytes));
        if (!descriptor || descriptor->usagePage != kSensorPage || descriptor->usage != kOtherCustom) continue;
        const auto uevent = parseHidUevent(readText(node / "device/uevent"));
        if (!uevent) continue;
        const auto instance = fs::canonical(node / "device", error).filename().string();

        std::string marker;
        bool accessDenied = false;
        const int fd = open(("/dev/" + name).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            marker = readMarker(fd, *descriptor);
            close(fd);
        } else {
            accessDenied = errno == EACCES || errno == EPERM;
        }

        Logger::instance().write(LogLevel::info, std::format(
            L"hidraw candidate {} usage=0x{:04X}:0x{:04X} marker={} access={}",
            detail::widen(name), descriptor->usagePage, descriptor->usage,
            marker.empty() ? L"no" : L"yes", accessDenied ? L"denied" : L"ok"));
        devices.push_back(describeHidrawDevice(L"/dev/" + detail::widen(name), detail::widen(instance),
                                               *descriptor, *uevent, std::move(marker), accessDenied));
    }
    return devices;
}

bool HidBackend::connect(const DeviceInfo&, RawCallback, SampleCallback) {
    Logger::instance().write(LogLevel::error, L"Linux streaming is not implemented yet.");
    return false;
}

void HidBackend::disconnect() {
    if (context_ && context_->fd >= 0) close(context_->fd);
    if (context_) context_->fd = -1;
}

std::wstring hexDump(const std::vector<std::uint8_t>& bytes) {
    std::wostringstream out;
    out << std::hex << std::uppercase << std::setfill(L'0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index) out << L' ';
        out << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return out.str();
}

} // namespace sony
