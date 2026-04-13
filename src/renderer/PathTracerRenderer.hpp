#pragma once

#include "renderer/IRenderFeature.hpp"
#include "renderer/PathTracerSettings.hpp"
#include "renderer/RenderStateStore.hpp"

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

    void setRenderStateStore(RenderStateStore& renderStateStore) noexcept
    {
        m_renderStateStore = &renderStateStore;
    }

    bool init(VulkanContext& context) override;
    void shutdown() override;
    void configureFrame(RenderFrameConfig& config) override;
    void beginFrame() override;
    void recordFrame(VkCommandBuffer commandBuffer) override;

private:
    struct alignas(16) GpuChunkRecord {
        std::int32_t coordX{0};
        std::int32_t coordY{0};
        std::int32_t coordZ{0};
        std::uint32_t voxelResolution{0};
        std::uint32_t voxelOffset{0};
        std::uint32_t voxelCount{0};
        std::uint32_t reserved0{0};
        std::uint32_t reserved1{0};
    };

    struct GpuBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
    };

    struct PushConstants {
        std::array<float, 4> sceneMin{};
        std::array<float, 4> sceneMax{};
        std::array<float, 4> frameData{};
        std::array<float, 4> cameraPosition{};
        std::array<float, 4> cameraForward{};
        std::array<std::uint32_t, 4> settings{};
    };

    [[nodiscard]] bool createDescriptorResources();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool uploadWorldData();
    [[nodiscard]] bool ensureBufferCapacity(GpuBuffer& buffer, VkDeviceSize sizeInBytes);
    [[nodiscard]] bool createBuffer(
        VkDeviceSize sizeInBytes,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        GpuBuffer& buffer);
    void destroyBuffer(GpuBuffer& buffer) noexcept;
    void destroyPipeline() noexcept;
    void destroyDescriptorResources() noexcept;
    [[nodiscard]] std::uint32_t findMemoryType(
        std::uint32_t typeFilter,
        VkMemoryPropertyFlags properties) const;
    [[nodiscard]] static std::filesystem::path shaderPath(const char* fileName);

    VulkanContext* m_context{nullptr};
    RenderStateStore* m_renderStateStore{nullptr};
    RenderStateSnapshot m_renderStateSnapshot;
    PathTracerSettings m_settings;
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    GpuBuffer m_chunkBuffer;
    GpuBuffer m_voxelBuffer;
    std::size_t m_uploadedChunkCount{0};
    std::size_t m_uploadedVoxelCount{0};
    std::uint64_t m_worldRevision{0};
    std::array<float, 3> m_sceneMin{};
    std::array<float, 3> m_sceneMax{};
    std::uint32_t m_frameIndex{0};
};

} // namespace Meridian