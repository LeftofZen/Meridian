#pragma once

#include <algorithm>
#include <array>

namespace Meridian {

struct PathTracerSettings {
    static constexpr int kDenoiserDebugViewFiltered = 0;
    static constexpr int kDenoiserDebugViewRaw = 1;
    static constexpr int kDenoiserDebugViewDifference = 2;
    static constexpr int kDenoiserDebugViewSplitScreen = 3;

    int maxBounces{2};
    int samplesPerPixel{2};
    int maxDdaSteps{64};
    bool denoiserEnabled{true};
    int denoiserAtrousIterations{4};
    float denoiserTemporalResponse{0.10F};
    float denoiserColorPhi{6.0F};
    float denoiserNormalPhi{24.0F};
    float denoiserDepthPhi{1.0F};
    float denoiserDifferenceGain{8.0F};
    int denoiserDebugView{kDenoiserDebugViewFiltered};
    bool atmosphereEnabled{true};
    int atmosphereViewSampleCount{32};
    int atmosphereLightSampleCount{4};
    float atmosphereEarthRadiusMeters{6371000.0F};
    float atmosphereThicknessMeters{100000.0F};
    float atmosphereRayleighScaleMeters{8000.0F};
    float atmosphereMieScaleMeters{1200.0F};
    float atmosphereRayleighCoefficient{1.0F};
    float atmosphereMieCoefficient{1.0F};
    float atmosphereOzoneCoefficient{1.0F};
    std::array<float, 3> atmosphereBetaRayleigh{5.802e-6F, 13.558e-6F, 33.100e-6F};
    std::array<float, 3> atmosphereBetaMie{3.996e-6F, 3.996e-6F, 3.996e-6F};
    std::array<float, 3> atmosphereBetaOzone{0.650e-6F, 1.881e-6F, 0.085e-6F};
    float atmosphereDensityScale{1.0F};
    float atmosphereLightExposure{10.0F};
    float atmosphereMieAnisotropy{0.85F};
    float atmosphereOzoneCenterAltitudeMeters{25000.0F};
    float atmosphereOzoneHalfWidthMeters{15000.0F};
    float atmosphereSunDiscAngularSize{0.02F};
    float atmosphereSunDiscSoftness{0.1F};
    float atmosphereSunDiscBrightness{10.0F};

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
        atmosphereViewSampleCount = std::clamp(atmosphereViewSampleCount, 4, 64);
        atmosphereLightSampleCount = std::clamp(atmosphereLightSampleCount, 1, 16);
        atmosphereEarthRadiusMeters = std::clamp(atmosphereEarthRadiusMeters, 100000.0F, 10000000.0F);
        atmosphereThicknessMeters = std::clamp(atmosphereThicknessMeters, 1000.0F, 200000.0F);
        atmosphereRayleighScaleMeters = std::clamp(atmosphereRayleighScaleMeters, 100.0F, 100000.0F);
        atmosphereMieScaleMeters = std::clamp(atmosphereMieScaleMeters, 100.0F, 20000.0F);
        atmosphereRayleighCoefficient = std::clamp(atmosphereRayleighCoefficient, 0.0F, 8.0F);
        atmosphereMieCoefficient = std::clamp(atmosphereMieCoefficient, 0.0F, 8.0F);
        atmosphereOzoneCoefficient = std::clamp(atmosphereOzoneCoefficient, 0.0F, 8.0F);
        for (float& component : atmosphereBetaRayleigh) {
            component = std::clamp(component, 0.0F, 0.0001F);
        }
        for (float& component : atmosphereBetaMie) {
            component = std::clamp(component, 0.0F, 0.0001F);
        }
        for (float& component : atmosphereBetaOzone) {
            component = std::clamp(component, 0.0F, 0.0001F);
        }
        atmosphereDensityScale = std::clamp(atmosphereDensityScale, 0.0F, 8.0F);
        atmosphereLightExposure = std::clamp(atmosphereLightExposure, 0.0F, 64.0F);
        atmosphereMieAnisotropy = std::clamp(atmosphereMieAnisotropy, 0.0F, 0.9381F);
        atmosphereOzoneCenterAltitudeMeters =
            std::clamp(atmosphereOzoneCenterAltitudeMeters, 0.0F, 60000.0F);
        atmosphereOzoneHalfWidthMeters =
            std::clamp(atmosphereOzoneHalfWidthMeters, 1000.0F, 30000.0F);
        atmosphereSunDiscAngularSize = std::clamp(atmosphereSunDiscAngularSize, 0.001F, 0.2F);
        atmosphereSunDiscSoftness = std::clamp(atmosphereSunDiscSoftness, 0.0F, 1.0F);
        atmosphereSunDiscBrightness = std::clamp(atmosphereSunDiscBrightness, 0.0F, 64.0F);
    }
};

} // namespace Meridian