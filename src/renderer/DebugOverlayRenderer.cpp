#include "renderer/DebugOverlayRenderer.hpp"
#include "renderer/VulkanContext.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>

namespace Meridian {

namespace {

enum class TerrainHeightmapPreviewChannel : int {
    Height = 0,
    RidgeMap = 1,
    Erosion = 2,
    TreeCoverage = 3,
};

struct TerrainChannelRange {
    float minimum{0.0F};
    float maximum{1.0F};
    bool hasSamples{false};
};

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

[[nodiscard]] ImU32 makeTerrainPreviewColor(float normalizedHeight) noexcept
{
    const float value = std::clamp(normalizedHeight, 0.0F, 1.0F);
    const std::uint8_t channel = static_cast<std::uint8_t>(value * 255.0F);
    return IM_COL32(channel, channel, channel, 255);
}

[[nodiscard]] TerrainHeightmapPreviewChannel terrainPreviewChannelFromIndex(int index) noexcept
{
    switch (index) {
    case 1:
        return TerrainHeightmapPreviewChannel::RidgeMap;
    case 2:
        return TerrainHeightmapPreviewChannel::Erosion;
    case 3:
        return TerrainHeightmapPreviewChannel::TreeCoverage;
    default:
        return TerrainHeightmapPreviewChannel::Height;
    }
}

[[nodiscard]] const char* terrainPreviewChannelLabel(
    TerrainHeightmapPreviewChannel channel) noexcept
{
    switch (channel) {
    case TerrainHeightmapPreviewChannel::Height:
        return "Height";
    case TerrainHeightmapPreviewChannel::RidgeMap:
        return "Ridge";
    case TerrainHeightmapPreviewChannel::Erosion:
        return "Erosion";
    case TerrainHeightmapPreviewChannel::TreeCoverage:
        return "Tree Coverage";
    }

    return "Height";
}

[[nodiscard]] const char* terrainPreviewChannelUnits(
    TerrainHeightmapPreviewChannel channel) noexcept
{
    return channel == TerrainHeightmapPreviewChannel::Height ? "m" : "normalized";
}

[[nodiscard]] const std::vector<float>* terrainPreviewSamples(
    const TerrainHeightmapTile& tile,
    TerrainHeightmapPreviewChannel channel) noexcept
{
    switch (channel) {
    case TerrainHeightmapPreviewChannel::Height:
        return &tile.grayscale;
    case TerrainHeightmapPreviewChannel::RidgeMap:
        return &tile.ridgeMap;
    case TerrainHeightmapPreviewChannel::Erosion:
        return &tile.erosion;
    case TerrainHeightmapPreviewChannel::TreeCoverage:
        return &tile.treeCoverage;
    }

    return &tile.grayscale;
}

[[nodiscard]] ImU32 makeTerrainPreviewColor(
    float normalizedValue,
    TerrainHeightmapPreviewChannel channel) noexcept
{
    const float value = std::clamp(normalizedValue, 0.0F, 1.0F);

    switch (channel) {
    case TerrainHeightmapPreviewChannel::Height: {
        const std::uint8_t grayscale = static_cast<std::uint8_t>(value * 255.0F);
        return IM_COL32(grayscale, grayscale, grayscale, 255);
    }
    case TerrainHeightmapPreviewChannel::RidgeMap:
        return IM_COL32(
            static_cast<std::uint8_t>(48.0F + value * 207.0F),
            static_cast<std::uint8_t>(24.0F + value * 168.0F),
            static_cast<std::uint8_t>(16.0F + value * 96.0F),
            255);
    case TerrainHeightmapPreviewChannel::Erosion:
        return IM_COL32(
            static_cast<std::uint8_t>(16.0F + value * 64.0F),
            static_cast<std::uint8_t>(48.0F + value * 144.0F),
            static_cast<std::uint8_t>(88.0F + value * 167.0F),
            255);
    case TerrainHeightmapPreviewChannel::TreeCoverage:
        return IM_COL32(
            static_cast<std::uint8_t>(18.0F + value * 66.0F),
            static_cast<std::uint8_t>(36.0F + value * 186.0F),
            static_cast<std::uint8_t>(18.0F + value * 66.0F),
            255);
    }

    return makeTerrainPreviewColor(value);
}

[[nodiscard]] TerrainChannelRange computeTerrainChannelRange(
    const std::vector<std::shared_ptr<const TerrainHeightmapTile>>& tiles,
    TerrainHeightmapPreviewChannel channel) noexcept
{
    TerrainChannelRange range{};

    for (const std::shared_ptr<const TerrainHeightmapTile>& tile : tiles) {
        if (!tile) {
            continue;
        }

        const std::vector<float>* samples = terrainPreviewSamples(*tile, channel);
        if (samples == nullptr || samples->empty()) {
            continue;
        }

        const auto [minimumIt, maximumIt] = std::minmax_element(samples->begin(), samples->end());
        if (!range.hasSamples) {
            range.minimum = *minimumIt;
            range.maximum = *maximumIt;
            range.hasSamples = true;
            continue;
        }

        range.minimum = std::min(range.minimum, *minimumIt);
        range.maximum = std::max(range.maximum, *maximumIt);
    }

    if (!range.hasSamples) {
        range.minimum = 0.0F;
        range.maximum = 1.0F;
    }

    return range;
}

void drawTerrainMosaic(
    const std::vector<std::shared_ptr<const TerrainHeightmapTile>>& tiles,
    TerrainHeightmapPreviewChannel channel,
    const TerrainChannelRange& range,
    const CameraRenderState& camera)
{
    constexpr float kViewerHeight = 420.0F;
    constexpr float kCanvasPadding = 12.0F;
    constexpr float kPreferredChunkExtent = 96.0F;
    constexpr float kMinimumChunkExtent = 24.0F;
    constexpr float kCameraMarkerRadius = 5.0F;

    int minimumChunkX = std::numeric_limits<int>::max();
    int maximumChunkX = std::numeric_limits<int>::min();
    int minimumChunkZ = std::numeric_limits<int>::max();
    int maximumChunkZ = std::numeric_limits<int>::min();
    bool hasValidTile = false;

    for (const std::shared_ptr<const TerrainHeightmapTile>& tile : tiles) {
        if (!tile || tile->resolution == 0) {
            continue;
        }

        const std::vector<float>* samples = terrainPreviewSamples(*tile, channel);
        if (samples == nullptr || samples->size() !=
                                      static_cast<std::size_t>(tile->resolution) * tile->resolution) {
            continue;
        }

        minimumChunkX = std::min(minimumChunkX, tile->coord.x);
        maximumChunkX = std::max(maximumChunkX, tile->coord.x);
        minimumChunkZ = std::min(minimumChunkZ, tile->coord.z);
        maximumChunkZ = std::max(maximumChunkZ, tile->coord.z);
        hasValidTile = true;
    }

    if (!hasValidTile) {
        ImGui::TextDisabled("No cached %s samples are available.", terrainPreviewChannelLabel(channel));
        return;
    }

    ImGui::BeginChild(
        "TerrainHeightmapViewer",
        ImVec2(0.0F, kViewerHeight),
        true,
        ImGuiWindowFlags_HorizontalScrollbar);

    const int chunkSpanX = maximumChunkX - minimumChunkX + 1;
    const int chunkSpanZ = maximumChunkZ - minimumChunkZ + 1;
    const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
    const float chunkExtent = std::clamp(
        (availableWidth - kCanvasPadding * 2.0F) / static_cast<float>(chunkSpanX),
        kMinimumChunkExtent,
        kPreferredChunkExtent);
    const ImVec2 canvasSize(
        kCanvasPadding * 2.0F + chunkExtent * static_cast<float>(chunkSpanX),
        kCanvasPadding * 2.0F + chunkExtent * static_cast<float>(chunkSpanZ));

    ImGui::InvisibleButton("##terrain_heightmap_mosaic", canvasSize);
    const ImVec2 canvasMin = ImGui::GetItemRectMin();
    const ImVec2 canvasMax = ImGui::GetItemRectMax();
    const ImVec2 mosaicMin(canvasMin.x + kCanvasPadding, canvasMin.y + kCanvasPadding);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(14, 16, 20, 255));

