// orientation.hpp
// Orientation filter: adaptive smoothing, recenter and axis mapping.
// Operates purely on MotionSample; no Windows, no hardware.
#pragma once

#include "sony_head_tracker/types.hpp"

namespace sony {

class OrientationFilter {
public:
    explicit OrientationFilter(FilterConfig config = {});
    MotionSample process(MotionSample sample);
    void recenter();
    void setConfig(FilterConfig config);

private:
    FilterConfig config_;
    Quaternion filtered_{};
    Quaternion center_{};
    Quaternion latestRaw_{};
    bool initialized_{};
    bool recenterPending_{};
    std::uint8_t resetCounter_{};
};

} // namespace sony
