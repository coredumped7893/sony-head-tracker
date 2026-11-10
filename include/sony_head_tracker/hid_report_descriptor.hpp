// hid_report_descriptor.hpp
// Pure parser for raw HID report-descriptor bytes (what Linux hidraw exposes).
// Produces the same DescriptorField list the Windows/macOS backends get from
// their OS parsers, plus bit offsets and array indices. No platform code.
#pragma once

#include "sony_head_tracker/device.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sony {

struct ParsedReportDescriptor {
    std::uint16_t usagePage{};          // first top-level Application collection
    std::uint16_t usage{};
    std::uint16_t inputReportBytes{};   // largest input report, including the report-ID byte
    std::uint16_t featureReportBytes{}; // largest feature report, including the report-ID byte
    std::vector<DescriptorField> fields; // input + feature fields in descriptor order
};

// Returns nullopt for anything Linux hid-core would reject (see the limits in
// the .cpp) or for a descriptor without a top-level Application collection.
std::optional<ParsedReportDescriptor> parseReportDescriptor(std::span<const std::uint8_t> bytes);

} // namespace sony
