#pragma once

#include <algorithm>

namespace Meridian {

struct PathTracerSettings {
    static constexpr int kDenoiserDebugViewFiltered = 0;
    static constexpr int kDenoiserDebugViewRaw = 1;
    static constexpr int kDenoiserDebugViewDifference = 2;
    static constexpr int kDenoiserDebugViewSplitScreen = 3;

    int maxBounces{4};
    int samplesPerPixel{4};
    int maxDdaSteps{64};
    bool denoiserEnabled{true};
    float denoiserKernelStep{3.0F};
    float denoiserColorPhi{1.0F};
    float denoiserNormalPhi{0.5F};
    float denoiserDifferenceGain{8.0F};
    int denoiserDebugView{kDenoiserDebugViewFiltered};

    void clamp() noexcept
    {
        maxBounces = std::clamp(maxBounces, 1, 8);
        samplesPerPixel = std::clamp(samplesPerPixel, 1, 16);
        maxDdaSteps = std::clamp(maxDdaSteps, 16, 4096);
        denoiserKernelStep = std::clamp(denoiserKernelStep, 0.5F, 8.0F);
        denoiserColorPhi = std::clamp(denoiserColorPhi, 0.01F, 8.0F);
        denoiserNormalPhi = std::clamp(denoiserNormalPhi, 0.01F, 8.0F);
        denoiserDifferenceGain = std::clamp(denoiserDifferenceGain, 1.0F, 32.0F);
        denoiserDebugView = std::clamp(denoiserDebugView, 0, 3);
    }
};

} // namespace Meridian