#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/PathTracerSettings.hpp"
#include "renderer/RenderStateStore.hpp"

namespace Meridian {

class VulkanContext;

class DebugOverlayRenderer final : public IRenderFeature {
public:
    DebugOverlayRenderer() = default;

    DebugOverlayRenderer(const DebugOverlayRenderer&) = delete;
    DebugOverlayRenderer& operator=(const DebugOverlayRenderer&) = delete;
    DebugOverlayRenderer(DebugOverlayRenderer&&) = delete;
    DebugOverlayRenderer& operator=(DebugOverlayRenderer&&) = delete;

    void setRenderStateStore(RenderStateStore& renderStateStore) noexcept
    {
        m_renderStateStore = &renderStateStore;
    }

    void setPathTracerSettings(PathTracerSettings& pathTracerSettings) noexcept
    {
        m_pathTracerSettings = &pathTracerSettings;
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
};

} // namespace Meridian