    for (int gridX = 0; gridX <= chunkSpanX; ++gridX) {
        const float x = mosaicMin.x + static_cast<float>(gridX) * chunkExtent;
        drawList->AddLine(
            ImVec2(x, mosaicMin.y),
            ImVec2(x, mosaicMin.y + chunkExtent * static_cast<float>(chunkSpanZ)),
            IM_COL32(255, 255, 255, 24));
    }
    for (int gridZ = 0; gridZ <= chunkSpanZ; ++gridZ) {
        const float y = mosaicMin.y + static_cast<float>(gridZ) * chunkExtent;
        drawList->AddLine(
            ImVec2(mosaicMin.x, y),
            ImVec2(mosaicMin.x + chunkExtent * static_cast<float>(chunkSpanX), y),
            IM_COL32(255, 255, 255, 24));
    }

    const float valueRange = std::max(range.maximum - range.minimum, 0.0001F);
    for (const std::shared_ptr<const TerrainHeightmapTile>& tile : tiles) {
        if (!tile || tile->resolution == 0) {
            continue;
        }

        const std::vector<float>* samples = terrainPreviewSamples(*tile, channel);
        if (samples == nullptr || samples->size() !=
                                      static_cast<std::size_t>(tile->resolution) * tile->resolution) {
            continue;
        }

        const float tileOriginX =
            mosaicMin.x + static_cast<float>(tile->coord.x - minimumChunkX) * chunkExtent;
        const float tileOriginY =
            mosaicMin.y + static_cast<float>(tile->coord.z - minimumChunkZ) * chunkExtent;
        const float sampleWidth = chunkExtent / static_cast<float>(tile->resolution);
        const float sampleHeight = chunkExtent / static_cast<float>(tile->resolution);
        const ImVec2 tileMin(tileOriginX, tileOriginY);
        const ImVec2 tileMax(tileOriginX + chunkExtent, tileOriginY + chunkExtent);

        for (std::uint32_t row = 0; row < tile->resolution; ++row) {
            for (std::uint32_t column = 0; column < tile->resolution; ++column) {
                const std::size_t sampleIndex =
                    static_cast<std::size_t>(row) * tile->resolution + column;
                const float normalizedValue = ((*samples)[sampleIndex] - range.minimum) / valueRange;
                const ImVec2 sampleMin(
                    tileOriginX + static_cast<float>(column) * sampleWidth,
                    tileOriginY + static_cast<float>(row) * sampleHeight);
                const ImVec2 sampleMax(sampleMin.x + sampleWidth, sampleMin.y + sampleHeight);
                drawList->AddRectFilled(
                    sampleMin,
                    sampleMax,
                    makeTerrainPreviewColor(normalizedValue, channel));
            }
        }

        drawList->AddRect(tileMin, tileMax, IM_COL32(255, 255, 255, 72));

        char label[32]{};
        std::snprintf(label, sizeof(label), "(%d, %d)", tile->coord.x, tile->coord.z);
        drawList->AddText(
            ImVec2(tileMin.x + 4.0F, tileMin.y + 4.0F),
            IM_COL32(255, 255, 255, 196),
            label);
    }

