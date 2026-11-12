// hid_report_descriptor.cpp
// Pure HID report-descriptor parser. No platform code.
#include "sony_head_tracker/hid_report_descriptor.hpp"

#include "sony_head_tracker/hid_descriptor.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace sony {

namespace {

// Linux hid-core limits (HID_MAX_DESCRIPTOR_SIZE, HID_MAX_USAGES, HID_MAX_BUFFER_SIZE).
constexpr std::size_t kMaxDescriptorBytes = 4096;
constexpr std::size_t kMaxUsages = 12288;
constexpr std::uint32_t kMaxReportBytes = 16384;
constexpr std::size_t kMaxGlobalStack = 16;
// Parser's own budget, not a kernel limit: Report Size 0 lets usage ranges expand without growing any report.
constexpr std::size_t kMaxFields = 65536;
constexpr std::uint32_t kMaxReportSizeBits = 256;

enum ReportType : std::size_t { kInput, kOutput, kFeature };

struct Usage {
    std::uint16_t page{};
    std::uint16_t id{};
};

struct Globals {
    std::uint16_t usagePage{};
    std::int32_t logicalMin{};
    std::int32_t logicalMax{};
    std::int32_t physicalMin{};
    std::int32_t physicalMax{};
    std::int8_t unitExponent{};
    std::uint32_t unit{};
    std::uint16_t reportSize{};
    std::uint16_t reportCount{};
    std::uint8_t reportId{};
};

struct Parser {
    ParsedReportDescriptor out;
    Globals globals;
    std::vector<Globals> globalStack;
    std::vector<Usage> usages;
    std::optional<Usage> usageMin;
    std::array<std::array<std::uint32_t, 256>, 3> bits{};
    unsigned depth{};
    bool foundApplication{};

    Usage makeUsage(std::uint32_t value, unsigned size) const {
        if (size == 4) return {static_cast<std::uint16_t>(value >> 16), static_cast<std::uint16_t>(value)};
        return {globals.usagePage, static_cast<std::uint16_t>(value)};
    }

    bool global(unsigned tag, std::uint32_t value, unsigned size) {
        switch (tag) {
        case 0x0: globals.usagePage = static_cast<std::uint16_t>(value); break;
        case 0x1: globals.logicalMin = static_cast<std::int32_t>(hidSigned(value, size)); break;
        case 0x2:
            globals.logicalMax = globals.logicalMin < 0 ? static_cast<std::int32_t>(hidSigned(value, size))
                                                        : static_cast<std::int32_t>(value);
            break;
        case 0x3: globals.physicalMin = static_cast<std::int32_t>(hidSigned(value, size)); break;
        case 0x4:
            globals.physicalMax = globals.physicalMin < 0 ? static_cast<std::int32_t>(hidSigned(value, size))
                                                          : static_cast<std::int32_t>(value);
            break;
        case 0x5: globals.unitExponent = decodeHidUnitExponent(value); break;
        case 0x6: globals.unit = value; break;
        case 0x7:
            if (value > kMaxReportSizeBits) return false;
            globals.reportSize = static_cast<std::uint16_t>(value);
            break;
        case 0x8:
            if (value == 0 || value > 255) return false;
            globals.reportId = static_cast<std::uint8_t>(value);
            break;
        case 0x9:
            if (value > kMaxUsages) return false;
            globals.reportCount = static_cast<std::uint16_t>(value);
            break;
        case 0xA:
            if (globalStack.size() >= kMaxGlobalStack) return false;
            globalStack.push_back(globals);
            break;
        case 0xB:
            if (globalStack.empty()) return false;
            globals = globalStack.back();
            globalStack.pop_back();
            break;
        default: break;
        }
        return true;
    }

    bool addUsage(Usage usage) {
        if (usages.size() >= kMaxUsages) return false;
        usages.push_back(usage);
        return true;
    }

    bool local(unsigned tag, std::uint32_t value, unsigned size) {
        if (tag == 0x0) return addUsage(makeUsage(value, size));
        if (tag == 0x1) usageMin = makeUsage(value, size);
        if (tag == 0x2 && usageMin) {
            for (std::uint32_t id = usageMin->id; id <= static_cast<std::uint16_t>(value); ++id) {
                if (!addUsage({usageMin->page, static_cast<std::uint16_t>(id)})) return false;
            }
        }
        return true;
    }

