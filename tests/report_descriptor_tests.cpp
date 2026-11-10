// report_descriptor_tests.cpp
// Raw HID report-descriptor parsing: the Sony ULT WEAR descriptor captured from
// sysfs, hand-written generic shapes, and rejection of malformed input.
#include "test_framework.hpp"

#include "sony_head_tracker/hid_descriptor.hpp"
#include "sony_head_tracker/hid_report_descriptor.hpp"
#include "sony_head_tracker/macos_support.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace sony;

namespace {

constexpr std::array<std::uint8_t, 171> kUltWear{
    0x05, 0x20, 0x09, 0xe1, 0xa1, 0x01, 0x85, 0x02, 0x0a, 0x08, 0x03, 0x15, 0x00, 0x25, 0xff, 0x75, 0x08,
    0x95, 0x17, 0xb1, 0x03, 0x0a, 0x02, 0x03, 0x15, 0x00, 0x25, 0xff, 0x75, 0x08, 0x95, 0x10, 0xb1, 0x03,
    0x85, 0x01, 0x0a, 0x16, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0xa1, 0x02, 0x0a, 0x40,
    0x08, 0x0a, 0x41, 0x08, 0xb1, 0x00, 0xc0, 0x0a, 0x19, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95,
    0x01, 0xa1, 0x02, 0x0a, 0x55, 0x08, 0x0a, 0x51, 0x08, 0xb1, 0x00, 0xc0, 0x0a, 0x0e, 0x03, 0x15, 0x00,
    0x25, 0x3f, 0x35, 0x0a, 0x45, 0x64, 0x75, 0x06, 0x95, 0x01, 0x66, 0x01, 0x10, 0x55, 0x0d, 0xb1, 0x02,
    0x0a, 0x44, 0x05, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x37, 0x60, 0x4f, 0x46, 0xed, 0x47, 0xa1, 0xb0,
    0xb9, 0x12, 0x55, 0x08, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02, 0x0a, 0x45, 0x05, 0x16, 0x01, 0x80, 0x26,
    0xff, 0x7f, 0x35, 0xe0, 0x45, 0x20, 0x55, 0x00, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02, 0x0a, 0x46, 0x05,
    0x15, 0x00, 0x25, 0xff, 0x35, 0x00, 0x45, 0x00, 0x55, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xc0,
    0x00,
};

const DescriptorField* find(const std::vector<DescriptorField>& fields, std::uint16_t usage, bool feature) {
    for (const auto& f : fields) {
        if (f.usage == usage && f.feature == feature) return &f;
    }
    return nullptr;
}

ParsedReportDescriptor parseUltWear() {
    const auto parsed = parseReportDescriptor(kUltWear);
    CHECK(parsed.has_value());
    return parsed.value_or(ParsedReportDescriptor{});
}

} // namespace

TEST(ult_wear_top_level_and_report_sizes) {
    const auto parsed = parseUltWear();
    CHECK(parsed.usagePage == 0x20);
    CHECK(parsed.usage == 0xE1);
    CHECK(parsed.inputReportBytes == 14);
    CHECK(parsed.featureReportBytes == 40);
}

TEST(ult_wear_feature_report_2_description_fields) {
    const auto parsed = parseUltWear();
    const auto* description = find(parsed.fields, 0x0308, true);
    CHECK(description != nullptr);
    if (!description) return;
    CHECK(description->usagePage == 0x20);
    CHECK(description->constant);
    CHECK(!description->isArray);
    CHECK(description->reportId == 2);
    CHECK(description->reportCount == 23);
    CHECK(description->bitSize == 8);
    CHECK(description->bitOffset == 0);
    CHECK(description->logicalMin == 0);
    CHECK(description->logicalMax == 255);

    const auto* second = find(parsed.fields, 0x0302, true);
    CHECK(second != nullptr);
    if (!second) return;
    CHECK(second->reportId == 2);
    CHECK(second->bitOffset == 184);
    CHECK(second->reportCount == 16);
}

TEST(ult_wear_selectors_carry_array_index_and_offset) {
    const auto parsed = parseUltWear();
    struct Expected { std::uint16_t usage; std::uint16_t index; std::uint32_t offset; };
    constexpr std::array<Expected, 4> expected{{
        {0x0840, 0, 0}, {0x0841, 1, 0}, {0x0855, 0, 1}, {0x0851, 1, 1},
    }};
    for (const auto& e : expected) {
        const auto* f = find(parsed.fields, e.usage, true);
        CHECK(f != nullptr);
        if (!f) continue;
        CHECK(f->isArray);
        CHECK(!f->constant);
        CHECK(f->arrayIndex == e.index);
        CHECK(f->reportId == 1);
        CHECK(f->bitOffset == e.offset);
        CHECK(f->bitSize == 1);
        CHECK(f->reportCount == 1);
        CHECK(f->logicalMin == 0);
        CHECK(f->logicalMax == 1);
    }
}