    const float cameraChunkX = camera.position[0] / static_cast<float>(kWorldChunkSize);
    const float cameraChunkZ = camera.position[2] / static_cast<float>(kWorldChunkSize);
    const ImVec2 cameraMarker(
        mosaicMin.x + (cameraChunkX - static_cast<float>(minimumChunkX)) * chunkExtent,
        mosaicMin.y + (cameraChunkZ - static_cast<float>(minimumChunkZ)) * chunkExtent);
    const bool cameraInsideMosaic =
        cameraMarker.x >= mosaicMin.x &&
        cameraMarker.x <= mosaicMin.x + chunkExtent * static_cast<float>(chunkSpanX) &&
        cameraMarker.y >= mosaicMin.y &&
        cameraMarker.y <= mosaicMin.y + chunkExtent * static_cast<float>(chunkSpanZ);
    if (cameraInsideMosaic) {
        drawList->AddCircleFilled(cameraMarker, kCameraMarkerRadius, IM_COL32(255, 96, 96, 255));
        drawList->AddCircle(cameraMarker, kCameraMarkerRadius + 2.0F, IM_COL32(255, 255, 255, 192));
        drawList->AddText(
            ImVec2(cameraMarker.x + 8.0F, cameraMarker.y - 18.0F),
            IM_COL32(255, 220, 220, 255),
            "Camera");
    }

