#include "renderer/DebugOverlayRenderer.hpp"
#include "renderer/VulkanContext.hpp"

#include <imgui.h>

namespace Meridian {

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

    ImGui::SetNextWindowSize(ImVec2(720.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("System Frame Times");
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
    ImGui::Text(
        "Render sync: frame %.3f / acquire %.3f / image %.3f ms",
        m_renderStateSnapshot.timing.renderFrameFenceWaitMilliseconds,
        m_renderStateSnapshot.timing.renderAcquireWaitMilliseconds,
        m_renderStateSnapshot.timing.renderImageFenceWaitMilliseconds);
    ImGui::Text(
        "Render work: frontend %.3f / record %.3f / submit+present %.3f ms",
        m_renderStateSnapshot.timing.renderFrontendMilliseconds,
        m_renderStateSnapshot.timing.renderCommandRecordingMilliseconds,
        m_renderStateSnapshot.timing.renderSubmitPresentMilliseconds);
    ImGui::Text(
        "Camera: (%.1f, %.1f, %.1f)",
        m_renderStateSnapshot.camera.position[0],
        m_renderStateSnapshot.camera.position[1],
        m_renderStateSnapshot.camera.position[2]);

    bool vsyncEnabled = m_context->isVSyncEnabled();
    if (ImGui::Checkbox("VSync", &vsyncEnabled)) {
        m_context->setVSyncEnabled(vsyncEnabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", m_context->getPresentModeName());
    ImGui::Separator();

    if (ImGui::BeginTable(
            "system-frame-times",
            5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("System");
        ImGui::TableSetupColumn("Current (ms)");
        ImGui::TableSetupColumn("Avg (ms)");
        ImGui::TableSetupColumn("Max (ms)");
        ImGui::TableSetupColumn("Update %");
        ImGui::TableHeadersRow();

        for (const SystemFrameStat& frameStat : m_renderStateSnapshot.systemFrameStats) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(frameStat.name.data());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", frameStat.updateTimeMilliseconds);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", frameStat.averageUpdateTimeMilliseconds);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", frameStat.maxUpdateTimeMilliseconds);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.1f", frameStat.frameSharePercent);
        }

        ImGui::EndTable();
    }

    if (!m_renderStateSnapshot.renderPhaseStats.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Render Frontend Phases");
        if (ImGui::BeginTable(
                "render-phase-times",
                5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Phase");
            ImGui::TableSetupColumn("Current (ms)");
            ImGui::TableSetupColumn("Avg (ms)");
            ImGui::TableSetupColumn("Max (ms)");
            ImGui::TableSetupColumn("Frontend %");
            ImGui::TableHeadersRow();

            for (const RenderPhaseFrameStat& phaseStat : m_renderStateSnapshot.renderPhaseStats) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(phaseStat.name.data());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", phaseStat.timeMilliseconds);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", phaseStat.averageTimeMilliseconds);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.3f", phaseStat.maxTimeMilliseconds);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.1f", phaseStat.frameSharePercent);
            }

            ImGui::EndTable();
        }
    }

    if (!m_renderStateSnapshot.renderFeatureStats.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Render Features");
        if (ImGui::BeginTable(
                "render-feature-times",
                8,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Feature");
            ImGui::TableSetupColumn("Config");
            ImGui::TableSetupColumn("Begin");
            ImGui::TableSetupColumn("Record");
            ImGui::TableSetupColumn("Total");
            ImGui::TableSetupColumn("Avg Total");
            ImGui::TableSetupColumn("Max Total");
            ImGui::TableSetupColumn("Frontend %");
            ImGui::TableHeadersRow();

            for (const RenderFeatureFrameStat& featureStat : m_renderStateSnapshot.renderFeatureStats) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(featureStat.name.data());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", featureStat.configureTimeMilliseconds);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", featureStat.beginTimeMilliseconds);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.3f", featureStat.recordTimeMilliseconds);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.3f", featureStat.totalTimeMilliseconds);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.3f", featureStat.averageTotalTimeMilliseconds);
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%.3f", featureStat.maxTotalTimeMilliseconds);
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%.1f", featureStat.frameSharePercent);
            }

            ImGui::EndTable();
        }
    }

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

        ImGui::Spacing();
        ImGui::TextUnformatted("Camera Controls");
        ImGui::TextUnformatted("Move: W A S D");
        ImGui::TextUnformatted("Vertical: Space / Left Ctrl");
        ImGui::TextUnformatted("Look: Hold Right Mouse / Arrow Keys");
        ImGui::TextUnformatted("Boost: Left Shift");
    }

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
} // namespace Meridian
