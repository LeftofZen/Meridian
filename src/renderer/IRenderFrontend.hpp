#pragma once

#include "renderer/RenderFrameConfig.hpp"

#include <volk.h>

struct SDL_Window;
union SDL_Event;

namespace Meridian {

class VulkanContext;

class IRenderFrontend {
public:
    virtual ~IRenderFrontend() = default;

    virtual bool init(SDL_Window* window, VulkanContext& context) = 0;
    virtual void shutdown() = 0;
    virtual void handleEvent(const SDL_Event& event) = 0;
    virtual void beginFrame() = 0;
    virtual void recordFrame(VkCommandBuffer commandBuffer) = 0;
    [[nodiscard]] virtual const RenderFrameConfig& getFrameConfig() const noexcept = 0;
};

} // namespace Meridian
