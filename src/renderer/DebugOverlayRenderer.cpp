#include "renderer/DebugOverlayRenderer.hpp"
#include "renderer/VulkanContext.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace Meridian {

namespace {

[[nodiscard]] std::array<float, 3> normalizeDirection(std::array<float, 3> direction) noexcept
{
    const float lengthSquared =
        direction[0] * direction[0] +
        direction[1] * direction[1] +
        direction[2] * direction[2];
    if (lengthSquared <= 0.0001F) {
        return {0.0F, 1.0F, 0.0F};
    }

    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {
        direction[0] * inverseLength,
        direction[1] * inverseLength,
        direction[2] * inverseLength,
    };
}

void sanitizeLightingState(LightingRenderSnapshot& lighting) noexcept
{
    lighting.sun.direction = normalizeDirection(lighting.sun.direction);
    lighting.sun.intensity = std::max(0.0F, lighting.sun.intensity);

    if (lighting.pointLights.size() > kMaxPointLights) {
        lighting.pointLights.resize(kMaxPointLights);
    }
    for (PointLightRenderState& light : lighting.pointLights) {
        light.intensity = std::max(0.0F, light.intensity);
        light.rangeMeters = std::max(0.1F, light.rangeMeters);
    }

    if (lighting.areaLights.size() > kMaxAreaLights) {
        lighting.areaLights.resize(kMaxAreaLights);
    }
    for (AreaLightRenderState& light : lighting.areaLights) {
        light.intensity = std::max(0.0F, light.intensity);
    }
}

[[nodiscard]] PointLightRenderState makePointLight(const CameraRenderState& camera) noexcept
{
    return PointLightRenderState{
        .positionMeters = {
            camera.position[0] + camera.forward[0] * 8.0F,
            camera.position[1] + camera.forward[1] * 8.0F,
            camera.position[2] + camera.forward[2] * 8.0F,
        },
        .color = {1.0F, 0.85F, 0.65F},
        .intensity = 1800.0F,
        .rangeMeters = 24.0F,
    };
}

[[nodiscard]] AreaLightRenderState makeAreaLight(const CameraRenderState& camera) noexcept
{
    return AreaLightRenderState{
        .centerMeters = {
            camera.position[0] + camera.forward[0] * 10.0F,
            camera.position[1] + camera.forward[1] * 10.0F + 2.0F,
            camera.position[2] + camera.forward[2] * 10.0F,
        },
        .rightExtentMeters = {4.0F, 0.0F, 0.0F},
        .upExtentMeters = {0.0F, 0.0F, 2.5F},
        .color = {1.0F, 0.96F, 0.88F},
        .intensity = 6.0F,
        .doubleSided = false,
    };
}

} // namespace

bool DebugOverlayRenderer::init(VulkanContext& context)
{
    m_context = &context;
    return true;
}

void DebugOverlayRenderer::shutdown()
{
    m_context = nullptr;
}

void DebugOverlayRenderer::beginFrame()
{
    if (m_context == nullptr || m_renderStateStore == nullptr) {
        return;
    }

    m_renderStateSnapshot = m_renderStateStore->snapshot();
    buildFrameStatsWindow();
}

