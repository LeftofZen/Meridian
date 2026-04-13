#pragma once

#include <algorithm>

namespace Meridian {

struct PathTracerSettings {
    int maxBounces{4};
    int samplesPerPixel{1};

    void clamp() noexcept
    {
        maxBounces = std::clamp(maxBounces, 1, 8);
        samplesPerPixel = std::clamp(samplesPerPixel, 1, 16);
    }
};

} // namespace Meridian