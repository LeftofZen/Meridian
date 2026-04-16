#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/PathTracerSettings.hpp"
#include "renderer/RenderStateStore.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

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
    [[nodiscard]] const char* name() const noexcept override { return "Path Tracer"; }

    void setRenderStateStore(RenderStateStore& renderStateStore) noexcept
    {
        m_renderStateStore = &renderStateStore;
    }

    bool init(VulkanContext& context) override;
    void shutdown() override;
    void configureFrame(RenderFrameConfig& config) override;
    void beginFrame() override;
    void recordPreRender(VkCommandBuffer commandBuffer) override;
    void recordFrame(VkCommandBuffer commandBuffer) override;

private:
    struct alignas(16) GpuDirectionalLight {
        std::array<float, 4> directionAndIntensity{};
        std::array<float, 4> color{};
    };

    struct alignas(16) GpuPointLight {
        std::array<float, 4> positionAndRange{};
        std::array<float, 4> colorAndIntensity{};
    };

    struct alignas(16) GpuAreaLight {
        std::array<float, 4> centerAndIntensity{};
        std::array<float, 4> rightExtentAndDoubleSided{};
        std::array<float, 4> upExtent{};
        std::array<float, 4> color{};
    };

    struct alignas(16) GpuLightScene {
        std::array<std::uint32_t, 4> counts{};
        GpuDirectionalLight sun;
        std::array<GpuPointLight, kMaxPointLights> pointLights{};
        std::array<GpuAreaLight, kMaxAreaLights> areaLights{};
    };

    struct alignas(16) GpuAtmosphereParameters {
        std::array<float, 4> planetAndScaleHeights{};
        std::array<float, 4> coefficients{};
        std::array<float, 4> betaRayleigh{};
        std::array<float, 4> betaMie{};
        std::array<float, 4> betaOzone{};
        std::array<float, 4> ozoneParameters{};
        std::array<float, 4> sunDiscParameters{};
        std::array<std::uint32_t, 4> controls{};
    };

    struct alignas(16) GpuChunkRecord {
        std::int32_t coordX{0};
        std::int32_t coordY{0};
        std::int32_t coordZ{0};
        std::uint32_t voxelResolution{0};
        std::uint32_t octreeNodeOffset{0};
        std::uint32_t octreeNodeCount{0};
        std::uint32_t reserved0{0};
        std::uint32_t reserved1{0};
    };

    struct GpuBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
    };

    struct GpuImage {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    struct PushConstants {
        std::array<float, 4> sceneMin{};
        std::array<float, 4> sceneMax{};
        std::array<float, 4> frameData{};
        std::array<float, 4> cameraPosition{};
        std::array<float, 4> cameraForward{};
        std::array<std::uint32_t, 4> settings{};
        std::array<std::int32_t, 4> chunkGridOrigin{};
        std::array<std::uint32_t, 4> chunkGridSize{};
    };

    struct DenoisePushConstants {
        std::array<std::uint32_t, 4> toggles{1U, 0U, 0U, 0U};
        std::array<float, 4> filterParameters0{8.0F, 0.0F, 0.0F, 0.0F};
    };

    struct TemporalPushConstants {
        std::array<std::uint32_t, 4> toggles{0U, 0U, 0U, 0U};
        std::array<float, 4> temporalParameters0{0.15F, 1.0F, 1.0F, 0.0F};
        std::array<float, 4> currentFrameData{};
        std::array<float, 4> currentCameraPosition{};
        std::array<float, 4> currentCameraForward{};
        std::array<float, 4> previousFrameData{};
        std::array<float, 4> previousCameraPosition{};
        std::array<float, 4> previousCameraForward{};
    };

    struct FilterPushConstants {
        std::array<float, 4> filterParameters0{1.0F, 6.0F, 24.0F, 1.0F};
    };

    struct FrameResources {
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        VkDescriptorSet temporalDescriptorSet{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, 3> filterDescriptorSets{
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
        };
        VkDescriptorSet denoiseDescriptorSet{VK_NULL_HANDLE};
        GpuBuffer chunkBuffer;
        GpuBuffer octreeBuffer;
        GpuBuffer chunkLookupBuffer;
        GpuBuffer lightBuffer;
        GpuBuffer atmosphereBuffer;
        GpuImage traceColor;
        GpuImage traceGuide;
        GpuImage historyColor;
        GpuImage filterPing;
        GpuImage filterPong;
        VkFramebuffer traceFramebuffer{VK_NULL_HANDLE};
        VkFramebuffer historyFramebuffer{VK_NULL_HANDLE};
        VkFramebuffer filterPingFramebuffer{VK_NULL_HANDLE};
        VkFramebuffer filterPongFramebuffer{VK_NULL_HANDLE};
        std::size_t uploadedChunkCount{0};
        std::size_t uploadedOctreeNodeCount{0};
        std::uint64_t worldRevision{~0ULL};
        std::uint64_t lightingRevision{~0ULL};
        std::array<float, 3> sceneMin{-1.0F, -1.0F, -1.0F};
        std::array<float, 3> sceneMax{1.0F, 1.0F, 1.0F};
        std::array<std::int32_t, 3> chunkGridOrigin{0, 0, 0};
        std::array<std::uint32_t, 3> chunkGridSize{1, 1, 1};
        std::array<std::int32_t, 3> cameraChunkCoord{0, 0, 0};
        std::uint32_t chunkResolution{32};
        std::uint64_t renderSettingsRevision{~0ULL};
        CameraRenderState cameraState{};
        bool historyValid{false};
    };

    [[nodiscard]] bool createDescriptorResources();
    [[nodiscard]] bool createTraceRenderPass();
    [[nodiscard]] bool createPostProcessRenderPass();
    [[nodiscard]] bool createTraceTargets();
    [[nodiscard]] bool createTraceTarget(FrameResources& frameResources);
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createTemporalPipeline();
    [[nodiscard]] bool createFilterPipeline();
    [[nodiscard]] bool createDenoisePipeline();
    [[nodiscard]] bool uploadSceneData(FrameResources& frameResources);
    [[nodiscard]] bool updateAtmosphereData(FrameResources& frameResources);
    [[nodiscard]] bool ensureBufferCapacity(GpuBuffer& buffer, VkDeviceSize sizeInBytes);
    [[nodiscard]] bool createBuffer(
        VkDeviceSize sizeInBytes,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        GpuBuffer& buffer);
    [[nodiscard]] bool createImage(
        VkExtent2D extent,
        VkFormat format,
        VkImageUsageFlags usage,
        GpuImage& image);
    void destroyBuffer(GpuBuffer& buffer) noexcept;
    void destroyImage(GpuImage& image) noexcept;
    void destroyPipeline() noexcept;
    void destroyDescriptorResources() noexcept;
    [[nodiscard]] std::uint32_t findMemoryType(
        std::uint32_t typeFilter,
        VkMemoryPropertyFlags properties) const;
    VulkanContext* m_context{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    RenderStateSnapshot m_renderStateSnapshot;
    PathTracerSettings m_settings;
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkRenderPass m_traceRenderPass{VK_NULL_HANDLE};
    VkRenderPass m_postProcessRenderPass{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_temporalDescriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_temporalDescriptorPool{VK_NULL_HANDLE};
    VkPipelineLayout m_temporalPipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_temporalPipeline{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_filterDescriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_filterDescriptorPool{VK_NULL_HANDLE};
    VkPipelineLayout m_filterPipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_filterPipeline{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_denoiseDescriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_denoiseDescriptorPool{VK_NULL_HANDLE};
    VkPipelineLayout m_denoisePipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_denoisePipeline{VK_NULL_HANDLE};
    VkSampler m_denoiseSampler{VK_NULL_HANDLE};
    std::vector<FrameResources> m_frameResources;
    std::size_t m_currentFrameSlot{0};
    std::uint32_t m_frameIndex{0};
};

} // namespace Meridian