#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/RenderStateStore.hpp"

namespace Meridian {

class VulkanContext;

class WorldSceneRenderer final : public IRenderFeature {
public:
    WorldSceneRenderer() = default;

    void setRenderStateStore(RenderStateStore& renderStateStore) noexcept
    {
        m_renderStateStore = &renderStateStore;
    }

    bool init(VulkanContext& context) override;
    void shutdown() override;
    void configureFrame(RenderFrameConfig& config) override;
    void beginFrame() override;

private:
    [[nodiscard]] float residentChunkBlend() const noexcept;

    VulkanContext* m_context{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    RenderStateSnapshot m_renderStateSnapshot;
};

} // namespace Meridian