TEST(ult_wear_collection_usages_do_not_leak_into_fields) {
    const auto parsed = parseUltWear();
    CHECK(find(parsed.fields, 0x0316, true) == nullptr);
    CHECK(find(parsed.fields, 0x0319, true) == nullptr);
    CHECK(parsed.fields.size() == 2 + 4 + 1 + 3);
}

TEST(ult_wear_report_interval_field) {
    const auto parsed = parseUltWear();
    const auto* f = find(parsed.fields, 0x030E, true);
    CHECK(f != nullptr);
    if (!f) return;
    CHECK(f->reportId == 1);
    CHECK(f->bitOffset == 2);
    CHECK(f->bitSize == 6);
    CHECK(f->reportCount == 1);
    CHECK(!f->isArray);
    CHECK(f->logicalMin == 0);
    CHECK(f->logicalMax == 63);
    CHECK(f->physicalMin == 10);
    CHECK(f->physicalMax == 100);
    CHECK(f->unitExponent == -3);
    CHECK(f->unit == 0x1001);
}

TEST(ult_wear_input_fields) {
    const auto parsed = parseUltWear();

    const auto* rotation = find(parsed.fields, 0x0544, false);
    CHECK(rotation != nullptr);
    if (rotation) {
        CHECK(rotation->reportId == 1);
        CHECK(rotation->bitOffset == 0);
        CHECK(rotation->bitSize == 16);
        CHECK(rotation->reportCount == 3);
        CHECK(rotation->logicalMin == -32767);
        CHECK(rotation->logicalMax == 32767);
        CHECK(rotation->physicalMin == -314159264);
        CHECK(rotation->physicalMax == 314159265);
        CHECK(rotation->unitExponent == -8);
        CHECK(rotation->unit == 0x1001);
    }

    const auto* gyro = find(parsed.fields, 0x0545, false);
    CHECK(gyro != nullptr);
    if (gyro) {
        CHECK(gyro->bitOffset == 48);
        CHECK(gyro->bitSize == 16);
        CHECK(gyro->reportCount == 3);
        CHECK(gyro->physicalMin == -32);
        CHECK(gyro->physicalMax == 32);
        CHECK(gyro->unitExponent == 0);
        CHECK(gyro->unit == 0x1001);
    }

    const auto* reset = find(parsed.fields, 0x0546, false);
    CHECK(reset != nullptr);
    if (reset) {
        CHECK(reset->bitOffset == 96);
        CHECK(reset->bitSize == 8);
        CHECK(reset->reportCount == 1);
        CHECK(reset->logicalMin == 0);
        CHECK(reset->logicalMax == 255);
        CHECK(reset->physicalMin == 0);
        CHECK(reset->physicalMax == 0);
    }
}

TEST(ult_wear_fields_feed_existing_helpers) {
    DeviceInfo device;
    device.usagePage = 0x20;
    device.usage = 0xE1;
    device.androidHeadTracker = true;
    const auto parsed = parseUltWear();
    device.fields = parsed.fields;
    CHECK(canConfigureFeatureReports(device));

    const auto* interval = find(parsed.fields, 0x030E, true);
    CHECK(interval != nullptr);
    if (!interval) return;
    const auto choice = chooseReportInterval(*interval);
    CHECK(choice.has_value());
    if (choice) CHECK(choice->raw == 1);
}

TEST(truncated_ult_wear_is_rejected_and_trailing_zero_tolerated) {
    for (std::size_t length = 0; length < 170; ++length) {
        CHECK(!parseReportDescriptor(std::span<const std::uint8_t>(kUltWear).first(length)).has_value());
    }
    CHECK(parseReportDescriptor(std::span<const std::uint8_t>(kUltWear).first(170)).has_value());
    CHECK(parseReportDescriptor(kUltWear).has_value());
}

namespace {

using Bytes = std::vector<std::uint8_t>;

// Application collection around `body`.
Bytes wrapped(const Bytes& body) {
    Bytes bytes{0x05, 0x20, 0x09, 0xe1, 0xa1, 0x01};
    bytes.insert(bytes.end(), body.begin(), body.end());
    bytes.push_back(0xc0);
    return bytes;
}

bool rejected(const Bytes& bytes) { return !parseReportDescriptor(bytes).has_value(); }

} // namespace

TEST(malformed_descriptors_are_rejected) {
    CHECK(rejected({}));
    auto padded = wrapped({});
    padded.resize(4096, 0x00);
    CHECK(!rejected(padded));
    padded.push_back(0x00);
    CHECK(rejected(padded));
    CHECK(rejected(wrapped({0xfe, 0x01, 0x01, 0x00})));
    CHECK(rejected(wrapped({0x85, 0x00})));
    CHECK(rejected(wrapped({0xc0})));
    CHECK(rejected(wrapped({0xb4})));
    CHECK(rejected({0x05, 0x20, 0x09, 0xe1}));
}

