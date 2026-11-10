// hid_backend_linux.cpp
// Linux hidraw backend. Candidates are found by descriptor (0x20:0xE1) and the
// sensor-description marker, never by model or VID/PID. Discovery opens nodes
// read-only and never writes; only connect() writes the verified tracker's
// power, reporting and interval feature reports.
#include "sony_head_tracker/hid_backend.hpp"

#include "sony_head_tracker/hid_report_descriptor.hpp"
#include "sony_head_tracker/hid_usages.hpp"
#include "sony_head_tracker/linux_support.hpp"
#include "sony_head_tracker/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sony {

struct HidBackend::Context {
    int fd{-1};
    std::vector<DescriptorField> fields;
    std::size_t inputReportBytes{};
    std::size_t featureReportBytes{};
    HidBackend::RawCallback raw;
    HidBackend::SampleCallback sample;
    std::chrono::steady_clock::time_point rateStart{std::chrono::steady_clock::now()};
    std::uint64_t rateCount{};
    double rate{};
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

std::optional<ParsedReportDescriptor> readDescriptorFromFd(int fd) {
    int size = 0;
    if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size <= 0 || size > HID_MAX_DESCRIPTOR_SIZE) return std::nullopt;
    hidraw_report_descriptor raw{};
    raw.size = static_cast<__u32>(size);
    if (ioctl(fd, HIDIOCGRDESC, &raw) < 0) return std::nullopt;
    return parseReportDescriptor(std::span<const std::uint8_t>(raw.value, raw.size));
}

bool readFeatureReport(int fd, std::uint8_t reportId, std::size_t bufferBytes, std::vector<std::uint8_t>& report) {
    report.assign(bufferBytes, 0);
    report[0] = reportId;
    const int length = ioctl(fd, HIDIOCGFEATURE(report.size()), report.data());
    if (length <= 0) return false;
    report.resize(static_cast<std::size_t>(length));
    return true;
}

bool configureFeatures(HidBackend::Context& context, const std::vector<FeatureAssignment>& plan) {
    std::vector<std::uint8_t> reportIds;
    for (const auto& item : plan) {
        if (std::find(reportIds.begin(), reportIds.end(), item.reportId) == reportIds.end()) reportIds.push_back(item.reportId);
    }
    for (const auto reportId : reportIds) {
        const auto layout = featureReportLayoutFor(context.fields, reportId);
        std::vector<std::uint8_t> before;
        if (!layout || !readFeatureReport(context.fd, reportId, context.featureReportBytes, before) ||
            (layout->hasReportIdPrefix && before.front() != reportId)) {
            Logger::instance().write(LogLevel::error, std::format(L"Could not read feature report {}", reportId));
            return false;
        }
        auto after = before;
        for (const auto& item : plan) {
            if (item.reportId != reportId) continue;
            if (!readFeatureBits(before, *layout, item.bitOffset, item.bitSize) ||
                !writeFeatureBits(after, *layout, item.bitOffset, item.bitSize, item.value)) {
                Logger::instance().write(LogLevel::error, std::format(L"Feature field is outside report {}", reportId));
                return false;
            }
        }
        if (after != before) {
            if (ioctl(context.fd, HIDIOCSFEATURE(after.size()), after.data()) < 0) {
                Logger::instance().write(LogLevel::error, std::format(
                    L"Feature report {} write failed (errno={})", reportId, errno));
                return false;
            }
            Logger::instance().write(LogLevel::info, std::format(
                L"Feature report {} before={} requested={}", reportId, hexDump(before), hexDump(after)));
        }
        std::vector<std::uint8_t> verified;
        if (!readFeatureReport(context.fd, reportId, context.featureReportBytes, verified) ||
            (layout->hasReportIdPrefix && verified.front() != reportId)) {
            Logger::instance().write(LogLevel::error, std::format(L"Could not read back feature report {}", reportId));
            return false;
        }
        for (const auto& item : plan) {
            if (item.reportId != reportId) continue;
            const auto value = readFeatureBits(verified, *layout, item.bitOffset, item.bitSize);
            if (!value || *value != item.value) {
                Logger::instance().write(LogLevel::error, std::format(
                    L"Feature report {} read-back differs from the requested configuration", reportId));
                return false;
            }
        }
        Logger::instance().write(LogLevel::info, std::format(
            L"Feature report {} length={} bytes={}", reportId, verified.size(), hexDump(verified)));
    }
    return true;
}