    drawList->AddRect(canvasMin, canvasMax, IM_COL32(255, 255, 255, 48));
    ImGui::EndChild();
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
    buildRenderingWindow();
    buildTerrainWindow();
    buildLightingWindow();
}

void DebugOverlayRenderer::buildRenderingWindow()
{
    if (m_context == nullptr || m_renderStateStore == nullptr) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(540.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Rendering & Path Tracing");
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

        float lodFactor = m_pathTracerSettings->lodFactor;
        if (ImGui::SliderFloat(
                "LOD Factor",
                &lodFactor,
                0.0F,
                0.2F,
                "%.4f")) {
            m_pathTracerSettings->lodFactor = lodFactor;
            m_pathTracerSettings->clamp();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Coarsens octree traversal with distance.\n"
                "Distance is measured from the camera to the center of the current\n"
                "SVO node in voxel units.\n"
                "Once a ray reaches occupied world data, the LOD factor limits\n"
                "how many additional child levels get traversed based on that\n"
                "node-to-camera distance. If descending further would exceed the\n"
                "detail budget, the current\n"
                "occupied node is shaded as a coarse hit instead of recursing deeper.\n"
                "0 = off (full resolution).");
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

    ImGui::End();
}

void DebugOverlayRenderer::buildTerrainWindow()
{
    const bool hasTerrainControls = m_getTerrainSettings && m_requestTerrainSettings;
    const bool hasTerrainPreview = static_cast<bool>(m_getTerrainHeightmapTiles);
    if (!hasTerrainControls && !hasTerrainPreview) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Terrain Generation");

    if (hasTerrainControls) {
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
    } else {
        ImGui::TextDisabled("Terrain settings are unavailable.");
    }

    if (hasTerrainPreview) {
        ImGui::Separator();
        buildTerrainHeightmapViewer();
    }

    ImGui::End();
}

void DebugOverlayRenderer::buildLightingWindow()
{
    if (m_renderStateStore == nullptr) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(460.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Lighting");
    buildLightingControls();
    ImGui::End();
}

void DebugOverlayRenderer::buildTerrainHeightmapViewer()
{
    if (!ImGui::CollapsingHeader("Terrain Heightmaps", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    if (!m_getTerrainHeightmapTiles) {
        ImGui::TextDisabled("Heightmap debug feed unavailable.");
        return;
    }

    std::vector<std::shared_ptr<const TerrainHeightmapTile>> tiles = m_getTerrainHeightmapTiles();
    if (tiles.empty()) {
        ImGui::TextDisabled("No cached terrain heightmaps yet.");
        return;
    }

    ImGui::TextDisabled("Showing cached pre-voxelization chunk tiles stitched in chunk-space.");

    int selectedChannelIndex = m_terrainHeightmapPreviewChannel;
    if (ImGui::RadioButton("Height", &selectedChannelIndex, 0)) {
        m_terrainHeightmapPreviewChannel = selectedChannelIndex;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Ridge", &selectedChannelIndex, 1)) {
        m_terrainHeightmapPreviewChannel = selectedChannelIndex;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Erosion", &selectedChannelIndex, 2)) {
        m_terrainHeightmapPreviewChannel = selectedChannelIndex;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Tree Coverage", &selectedChannelIndex, 3)) {
        m_terrainHeightmapPreviewChannel = selectedChannelIndex;
    }

    const TerrainHeightmapPreviewChannel channel =
        terrainPreviewChannelFromIndex(m_terrainHeightmapPreviewChannel);
    const TerrainChannelRange channelRange = computeTerrainChannelRange(tiles, channel);

    drawTerrainMosaic(tiles, channel, channelRange, m_renderStateSnapshot.camera);
    if (channelRange.hasSamples) {
        ImGui::Text(
            "%s range: %.3f .. %.3f %s",
            terrainPreviewChannelLabel(channel),
            channelRange.minimum,
            channelRange.maximum,
            terrainPreviewChannelUnits(channel));
    }
}

void DebugOverlayRenderer::buildLightingControls()
{
    if (m_renderStateStore == nullptr) {
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