    DescriptorField baseField(const Usage& usage, bool feature) const {
        DescriptorField f;
        f.usagePage = usage.page;
        f.usage = usage.id;
        f.reportId = globals.reportId;
        f.reportCount = globals.reportCount;
        f.bitSize = globals.reportSize;
        f.logicalMin = globals.logicalMin;
        f.logicalMax = globals.logicalMax;
        f.physicalMin = globals.physicalMin;
        f.physicalMax = globals.physicalMax;
        f.unitExponent = globals.unitExponent;
        f.unit = globals.unit;
        f.feature = feature;
        return f;
    }

    bool data(ReportType type, std::uint32_t flags) {
        auto& counter = bits[type][globals.reportId];
        const auto offset = counter;
        counter += static_cast<std::uint32_t>(globals.reportSize) * globals.reportCount;
        if ((counter + 7) / 8 + (globals.reportId != 0 ? 1u : 0u) > kMaxReportBytes) return false;
        if (type == kOutput || usages.empty()) return true;

        const bool feature = type == kFeature;
        const bool constant = (flags & 0x01) != 0;
        const bool variable = (flags & 0x02) != 0;
        if (!variable) {
            for (std::size_t i = 0; i < usages.size(); ++i) {
                auto f = baseField(usages[i], feature);
                f.bitOffset = offset;
                f.isArray = true;
                f.arrayIndex = static_cast<std::uint16_t>(i);
                f.constant = constant;
                out.fields.push_back(f);
            }
        } else if (usages.size() == 1) {
            auto f = baseField(usages[0], feature);
            f.bitOffset = offset;
            f.constant = constant;
            out.fields.push_back(f);
        } else {
            for (std::uint32_t i = 0; i < globals.reportCount; ++i) {
                auto f = baseField(usages[std::min<std::size_t>(i, usages.size() - 1)], feature);
                f.reportCount = 1;
                f.bitOffset = offset + i * globals.reportSize;
                f.constant = constant;
                out.fields.push_back(f);
            }
        }
        return out.fields.size() <= kMaxFields;
    }

    void collection(std::uint32_t type) {
        if (depth == 0 && type == 0x01 && !foundApplication) {
            foundApplication = true;
            if (!usages.empty()) {
                out.usagePage = usages.front().page;
                out.usage = usages.front().id;
            }
        }
        ++depth;
    }

    bool main(unsigned tag, std::uint32_t value) {
        bool ok = true;
        switch (tag) {
        case 0x8: ok = data(kInput, value); break;
        case 0x9: ok = data(kOutput, value); break;
        case 0xB: ok = data(kFeature, value); break;
        case 0xA: collection(value); break;
        case 0xC:
            if (depth == 0) return false;
            --depth;
            break;
        default: break;
        }
        usages.clear();
        usageMin.reset();
        return ok;
    }

    std::uint16_t largestReport(ReportType type) const {
        std::uint32_t largest = 0;
        for (std::size_t id = 0; id < 256; ++id) {
            if (bits[type][id] == 0) continue;
            largest = std::max(largest, (bits[type][id] + 7) / 8 + (id != 0 ? 1u : 0u));
        }
        return static_cast<std::uint16_t>(largest);
    }

    bool run(std::span<const std::uint8_t> bytes) {
        if (bytes.empty() || bytes.size() > kMaxDescriptorBytes) return false;
        std::size_t pos = 0;
        while (pos < bytes.size()) {
            const auto prefix = bytes[pos++];
            if (prefix == 0xFE) return false;
            const unsigned size = (prefix & 0x03) == 3 ? 4 : (prefix & 0x03);
            if (pos + size > bytes.size()) return false;
            std::uint32_t value = 0;
            for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint32_t>(bytes[pos + i]) << (8 * i);
            pos += size;

            const unsigned type = (prefix >> 2) & 0x03;
            const unsigned tag = prefix >> 4;
            bool ok = true;
            if (type == 0) ok = main(tag, value);
            else if (type == 1) ok = global(tag, value, size);
            else if (type == 2) ok = local(tag, value, size);
            if (!ok) return false;
        }
        if (depth != 0 || !foundApplication) return false;
        out.inputReportBytes = largestReport(kInput);
        out.featureReportBytes = largestReport(kFeature);
        return true;
    }
};

} // namespace

std::optional<ParsedReportDescriptor> parseReportDescriptor(std::span<const std::uint8_t> bytes) {
    Parser parser;
    if (!parser.run(bytes)) return std::nullopt;
    return std::move(parser.out);
}

} // namespace sony
