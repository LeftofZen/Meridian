#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/PathTracerSettings.hpp"
#include "renderer/RenderStateStore.hpp"
#include "world/TerrainHeightmapGenerator.hpp"

#include <functional>

namespace Meridian {

class VulkanContext;

class DebugOverlayRenderer final : public IRenderFeature {
public:
    DebugOverlayRenderer() = default;

    DebugOverlayRenderer(const DebugOverlayRenderer&) = delete;
    DebugOverlayRenderer& operator=(const DebugOverlayRenderer&) = delete;
    DebugOverlayRenderer(DebugOverlayRenderer&&) = delete;
    DebugOverlayRenderer& operator=(DebugOverlayRenderer&&) = delete;

    [[nodiscard]] const char* name() const noexcept override { return "Debug Overlay"; }

    void setRenderStateStore(RenderStateStore& renderStateStore) noexcept
    {
        m_renderStateStore = &renderStateStore;
    }

    void setPathTracerSettings(PathTracerSettings& pathTracerSettings) noexcept
    {
        m_pathTracerSettings = &pathTracerSettings;
    }

    void setTerrainSettingsCallbacks(
        std::function<TerrainHeightmapSettings()> getTerrainSettings,
        std::function<void(const TerrainHeightmapSettings&)> requestTerrainSettings)
    {
        m_getTerrainSettings = std::move(getTerrainSettings);
        m_requestTerrainSettings = std::move(requestTerrainSettings);
    }

    bool init(VulkanContext& context) override;
    void shutdown() override;
    void beginFrame() override;

private:
    void buildFrameStatsWindow();

    VulkanContext* m_context{nullptr};
    PathTracerSettings* m_pathTracerSettings{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    RenderStateSnapshot m_renderStateSnapshot;
    std::function<TerrainHeightmapSettings()> m_getTerrainSettings;
    std::function<void(const TerrainHeightmapSettings&)> m_requestTerrainSettings;
};

} // namespace Meridian
