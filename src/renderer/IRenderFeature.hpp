#pragma once

#include "renderer/RenderFrameConfig.hpp"

#include <volk.h>

union SDL_Event;

namespace Meridian {

class VulkanContext;

class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;

    [[nodiscard]] virtual const char* name() const noexcept { return "RenderFeature"; }
    virtual bool init(VulkanContext& /*context*/) { return true; }
    virtual void shutdown() {}
    virtual void handleEvent(const SDL_Event& /*event*/) {}
    virtual void configureFrame(RenderFrameConfig& /*config*/) {}
    virtual void beginFrame() {}
    virtual void recordFrame(VkCommandBuffer /*commandBuffer*/) {}
};

} // namespace Meridian
