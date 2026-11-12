// orientation.cpp
// Orientation filter (smoothing, recenter, axis map).
// Pure: operates only on MotionSample and the maths helpers.
#include "sony_head_tracker/orientation.hpp"

#include "sony_head_tracker/math.hpp"

#include <algorithm>
#include <cmath>

namespace sony {

OrientationFilter::OrientationFilter(FilterConfig config) : config_(config) {}
void OrientationFilter::setConfig(FilterConfig config) { config_ = config; }
void OrientationFilter::recenter() { recenterPending_ = true; }

MotionSample OrientationFilter::process(MotionSample sample) {
    if (sample.angularVelocity) sample.angularVelocity = remap(*sample.angularVelocity, config_.axes);
    if (sample.acceleration) sample.acceleration = remap(*sample.acceleration, config_.axes);
    // Missing angular velocity behaves as zero, so an orientation-only device is always smoothed fully.
    const Vec3 angular = sample.angularVelocity.value_or(Vec3{});
    latestRaw_ = rotationVectorToQuaternion(sample.rotationVector);
    if (!initialized_) { filtered_ = latestRaw_; center_ = latestRaw_; resetCounter_ = sample.resetCounter; initialized_ = true; }
    if (sample.resetCounter != resetCounter_) {
        // The device re-zeroed its reference frame, so the old center no longer applies.
        // Re-anchor it so the current relative pose carries over unchanged.
        center_ = multiply(latestRaw_, conjugate(multiply(conjugate(center_), filtered_)));
        filtered_ = latestRaw_;
        resetCounter_ = sample.resetCounter;
    }
    if (recenterPending_) { center_ = latestRaw_; recenterPending_ = false; }
    const auto speed = std::sqrt(angular.x*angular.x + angular.y*angular.y + angular.z*angular.z);
    const auto fast = std::clamp(speed / std::max(0.01, config_.fastMovementRadiansPerSecond), 0.0, 1.0);
    const auto alpha = std::clamp(config_.smoothing + (1.0-config_.smoothing)*fast, 0.001, 1.0);
    filtered_ = slerp(filtered_, latestRaw_, alpha);
    // Center in the device frame and remap only the relative rotation: remapping first would
    // mix axes whenever the head is turned away from the device's reference frame.
    const auto relative = remap(quaternionToRotationVector(multiply(conjugate(center_), filtered_)), config_.axes);
    sample.rotationVector = relative;
    sample.orientation = rotationVectorToQuaternion(relative);
    // A mirrored axis map (odd number of swaps and flips) turns the rotation into its inverse, whose
    // Euler angles mix pitch into roll once the head turns. Read the angles unmirrored, then mirror them.
    const auto& [source, sign] = config_.axes;
    const auto parity = (int(source[1]) - int(source[0])) * (int(source[2]) - int(source[0])) * (int(source[2]) - int(source[1]));
    const auto mirror = parity * sign[0] * sign[1] * sign[2] < 0.0 ? -1.0 : 1.0;
    const auto e = quaternionToEulerDegrees(rotationVectorToQuaternion({relative.x * mirror, relative.y * mirror, relative.z * mirror}));
    sample.euler = {e.yaw * mirror, e.pitch * mirror, e.roll * mirror};
    return sample;
}

} // namespace sony
