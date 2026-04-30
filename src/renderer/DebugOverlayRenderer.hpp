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

    void setUpdateRateLimitCallbacks(
        std::function<float()> getUpdateRateLimit,
        std::function<void(float)> setUpdateRateLimit)
    {
        m_getUpdateRateLimit = std::move(getUpdateRateLimit);
        m_setUpdateRateLimit = std::move(setUpdateRateLimit);
    }

    void setTerrainSettingsCallbacks(
        std::function<TerrainHeightmapSettings()> getTerrainSettings,
        std::function<void(const TerrainHeightmapSettings&)> requestTerrainSettings)
    {
        m_getTerrainSettings = std::move(getTerrainSettings);
        m_requestTerrainSettings = std::move(requestTerrainSettings);
    }

    void setTerrainHeightmapTilesCallback(
        std::function<std::vector<std::shared_ptr<const TerrainHeightmapTile>>()> getTerrainHeightmapTiles)
    {
        m_getTerrainHeightmapTiles = std::move(getTerrainHeightmapTiles);
    }

    bool init(VulkanContext& context) override;
    void shutdown() override;
    void beginFrame() override;

private:
    void buildRenderingWindow();
    void buildTerrainWindow();
    void buildLightingWindow();
    void buildLightingControls();
    void buildTerrainHeightmapViewer();

    VulkanContext* m_context{nullptr};
    PathTracerSettings* m_pathTracerSettings{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    RenderStateSnapshot m_renderStateSnapshot;
    std::function<float()> m_getUpdateRateLimit;
    std::function<void(float)> m_setUpdateRateLimit;
    std::function<TerrainHeightmapSettings()> m_getTerrainSettings;
    std::function<void(const TerrainHeightmapSettings&)> m_requestTerrainSettings;
    std::function<std::vector<std::shared_ptr<const TerrainHeightmapTile>>()> m_getTerrainHeightmapTiles;
    int m_terrainHeightmapPreviewChannel{0};
};

} // namespace Meridian
