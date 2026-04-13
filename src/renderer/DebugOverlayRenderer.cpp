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

    ImGui::SetNextWindowSize(ImVec2(420.0F, 0.0F), ImGuiCond_FirstUseEver);
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

    bool vsyncEnabled = m_context->isVSyncEnabled();
    if (ImGui::Checkbox("VSync", &vsyncEnabled)) {
        m_context->setVSyncEnabled(vsyncEnabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", m_context->getPresentModeName());
    ImGui::Separator();

    if (ImGui::BeginTable(
            "system-frame-times",
            2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("System");
        ImGui::TableSetupColumn("Update (ms)");
        ImGui::TableHeadersRow();

        for (const SystemFrameStat& frameStat : m_renderStateSnapshot.systemFrameStats) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(frameStat.name.data());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", frameStat.updateTimeMilliseconds);
        }

        ImGui::EndTable();
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
    }

    ImGui::End();
}
} // namespace Meridian
