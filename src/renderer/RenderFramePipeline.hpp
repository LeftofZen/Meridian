#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/IRenderFrontend.hpp"

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
    [[nodiscard]] bool createDescriptorPool();
    void destroyDescriptorPool();
    static void checkVkResult(VkResult result);

    VulkanContext* m_context{nullptr};
    SDL_Window* m_windowHandle{nullptr};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    std::vector<IRenderFeature*> m_features;
    std::mutex m_eventMutex;
    std::vector<SDL_Event> m_pendingEvents;
    RenderFrameConfig m_frameConfig{};
    std::atomic<bool> m_initialised{false};
};

} // namespace Meridian
