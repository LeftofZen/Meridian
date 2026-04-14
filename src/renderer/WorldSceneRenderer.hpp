#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/RenderStateStore.hpp"

namespace Meridian {

class VulkanContext;

class WorldSceneRenderer final : public IRenderFeature {
public:
    WorldSceneRenderer() = default;

    [[nodiscard]] const char* name() const noexcept override { return "World Scene"; }

    void setRenderStateStore(RenderStateStore& renderStateStore) noexcept
    {
        m_renderStateStore = &renderStateStore;
    }

    bool init(VulkanContext& context) override;
    void shutdown() override;
    void handleEvent(const SDL_Event& event) override;
    void configureFrame(RenderFrameConfig& config) override;
    void beginFrame() override;

private:
    void drawChunkWireframeOverlay();
    [[nodiscard]] float residentChunkBlend() const noexcept;

    VulkanContext* m_context{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    RenderStateSnapshot m_renderStateSnapshot;
    bool m_chunkWireframeEnabled{false};
};

} // namespace Meridian
