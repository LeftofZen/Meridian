#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/PathTracerSettings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace Meridian {

class VulkanContext;

class PathTracerRenderer final : public IRenderFeature {
public:
    PathTracerRenderer() = default;
    ~PathTracerRenderer() override = default;

    PathTracerRenderer(const PathTracerRenderer&) = delete;
    PathTracerRenderer& operator=(const PathTracerRenderer&) = delete;
    PathTracerRenderer(PathTracerRenderer&&) = delete;
    PathTracerRenderer& operator=(PathTracerRenderer&&) = delete;

    [[nodiscard]] PathTracerSettings& settings() noexcept { return m_settings; }
    [[nodiscard]] const PathTracerSettings& settings() const noexcept { return m_settings; }

    bool init(VulkanContext& context) override;
    void shutdown() override;
    void configureFrame(RenderFrameConfig& config) override;
    void beginFrame() override;
    void recordFrame(VkCommandBuffer commandBuffer) override;

private:
    struct PushConstants {
        std::array<float, 2> resolution{};
        std::uint32_t frameIndex{0};
        std::uint32_t samplesPerPixel{1};
        std::uint32_t maxBounces{4};
    };

    [[nodiscard]] bool createPipeline();
    void destroyPipeline() noexcept;
    [[nodiscard]] static std::filesystem::path shaderPath(const char* fileName);

    VulkanContext* m_context{nullptr};
    PathTracerSettings m_settings;
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    std::uint32_t m_frameIndex{0};
};

} // namespace Meridian