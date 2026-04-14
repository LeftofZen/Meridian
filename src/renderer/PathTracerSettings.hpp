#pragma once

#include <algorithm>

namespace Meridian {

struct PathTracerSettings {
    int maxBounces{4};
    int samplesPerPixel{4};
    int maxDdaSteps{64};

    void clamp() noexcept
    {
        maxBounces = std::clamp(maxBounces, 1, 8);
        samplesPerPixel = std::clamp(samplesPerPixel, 1, 16);
        maxDdaSteps = std::clamp(maxDdaSteps, 16, 4096);
    }
};

} // namespace Meridian