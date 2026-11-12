// orientation_tests.cpp
// OrientationFilter behaviour: recenter, smoothing attenuation, axis mapping, and
// graceful handling of samples with no angular velocity, and device frame resets.
#include "test_framework.hpp"

#include "sony_head_tracker/orientation.hpp"

#include <chrono>
#include <cmath>
#include <numbers>

using namespace sony;

static FilterConfig identityConfig(double smoothing) {
    FilterConfig cfg;
    cfg.axes = AxisMapping{{0, 1, 2}, {1.0, 1.0, 1.0}};   // no remap/inversion
    cfg.smoothing = smoothing;
    return cfg;
}

static MotionSample rot(Vec3 v) { MotionSample s; s.rotationVector = v; return s; }

TEST(recenter_zeroes_output_for_the_held_pose) {
    OrientationFilter f(identityConfig(1.0));
    f.process(rot({0, 0, 0}));                     // first sample becomes the center
    const auto moved = f.process(rot({0.3, 0, 0}));
    CHECK(std::fabs(moved.euler.roll) > 1.0);      // rotation about X shows up as roll

    f.recenter();
    const auto after = f.process(rot({0.3, 0, 0}));   // recenter onto the current pose
    CHECK_NEAR(after.euler.yaw, 0.0, 1e-6);
    CHECK_NEAR(after.euler.pitch, 0.0, 1e-6);
    CHECK_NEAR(after.euler.roll, 0.0, 1e-6);
}

TEST(smoothing_attenuates_a_single_step) {
    OrientationFilter fast(identityConfig(1.0));   // follows instantly
    OrientationFilter slow(identityConfig(0.1));   // heavily smoothed
    fast.process(rot({0, 0, 0}));
    slow.process(rot({0, 0, 0}));

    const auto fastOut = fast.process(rot({0, 0, 0.5}));   // rotation about Z -> yaw
    const auto slowOut = slow.process(rot({0, 0, 0.5}));

    CHECK(std::fabs(slowOut.euler.yaw) < std::fabs(fastOut.euler.yaw));
    constexpr double deg = 180.0 / std::numbers::pi;
    CHECK_NEAR(fastOut.euler.yaw, 0.5 * deg, 0.5);         // ~28.6 degrees
}

TEST(axis_remap_applies_to_angular_velocity) {
    OrientationFilter f(identityConfig(1.0));
    MotionSample s = rot({0, 0, 0});
    s.angularVelocity = Vec3{1.0, 2.0, 3.0};
    const auto out = f.process(std::move(s));
    CHECK(out.angularVelocity.has_value());
    CHECK_NEAR(out.angularVelocity->x, 1.0, 1e-9);         // identity mapping preserves it
    CHECK_NEAR(out.angularVelocity->y, 2.0, 1e-9);
    CHECK_NEAR(out.angularVelocity->z, 3.0, 1e-9);
}

TEST(missing_angular_velocity_stays_missing_and_is_stable) {
    OrientationFilter f;   // default config
    const auto out = f.process(rot({0.1, 0.1, 0.1}));       // no angularVelocity set
    CHECK(!out.angularVelocity.has_value());                // the filter never fabricates a gyro
    const auto out2 = f.process(rot({0.1, 0.1, 0.1}));
    CHECK(std::isfinite(out2.euler.yaw));
    CHECK(std::isfinite(out2.euler.pitch));
    CHECK(std::isfinite(out2.euler.roll));
}

TEST(device_reference_reset_keeps_axes_and_pose) {
    OrientationFilter f(identityConfig(1.0));
    MotionSample start = rot({0, 0, 1.2});   // head forward, device frame yawed 1.2 rad
    start.resetCounter = 5;
    f.process(std::move(start));             // becomes the center

    MotionSample reset = rot({0, 0, 0});     // same head pose after the device re-zeroed
    reset.resetCounter = 6;
    const auto held = f.process(std::move(reset));
    CHECK_NEAR(held.euler.yaw, 0.0, 1e-6);
    CHECK_NEAR(held.euler.pitch, 0.0, 1e-6);
    CHECK_NEAR(held.euler.roll, 0.0, 1e-6);

    MotionSample nod = rot({0.3, 0, 0});     // rotation about X only
    nod.resetCounter = 6;
    const auto out = f.process(std::move(nod));
    constexpr double deg = 180.0 / std::numbers::pi;
    CHECK_NEAR(out.euler.roll, 0.3 * deg, 1e-6);
    CHECK_NEAR(out.euler.yaw, 0.0, 1e-6);
    CHECK_NEAR(out.euler.pitch, 0.0, 1e-6);
}

TEST(slow_head_motion_does_not_drift_the_center) {
    FilterConfig cfg;
    cfg.axes = AxisMapping{{0, 1, 2}, {1.0, 1.0, 1.0}};
    OrientationFilter f(cfg);
    const auto start = std::chrono::steady_clock::time_point{};
    double t = 0, yaw = 0;
    MotionSample out;
    const auto step = [&](double rate) {
        yaw += rate / 60.0;
        t += 1.0 / 60.0;
        MotionSample s = rot({0, 0, yaw});
        s.angularVelocity = Vec3{0, 0, rate};
        s.receivedAt = start + std::chrono::microseconds(static_cast<long long>(t * 1e6));
        out = f.process(s);
    };
    step(0.0);   // center at yaw 0
    for (int cycle = 0; cycle < 5; ++cycle) {   // snap right fast, come back slowly
        while (yaw < 0.7) step(2.0);
        while (yaw > 0.0) step(-0.05);
    }
    yaw = 0.0;
    for (int i = 0; i < 600; ++i) step(0.0);   // hold still facing forward for 10 s
    CHECK_NEAR(out.euler.yaw, 0.0, 0.5);
}