void DebugOverlayRenderer::buildFrameStatsWindow()
{
    if (m_context == nullptr || m_renderStateStore == nullptr) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(540.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Runtime Stats");
    ImGui::Text(
        "Update delta: %.3f ms (%.1f UPS)",
        m_renderStateSnapshot.timing.updateDeltaMilliseconds,
        m_renderStateSnapshot.timing.updatesPerSecond);
    ImGui::Text("Update CPU: %.3f ms", m_renderStateSnapshot.timing.updateCpuMilliseconds);
    ImGui::Text(
        "Render delta: %.3f ms (%.1f FPS)",
        m_renderStateSnapshot.timing.renderDeltaMilliseconds,
        m_renderStateSnapshot.timing.framesPerSecond);
    ImGui::Text("Render CPU: %.3f ms", m_renderStateSnapshot.timing.renderCpuMilliseconds);
    const VkExtent2D renderTargetExtent = m_context->getSwapchainExtent();
    ImGui::Text(
        "Render target: %u x %u",
        renderTargetExtent.width,
        renderTargetExtent.height);
    ImGui::Text(
        "Camera: (%.1f, %.1f, %.1f)",
        m_renderStateSnapshot.camera.position[0],
        m_renderStateSnapshot.camera.position[1],
        m_renderStateSnapshot.camera.position[2]);

    if (m_getUpdateRateLimit && m_setUpdateRateLimit) {
        float updateRateLimit = m_getUpdateRateLimit();
        if (ImGui::SliderFloat("UPS Limit", &updateRateLimit, 30.0F, 1000.0F, "%.0f")) {
            m_setUpdateRateLimit(updateRateLimit);
        }
    }

    bool vsyncEnabled = m_context->isVSyncEnabled();
    if (ImGui::Checkbox("VSync", &vsyncEnabled)) {
        m_context->setVSyncEnabled(vsyncEnabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", m_context->getPresentModeName());

    if (m_context->supportsFragmentShadingRate()) {
        const std::span<const std::uint32_t> supportedRates =
            m_context->supportedFragmentShadingRates();
        if (!supportedRates.empty()) {
            std::size_t selectedRateIndex = 0;
            const auto selectedRate = std::find(
                supportedRates.begin(),
                supportedRates.end(),
                m_context->fragmentShadingRateTexelSize());
            if (selectedRate != supportedRates.end()) {
                selectedRateIndex = static_cast<std::size_t>(
                    std::distance(supportedRates.begin(), selectedRate));
            }

            int shadingRateIndex = static_cast<int>(selectedRateIndex);
            if (ImGui::SliderInt(
                    "Fragment Shading Rate",
                    &shadingRateIndex,
                    0,
                    static_cast<int>(supportedRates.size()) - 1,
                    "%d")) {
                m_context->setFragmentShadingRateTexelSize(
                    supportedRates[static_cast<std::size_t>(shadingRateIndex)]);
            }
            ImGui::SameLine();
            ImGui::Text(
                "%ux%u",
                supportedRates[static_cast<std::size_t>(shadingRateIndex)],
                supportedRates[static_cast<std::size_t>(shadingRateIndex)]);
        }
    } else {
        ImGui::TextDisabled("Fragment Shading Rate: unsupported");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Detailed profiling is emitted to Tracy.");

    if (m_pathTracerSettings != nullptr) {
        ImGui::Separator();
        ImGui::TextUnformatted("Path Tracer");

        int maxBounces = m_pathTracerSettings->maxBounces;
        if (ImGui::SliderInt("Bounces", &maxBounces, 1, 8)) {
            m_pathTracerSettings->maxBounces = maxBounces;
            m_pathTracerSettings->clamp();
        }

        int samplesPerPixel = m_pathTracerSettings->samplesPerPixel;
        if (ImGui::SliderInt("Samples / pixel", &samplesPerPixel, 1, 16)) {
            m_pathTracerSettings->samplesPerPixel = samplesPerPixel;
            m_pathTracerSettings->clamp();
        }

        int maxDdaSteps = m_pathTracerSettings->maxDdaSteps;
        if (ImGui::SliderInt(
            "Max DDA Steps",
            &maxDdaSteps,
            16,
            4096,
            "%d",
            ImGuiSliderFlags_Logarithmic)) {
            m_pathTracerSettings->maxDdaSteps = maxDdaSteps;
            m_pathTracerSettings->clamp();
        }

        if (ImGui::Button(
                m_pathTracerSettings->denoiserEnabled
                    ? "Denoiser: On"
                    : "Denoiser: Off")) {
            m_pathTracerSettings->denoiserEnabled = !m_pathTracerSettings->denoiserEnabled;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s",
            m_pathTracerSettings->denoiserEnabled
                ? "Filtering active"
                : "Showing raw path traced output");

        int denoiserAtrousIterations = m_pathTracerSettings->denoiserAtrousIterations;
        if (ImGui::SliderInt("SVGF Iterations", &denoiserAtrousIterations, 0, 6)) {
            m_pathTracerSettings->denoiserAtrousIterations = denoiserAtrousIterations;
            m_pathTracerSettings->clamp();
        }

        float denoiserTemporalResponse = m_pathTracerSettings->denoiserTemporalResponse;
        if (ImGui::SliderFloat(
                "SVGF Temporal Response",
                &denoiserTemporalResponse,
                0.01F,
                1.0F,
                "%.2f")) {
            m_pathTracerSettings->denoiserTemporalResponse = denoiserTemporalResponse;
            m_pathTracerSettings->clamp();
        }

        float denoiserColorPhi = m_pathTracerSettings->denoiserColorPhi;
        if (ImGui::SliderFloat("SVGF Color Phi", &denoiserColorPhi, 0.1F, 24.0F, "%.2f")) {
            m_pathTracerSettings->denoiserColorPhi = denoiserColorPhi;
            m_pathTracerSettings->clamp();
        }

        float denoiserNormalPhi = m_pathTracerSettings->denoiserNormalPhi;
        if (ImGui::SliderFloat("SVGF Normal Phi", &denoiserNormalPhi, 1.0F, 64.0F, "%.1f")) {
            m_pathTracerSettings->denoiserNormalPhi = denoiserNormalPhi;
            m_pathTracerSettings->clamp();
        }

        float denoiserDepthPhi = m_pathTracerSettings->denoiserDepthPhi;
        if (ImGui::SliderFloat("SVGF Depth Phi", &denoiserDepthPhi, 0.05F, 8.0F, "%.2f")) {
            m_pathTracerSettings->denoiserDepthPhi = denoiserDepthPhi;
            m_pathTracerSettings->clamp();
        }

        float denoiserDifferenceGain = m_pathTracerSettings->denoiserDifferenceGain;
        if (ImGui::SliderFloat(
                "SVGF Difference Gain",
                &denoiserDifferenceGain,
                1.0F,
                32.0F,
                "%.1f")) {
            m_pathTracerSettings->denoiserDifferenceGain = denoiserDifferenceGain;
            m_pathTracerSettings->clamp();
        }

        static constexpr const char* kDenoiserDebugViewLabels[] = {
            "Filtered",
            "Raw",
            "Difference",
            "Split Screen",
        };
        int denoiserDebugView = m_pathTracerSettings->denoiserDebugView;
        if (ImGui::SliderInt(
                "Denoise Debug View",
                &denoiserDebugView,
                PathTracerSettings::kDenoiserDebugViewFiltered,
                PathTracerSettings::kDenoiserDebugViewSplitScreen,
                kDenoiserDebugViewLabels[denoiserDebugView])) {
            m_pathTracerSettings->denoiserDebugView = denoiserDebugView;
            m_pathTracerSettings->clamp();
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button(
                    m_pathTracerSettings->atmosphereEnabled
                        ? "Atmosphere: On"
                        : "Atmosphere: Off")) {
                m_pathTracerSettings->atmosphereEnabled = !m_pathTracerSettings->atmosphereEnabled;
                m_pathTracerSettings->clamp();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Single scattering on sky and primary aerial perspective");

            int atmosphereViewSampleCount = m_pathTracerSettings->atmosphereViewSampleCount;
            if (ImGui::SliderInt("Atmosphere View Steps", &atmosphereViewSampleCount, 4, 64)) {
                m_pathTracerSettings->atmosphereViewSampleCount = atmosphereViewSampleCount;
                m_pathTracerSettings->clamp();
            }

            int atmosphereLightSampleCount = m_pathTracerSettings->atmosphereLightSampleCount;
            if (ImGui::SliderInt("Atmosphere Light Steps", &atmosphereLightSampleCount, 1, 16)) {
                m_pathTracerSettings->atmosphereLightSampleCount = atmosphereLightSampleCount;
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Earth Radius (m)",
                    &m_pathTracerSettings->atmosphereEarthRadiusMeters,
                    100000.0F,
                    10000000.0F,
                    "%.0f",
                    ImGuiSliderFlags_Logarithmic)) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Atmosphere Thickness (m)",
                    &m_pathTracerSettings->atmosphereThicknessMeters,
                    1000.0F,
                    200000.0F,
                    "%.0f",
                    ImGuiSliderFlags_Logarithmic)) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Rayleigh Scale (m)",
                    &m_pathTracerSettings->atmosphereRayleighScaleMeters,
                    100.0F,
                    100000.0F,
                    "%.0f",
                    ImGuiSliderFlags_Logarithmic)) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Mie Scale (m)",
                    &m_pathTracerSettings->atmosphereMieScaleMeters,
                    100.0F,
                    20000.0F,
                    "%.0f",
                    ImGuiSliderFlags_Logarithmic)) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Rayleigh Coefficient",
                    &m_pathTracerSettings->atmosphereRayleighCoefficient,
                    0.0F,
                    8.0F,
                    "%.2f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Mie Coefficient",
                    &m_pathTracerSettings->atmosphereMieCoefficient,
                    0.0F,
                    8.0F,
                    "%.2f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Ozone Coefficient",
                    &m_pathTracerSettings->atmosphereOzoneCoefficient,
                    0.0F,
                    8.0F,
                    "%.2f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::DragFloat3(
                    "Rayleigh Beta",
                    m_pathTracerSettings->atmosphereBetaRayleigh.data(),
                    1.0e-7F,
                    0.0F,
                    0.0001F,
                    "%.7f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::DragFloat3(
                    "Mie Beta",
                    m_pathTracerSettings->atmosphereBetaMie.data(),
                    1.0e-7F,
                    0.0F,
                    0.0001F,
                    "%.7f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::DragFloat3(
                    "Ozone Beta",
                    m_pathTracerSettings->atmosphereBetaOzone.data(),
                    1.0e-7F,
                    0.0F,
                    0.0001F,
                    "%.7f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Density Scale",
                    &m_pathTracerSettings->atmosphereDensityScale,
                    0.0F,
                    8.0F,
                    "%.2f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Light Exposure",
                    &m_pathTracerSettings->atmosphereLightExposure,
                    0.0F,
                    64.0F,
                    "%.2f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Mie Anisotropy",
                    &m_pathTracerSettings->atmosphereMieAnisotropy,
                    0.0F,
                    0.9381F,
                    "%.3f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Ozone Center Altitude (m)",
                    &m_pathTracerSettings->atmosphereOzoneCenterAltitudeMeters,
                    0.0F,
                    60000.0F,
                    "%.0f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Ozone Half Width (m)",
                    &m_pathTracerSettings->atmosphereOzoneHalfWidthMeters,
                    1000.0F,
                    30000.0F,
                    "%.0f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Sun Disc Angular Size",
                    &m_pathTracerSettings->atmosphereSunDiscAngularSize,
                    0.001F,
                    0.2F,
                    "%.4f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Sun Disc Softness",
                    &m_pathTracerSettings->atmosphereSunDiscSoftness,
                    0.0F,
                    1.0F,
                    "%.2f")) {
                m_pathTracerSettings->clamp();
            }

            if (ImGui::SliderFloat(
                    "Sun Disc Brightness",
                    &m_pathTracerSettings->atmosphereSunDiscBrightness,
                    0.0F,
                    64.0F,
                    "%.2f")) {
                m_pathTracerSettings->clamp();
            }
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Camera Controls");
        ImGui::TextUnformatted("Move: W A S D");
        ImGui::TextUnformatted("Vertical: Space / Left Ctrl");
        ImGui::TextUnformatted("Look: Hold Right Mouse / Arrow Keys");
        ImGui::TextUnformatted("Boost: Left Shift");
    }

    buildLightingControls();

    if (m_getTerrainSettings && m_requestTerrainSettings) {
        ImGui::Separator();
        ImGui::TextUnformatted("Terrain Erosion");

        TerrainHeightmapSettings terrainSettings = m_getTerrainSettings();
        bool changed = false;

        int octaveCount = static_cast<int>(terrainSettings.octaveCount);
        changed |= ImGui::SliderInt("Terrain Octaves", &octaveCount, 1, 8);
        terrainSettings.octaveCount = static_cast<std::uint32_t>(octaveCount);

        int heightOctaveCount = static_cast<int>(terrainSettings.heightOctaveCount);
        changed |= ImGui::SliderInt("Height Octaves", &heightOctaveCount, 1, 8);
        terrainSettings.heightOctaveCount = static_cast<std::uint32_t>(heightOctaveCount);

        changed |= ImGui::SliderFloat(
            "World Scale",
            &terrainSettings.baseFrequency,
            0.001F,
            0.05F,
            "%.4f");
        changed |= ImGui::SliderFloat(
            "Height Frequency",
            &terrainSettings.heightFrequency,
            0.25F,
            8.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Height Amplitude",
            &terrainSettings.heightAmplitude,
            0.01F,
            0.35F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Height Lacunarity",
            &terrainSettings.heightLacunarity,
            1.1F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Height Gain",
            &terrainSettings.heightGain,
            0.01F,
            0.8F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Erosion Scale",
            &terrainSettings.erosionFrequency,
            0.01F,
            0.5F,
            "%.4f");
        changed |= ImGui::SliderFloat(
            "Erosion Strength",
            &terrainSettings.erosionStrength,
            0.0F,
            1.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Gully Weight",
            &terrainSettings.gullyWeight,
            0.0F,
            1.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Erosion Gain",
            &terrainSettings.octaveGain,
            0.1F,
            0.95F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Erosion Lacunarity",
            &terrainSettings.lacunarity,
            1.1F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Cell Scale",
            &terrainSettings.cellSizeMultiplier,
            0.25F,
            2.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Assumed Slope",
            &terrainSettings.slopeScale,
            0.05F,
            2.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Assumed Slope Blend",
            &terrainSettings.straightSteeringStrength,
            0.0F,
            1.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Erosion Detail",
            &terrainSettings.stackedDetail,
            0.25F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Normalization",
            &terrainSettings.normalizationFactor,
            0.0F,
            1.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Ridge Rounding",
            &terrainSettings.ridgeRounding,
            0.0F,
            2.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Crease Rounding",
            &terrainSettings.creaseRounding,
            0.0F,
            2.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Input Rounding Mult",
            &terrainSettings.inputRoundingMultiplier,
            0.0F,
            2.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Octave Rounding Mult",
            &terrainSettings.octaveRoundingMultiplier,
            0.25F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Onset Initial",
            &terrainSettings.onsetInitial,
            0.1F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Onset Octave",
            &terrainSettings.onsetOctave,
            0.1F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "RidgeMap Onset Initial",
            &terrainSettings.ridgeMapOnsetInitial,
            0.1F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "RidgeMap Onset Octave",
            &terrainSettings.ridgeMapOnsetOctave,
            0.1F,
            4.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Height Offset Base",
            &terrainSettings.heightOffsetBase,
            -1.0F,
            1.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Height Offset Fade",
            &terrainSettings.heightOffsetFadeInfluence,
            0.0F,
            1.0F,
            "%.3f");
        changed |= ImGui::SliderFloat(
            "Min Height",
            &terrainSettings.minWorldHeight,
            -128.0F,
            0.0F,
            "%.1f");
        changed |= ImGui::SliderFloat(
            "Max Height",
            &terrainSettings.maxWorldHeight,
            1.0F,
            192.0F,
            "%.1f");

        if (changed) {
            terrainSettings.clamp();
            m_requestTerrainSettings(terrainSettings);
        }
    }

    ImGui::End();
}

