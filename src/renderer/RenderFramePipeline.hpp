#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/IRenderFrontend.hpp"
#include "renderer/RenderStateStore.hpp"

#include <atomic>
#include <mutex>
#include <vector>

namespace Meridian {

class RenderFramePipeline final : public IRenderFrontend {
public:
    RenderFramePipeline() = default;
    ~RenderFramePipeline() override;

    RenderFramePipeline(const RenderFramePipeline&) = delete;
    RenderFramePipeline& operator=(const RenderFramePipeline&) = delete;
    RenderFramePipeline(RenderFramePipeline&&) = delete;
    RenderFramePipeline& operator=(RenderFramePipeline&&) = delete;

    void addFeature(IRenderFeature& feature);
    void setRenderStateStore(RenderStateStore& renderStateStore) noexcept
    {
        m_renderStateStore = &renderStateStore;
    }

    bool init(SDL_Window* window, VulkanContext& context) override;
    void shutdown() override;
    void handleEvent(const SDL_Event& event) override;
    void beginFrame() override;
    void recordFrame(VkCommandBuffer commandBuffer) override;
    [[nodiscard]] const RenderFrameConfig& getFrameConfig() const noexcept override
    {
        return m_frameConfig;
    }

private:
    struct FrameProfilingData {
        float eventProcessingMilliseconds{0.0F};
        float imguiNewFrameMilliseconds{0.0F};
        float configureTotalMilliseconds{0.0F};
        float beginTotalMilliseconds{0.0F};
        float imguiRenderMilliseconds{0.0F};
        float recordTotalMilliseconds{0.0F};
        float imguiDrawMilliseconds{0.0F};
        std::vector<RenderFeatureTimingSample> featureSamples;

        void reset(std::size_t featureCount)
        {
            eventProcessingMilliseconds = 0.0F;
            imguiNewFrameMilliseconds = 0.0F;
            configureTotalMilliseconds = 0.0F;
            beginTotalMilliseconds = 0.0F;
            imguiRenderMilliseconds = 0.0F;
            recordTotalMilliseconds = 0.0F;
            imguiDrawMilliseconds = 0.0F;
            featureSamples.assign(featureCount, RenderFeatureTimingSample{});
        }
    };

    [[nodiscard]] bool createDescriptorPool();
    void destroyDescriptorPool();
    static void checkVkResult(VkResult result);

    VulkanContext* m_context{nullptr};
    SDL_Window* m_windowHandle{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::vector<IRenderFeature*> m_features;
    std::mutex m_eventMutex;
    std::vector<SDL_Event> m_pendingEvents;
    RenderFrameConfig m_frameConfig{};
    FrameProfilingData m_frameProfilingData{};
    std::atomic<bool> m_initialised{false};
};

} // namespace Meridian
