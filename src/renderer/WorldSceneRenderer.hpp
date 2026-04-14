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
    struct LightGizmoDragState {
        bool active{false};
        bool pointLight{true};
        std::size_t lightIndex{0};
        int axisIndex{0};
        std::array<float, 3> startPosition{};
        float startAxisParameter{0.0F};
    };

    void drawChunkWireframeOverlay();
    void drawLightGizmoOverlay();
    void clearActiveLightDrag() noexcept;
    [[nodiscard]] float residentChunkBlend() const noexcept;

    VulkanContext* m_context{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    RenderStateSnapshot m_renderStateSnapshot;
    bool m_chunkWireframeEnabled{false};
    LightGizmoDragState m_activeLightDrag;
};

} // namespace Meridian