void DebugOverlayRenderer::buildLightingControls()
{
    if (m_renderStateStore == nullptr) {
        return;
    }

    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    LightingRenderSnapshot lighting = m_renderStateSnapshot.lighting;
    bool lightingChanged = false;

    if (ImGui::TreeNode("Sun")) {
        lightingChanged |= ImGui::DragFloat3(
            "Direction",
            lighting.sun.direction.data(),
            0.01F,
            -1.0F,
            1.0F,
            "%.2f");
        lightingChanged |= ImGui::ColorEdit3("Color", lighting.sun.color.data());
        lightingChanged |= ImGui::SliderFloat(
            "Intensity",
            &lighting.sun.intensity,
            0.0F,
            16.0F,
            "%.2f");
        ImGui::TreePop();
    }

    ImGui::Spacing();
    ImGui::Text("Point Lights (%zu / %zu)", lighting.pointLights.size(), kMaxPointLights);
    if (lighting.pointLights.size() < kMaxPointLights && ImGui::Button("Add Point Light")) {
        lighting.pointLights.push_back(makePointLight(m_renderStateSnapshot.camera));
        lightingChanged = true;
    }

    for (std::size_t index = 0; index < lighting.pointLights.size();) {
        bool removedLight = false;
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::TreeNode("Point Light")) {
            PointLightRenderState& light = lighting.pointLights[index];
            ImGui::Text("Index %zu", index);
            lightingChanged |= ImGui::DragFloat3(
                "Position (m)",
                light.positionMeters.data(),
                0.25F,
                -2048.0F,
                2048.0F,
                "%.1f");
            lightingChanged |= ImGui::ColorEdit3("Color", light.color.data());
            lightingChanged |= ImGui::SliderFloat(
                "Intensity",
                &light.intensity,
                1.0F,
                10000.0F,
                "%.1f",
                ImGuiSliderFlags_Logarithmic);
            lightingChanged |= ImGui::SliderFloat(
                "Range (m)",
                &light.rangeMeters,
                0.5F,
                128.0F,
                "%.1f");
            if (ImGui::Button("Remove")) {
                lighting.pointLights.erase(lighting.pointLights.begin() + static_cast<std::ptrdiff_t>(index));
                lightingChanged = true;
                removedLight = true;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();

        if (!removedLight) {
            ++index;
        }
    }

    ImGui::Spacing();
    ImGui::Text("Area Lights (%zu / %zu)", lighting.areaLights.size(), kMaxAreaLights);
    if (lighting.areaLights.size() < kMaxAreaLights && ImGui::Button("Add Area Light")) {
        lighting.areaLights.push_back(makeAreaLight(m_renderStateSnapshot.camera));
        lightingChanged = true;
    }

    for (std::size_t index = 0; index < lighting.areaLights.size();) {
        bool removedLight = false;
        ImGui::PushID(static_cast<int>(index + kMaxPointLights));
        if (ImGui::TreeNode("Area Light")) {
            AreaLightRenderState& light = lighting.areaLights[index];
            ImGui::Text("Index %zu", index);
            lightingChanged |= ImGui::DragFloat3(
                "Center (m)",
                light.centerMeters.data(),
                0.25F,
                -2048.0F,
                2048.0F,
                "%.1f");
            lightingChanged |= ImGui::DragFloat3(
                "Right Extent (m)",
                light.rightExtentMeters.data(),
                0.1F,
                -64.0F,
                64.0F,
                "%.2f");
            lightingChanged |= ImGui::DragFloat3(
                "Up Extent (m)",
                light.upExtentMeters.data(),
                0.1F,
                -64.0F,
                64.0F,
                "%.2f");
            lightingChanged |= ImGui::ColorEdit3("Color", light.color.data());
            lightingChanged |= ImGui::SliderFloat(
                "Intensity",
                &light.intensity,
                0.0F,
                24.0F,
                "%.2f");
            lightingChanged |= ImGui::Checkbox("Double Sided", &light.doubleSided);
            if (ImGui::Button("Remove")) {
                lighting.areaLights.erase(lighting.areaLights.begin() + static_cast<std::ptrdiff_t>(index));
                lightingChanged = true;
                removedLight = true;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();

        if (!removedLight) {
            ++index;
        }
    }

    if (!lightingChanged) {
        return;
    }

    sanitizeLightingState(lighting);
    m_renderStateStore->updateLightingState(lighting);
    m_renderStateSnapshot.lighting = std::move(lighting);
}
} // namespace Meridian