TEST(kernel_limits_are_enforced_at_the_boundary) {
    const Bytes input{0x09, 0x01, 0x81, 0x02};
    auto with = [&](Bytes globals) {
        globals.insert(globals.end(), input.begin(), input.end());
        return wrapped(globals);
    };
    CHECK(rejected(with({0x96, 0x01, 0x30})));
    CHECK(!rejected(with({0x96, 0x00, 0x30})));
    CHECK(rejected(with({0x76, 0x01, 0x01})));
    CHECK(!rejected(with({0x76, 0x00, 0x01})));
    CHECK(rejected(with({0x85, 0x01, 0x96, 0x00, 0x30, 0x77, 0x00, 0x01, 0x00, 0x00})));

    Bytes deepPush;
    for (int i = 0; i < 16; ++i) deepPush.push_back(0xa4);
    CHECK(!rejected(wrapped(deepPush)));
    deepPush.push_back(0xa4);
    CHECK(rejected(wrapped(deepPush)));

    CHECK(!rejected(wrapped({0x1a, 0x00, 0x00, 0x2a, 0xff, 0x2f, 0x81, 0x00})));
    CHECK(rejected(wrapped({0x1a, 0x00, 0x00, 0x2a, 0x00, 0x30, 0x81, 0x00})));
}

TEST(variable_item_with_several_usages_splits_into_single_fields) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 3);
    for (std::size_t i = 0; i < parsed->fields.size(); ++i) {
        CHECK(parsed->fields[i].usage == 0x30 + i);
        CHECK(parsed->fields[i].reportCount == 1);
        CHECK(parsed->fields[i].bitOffset == 16 * i);
        CHECK(!parsed->fields[i].isArray);
    }
}

TEST(variable_item_with_fewer_usages_than_values_repeats_last_usage) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x09, 0x30, 0x09, 0x31, 0x75, 0x08, 0x95, 0x03, 0x81, 0x02}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 3);
    CHECK(parsed->fields[2].usage == 0x31);
    CHECK(parsed->fields[2].bitOffset == 16);
}

TEST(usage_range_array_assigns_indices_in_order) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x19, 0x10, 0x29, 0x13, 0x75, 0x04, 0x95, 0x02, 0x81, 0x00}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 4);
    for (std::size_t i = 0; i < parsed->fields.size(); ++i) {
        CHECK(parsed->fields[i].usage == 0x10 + i);
        CHECK(parsed->fields[i].isArray);
        CHECK(parsed->fields[i].arrayIndex == i);
        CHECK(parsed->fields[i].bitOffset == 0);
        CHECK(parsed->fields[i].bitSize == 4);
        CHECK(parsed->fields[i].reportCount == 2);
    }
}

TEST(four_byte_usage_carries_its_own_page) {
    const Bytes bytes{0x0b, 0xe1, 0x00, 0x20, 0x00, 0xa1, 0x01, 0xc0};
    const auto parsed = parseReportDescriptor(bytes);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->usagePage == 0x20);
    CHECK(parsed->usage == 0xE1);
}

TEST(push_and_pop_restore_globals) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x75, 0x08, 0xa4, 0x75, 0x10, 0xb4, 0x09, 0x30, 0x95, 0x01, 0x81, 0x02}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 1);
    CHECK(parsed->fields[0].bitSize == 8);
}

TEST(padding_advances_the_next_field_offset) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x75, 0x03, 0x95, 0x01, 0x81, 0x03, 0x09, 0x30, 0x75, 0x08, 0x81, 0x02}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 1);
    CHECK(parsed->fields[0].bitOffset == 3);
    CHECK(parsed->inputReportBytes == 2);
}

TEST(input_and_feature_counters_are_independent_per_report_id) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x85, 0x01, 0x75, 0x08, 0x95, 0x01,
        0x09, 0x30, 0x81, 0x02,
        0x09, 0x31, 0xb1, 0x02,
        0x09, 0x32, 0x81, 0x02}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 3);
    CHECK(parsed->fields[0].bitOffset == 0);
    CHECK(parsed->fields[1].bitOffset == 0);
    CHECK(parsed->fields[1].feature);
    CHECK(parsed->fields[2].bitOffset == 8);
    CHECK(parsed->inputReportBytes == 3);
    CHECK(parsed->featureReportBytes == 2);
}

TEST(output_items_only_advance_the_output_counter) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x85, 0x01, 0x75, 0x08, 0x95, 0x04, 0x09, 0x30, 0x91, 0x02,
        0x95, 0x01, 0x09, 0x31, 0x81, 0x02}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 1);
    CHECK(parsed->fields[0].bitOffset == 0);
}

TEST(logical_maximum_signedness_follows_the_minimum) {
    const auto parsed = parseReportDescriptor(wrapped({
        0x15, 0x00, 0x25, 0xff, 0x09, 0x30, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
        0x15, 0x80, 0x25, 0x7f, 0x09, 0x31, 0x81, 0x02}));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->fields.size() == 2);
    CHECK(parsed->fields[0].logicalMin == 0);
    CHECK(parsed->fields[0].logicalMax == 255);
    CHECK(parsed->fields[1].logicalMin == -128);
    CHECK(parsed->fields[1].logicalMax == 127);
}
