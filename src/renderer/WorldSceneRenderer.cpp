#include "renderer/WorldSceneRenderer.hpp"

#include "renderer/VulkanContext.hpp"

#include <imgui.h>

#include <algorithm>

namespace Meridian {

bool WorldSceneRenderer::init(VulkanContext& context)
{
    m_context = &context;
    return true;
}

void WorldSceneRenderer::shutdown()
{
    m_context = nullptr;
}

void WorldSceneRenderer::configureFrame(RenderFrameConfig& config)
{
    if (m_renderStateStore != nullptr) {
        m_renderStateSnapshot = m_renderStateStore->snapshot();
    }

    const float residentBlend = residentChunkBlend();
    config.clearColor = {
        0.05F + 0.06F * residentBlend,
        0.08F + 0.10F * residentBlend,
        0.12F + 0.14F * residentBlend,
        1.0F,
    };
}

void WorldSceneRenderer::beginFrame()
{
    if (m_context == nullptr || m_renderStateStore == nullptr) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(360.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("World Scene");
    ImGui::TextUnformatted("Scene module");
    ImGui::Separator();
    ImGui::Text("Resident chunks: %llu",
        static_cast<unsigned long long>(m_renderStateSnapshot.world.residentChunkCount));
    ImGui::Text("Generating chunks: %llu",
        static_cast<unsigned long long>(m_renderStateSnapshot.world.inFlightChunkCount));
    ImGui::Text("Queued chunks: %llu",
        static_cast<unsigned long long>(m_renderStateSnapshot.world.pendingChunkCount));
    ImGui::Spacing();
    ImGui::TextWrapped(
        "This module now consumes a render-state snapshot, which is the place to grow world-facing data like camera state, lighting state, and GPU residency without coupling scene rendering to the simulation thread.");
    ImGui::Text("Present mode: %s", m_context->getPresentModeName());
    ImGui::Spacing();

    float renderDistanceChunks = m_renderStateSnapshot.worldRenderSettings.renderDistanceChunks;
    if (ImGui::SliderFloat("Render Distance (chunks)", &renderDistanceChunks, 1.0F, 32.0F, "%.1f")) {
        m_renderStateStore->setWorldRenderDistanceChunks(renderDistanceChunks);
        m_renderStateSnapshot.worldRenderSettings.renderDistanceChunks = renderDistanceChunks;
    }

    ImGui::Text(
        "Approx render radius: %.0f m",
        m_renderStateSnapshot.worldRenderSettings.renderDistanceChunks * 32.0F);
    ImGui::TextWrapped(
        "This distance now drives both path-traced chunk visibility and world chunk generation around the camera. Higher values will stream and keep more terrain resident.");
    ImGui::End();
}

float WorldSceneRenderer::residentChunkBlend() const noexcept
{
    constexpr float expectedBootstrapChunks = 27.0F;
    const float residentChunks = static_cast<float>(m_renderStateSnapshot.world.residentChunkCount);
    return std::clamp(residentChunks / expectedBootstrapChunks, 0.0F, 1.0F);
}

} // namespace Meridian
