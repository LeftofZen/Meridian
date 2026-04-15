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
    int maxDdaSteps{192};
    bool denoiserEnabled{true};
    int denoiserAtrousIterations{4};
    float denoiserTemporalResponse{0.15F};
    float denoiserColorPhi{6.0F};
    float denoiserNormalPhi{24.0F};
    float denoiserDepthPhi{1.0F};
    float denoiserDifferenceGain{8.0F};
    int denoiserDebugView{kDenoiserDebugViewFiltered};

    void clamp() noexcept
    {
        maxBounces = std::clamp(maxBounces, 1, 8);
        samplesPerPixel = std::clamp(samplesPerPixel, 1, 16);
        maxDdaSteps = std::clamp(maxDdaSteps, 16, 4096);
        denoiserAtrousIterations = std::clamp(denoiserAtrousIterations, 0, 6);
        denoiserTemporalResponse = std::clamp(denoiserTemporalResponse, 0.01F, 1.0F);
        denoiserColorPhi = std::clamp(denoiserColorPhi, 0.1F, 24.0F);
        denoiserNormalPhi = std::clamp(denoiserNormalPhi, 1.0F, 64.0F);
        denoiserDepthPhi = std::clamp(denoiserDepthPhi, 0.05F, 8.0F);
        denoiserDifferenceGain = std::clamp(denoiserDifferenceGain, 1.0F, 32.0F);
        denoiserDebugView = std::clamp(denoiserDebugView, 0, 3);
    }
};

} // namespace Meridian