// EINTR is expected here: SIGUSR1 (recenter) may be delivered to this thread.
bool streamOnce(HidBackend::Context& context, std::vector<std::uint8_t>& buffer) {
    pollfd waiting{context.fd, POLLIN, 0};
    const int ready = poll(&waiting, 1, 100);
    if (ready < 0) return errno == EINTR;
    if (ready == 0) return true;
    if (waiting.revents & (POLLHUP | POLLERR | POLLNVAL)) return false;
    const auto length = read(context.fd, buffer.data(), buffer.size());
    if (length < 0) return errno == EINTR || errno == EAGAIN;
    if (length == 0) return false;
    const std::vector<std::uint8_t> report(buffer.begin(), buffer.begin() + length);
    if (context.raw) context.raw(report);
    auto sample = decodeMotionSample(report, context.fields);
    if (!sample) return true;
    sample->receivedAt = std::chrono::steady_clock::now();
    ++context.rateCount;
    const auto elapsed = std::chrono::duration<double>(sample->receivedAt - context.rateStart).count();
    if (elapsed >= 1.0) {
        context.rate = context.rateCount / elapsed;
        context.rateCount = 0;
        context.rateStart = sample->receivedAt;
    }
    sample->packetsPerSecond = context.rate;
    sample->receiveLatencyMs = -1.0;
    if (context.sample) context.sample(std::move(*sample));
    return true;
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

bool HidBackend::connect(const DeviceInfo& device, RawCallback raw, SampleCallback sample) {
    disconnect();
    readerStop_.reset();
    const std::string path(device.path.begin(), device.path.end());
    context_->fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (context_->fd < 0) {
        Logger::instance().write(LogLevel::error, std::format(L"Could not open {} read-write (errno={})", device.path, errno));
        return false;
    }
    // Trust only what the open fd reports: the hidraw number may have been reused since enumerate().
    const auto descriptor = readDescriptorFromFd(context_->fd);
    if (!descriptor) {
        Logger::instance().write(LogLevel::error, L"Could not read the report descriptor from the opened node");
        disconnect();
        return false;
    }
    HidUevent uevent;
    uevent.vendorId = device.vendorId;
    uevent.productId = device.productId;
    uevent.name = device.bluetoothName;
    uevent.uniq = device.bluetoothAddress;
    const auto verified = describeHidrawDevice(device.path, device.instanceId, *descriptor, uevent,
                                               readMarker(context_->fd, *descriptor), false);
    const auto field = std::find_if(verified.fields.begin(), verified.fields.end(), [](const auto& candidate) {
        return candidate.feature && candidate.usagePage == kSensorPage && candidate.usage == kReportInterval;
    });
    const auto interval = field == verified.fields.end() ? std::nullopt : chooseReportInterval(*field);
    const auto plan = canConfigureFeatureReports(verified) && interval
        ? planFeatureConfiguration(verified.fields, interval->raw) : std::nullopt;
    if (!plan) {
        Logger::instance().write(LogLevel::error, L"The opened node is not a configurable Android head tracker");
        disconnect();
        return false;
    }
    context_->fields = verified.fields;
    context_->inputReportBytes = verified.inputReportBytes;
    context_->featureReportBytes = verified.featureReportBytes;
    context_->raw = std::move(raw);
    context_->sample = std::move(sample);
    if (!configureFeatures(*context_, *plan)) {
        disconnect();
        return false;
    }
    Logger::instance().write(LogLevel::info, L"Linux head tracker feature configuration accepted");
    running_ = true;
    reader_ = std::thread([this, context = context_.get()] {
        std::vector<std::uint8_t> buffer(context->inputReportBytes);
        while (!readerStop_.stopRequested() && streamOnce(*context, buffer)) {}
        running_ = false;
    });
    return true;
}

void HidBackend::disconnect() {
    running_ = false;
    readerStop_.requestStop();
    if (reader_.joinable()) reader_.join();
    if (context_ && context_->fd >= 0) close(context_->fd);
    if (context_) *context_ = Context{};
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
