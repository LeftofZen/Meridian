#pragma once

#include "world/WorldData.hpp"

#include <volk.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Meridian {

class VulkanContext;

struct TerrainHeightmapTile {
    ChunkCoord coord;
    std::uint32_t resolution{0};
    std::vector<float> grayscale;
    std::vector<float> ridgeMap;
    std::vector<float> erosion;
    std::vector<float> treeCoverage;
};

struct TerrainHeightmapSettings {
    std::uint32_t tileResolution{kWorldChunkResolution};
    std::uint32_t octaveCount{4};
    std::uint32_t heightOctaveCount{3};
    std::uint32_t worldSeed{1337};
    float minWorldHeight{-48.0F};
    float maxWorldHeight{96.0F};
    float baseFrequency{0.0065F};
    float heightFrequency{3.0F};
    float heightAmplitude{0.125F};
    float heightLacunarity{2.0F};
    float heightGain{0.1F};
    float erosionFrequency{0.15F};
    float erosionStrength{0.18F};
    float octaveGain{0.55F};
    float lacunarity{2.0F};
    float cellSizeMultiplier{2.75F};
    float slopeScale{0.7F};
    float stackedDetail{1.5F};
    float normalizationFactor{0.5F};
    float straightSteeringStrength{1.0F};
    float gullyWeight{0.5F};
    float ridgeRounding{0.1F};
    float creaseRounding{0.0F};
    float inputRoundingMultiplier{0.1F};
    float octaveRoundingMultiplier{2.0F};
    float onsetInitial{1.25F};
    float onsetOctave{1.25F};
    float ridgeMapOnsetInitial{2.8F};
    float ridgeMapOnsetOctave{1.5F};
    float heightOffsetBase{-0.65F};
    float heightOffsetFadeInfluence{0.0F};

    void clamp() noexcept;
    [[nodiscard]] float heightRange() const noexcept { return maxWorldHeight - minWorldHeight; }
};

class TerrainHeightmapGenerator final {
public:
    TerrainHeightmapGenerator() = default;
    ~TerrainHeightmapGenerator() = default;

    TerrainHeightmapGenerator(const TerrainHeightmapGenerator&) = delete;
    TerrainHeightmapGenerator& operator=(const TerrainHeightmapGenerator&) = delete;
    TerrainHeightmapGenerator(TerrainHeightmapGenerator&&) = delete;
    TerrainHeightmapGenerator& operator=(TerrainHeightmapGenerator&&) = delete;

    [[nodiscard]] bool init(VulkanContext& context);
    void shutdown() noexcept;

    [[nodiscard]] TerrainHeightmapTile generateTile(ChunkCoord coord);
    [[nodiscard]] TerrainHeightmapSettings settings() const;
    void setSettings(const TerrainHeightmapSettings& settings);
    void invalidateCache() noexcept;

private:
    struct GpuBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
    };

    struct PushConstants {
        std::int32_t chunkX{0};
        std::int32_t chunkZ{0};
        std::uint32_t resolution{0};
        std::uint32_t seed{0};
        std::array<float, 4> params0{};
        std::array<float, 4> params1{};
        std::array<float, 4> params2{};
        std::array<float, 4> params3{};
        std::array<float, 4> params4{};
        std::array<float, 4> params5{};
        std::array<float, 4> params6{};
    };

    [[nodiscard]] bool createDescriptorResources();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createCommandResources();
    [[nodiscard]] bool createOutputBuffer();
    [[nodiscard]] bool createBuffer(
        VkDeviceSize sizeInBytes,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        GpuBuffer& buffer);
    void updateDescriptorSet();
    void destroyBuffer(GpuBuffer& buffer) noexcept;
    void destroyCommandResources() noexcept;
    void destroyPipeline() noexcept;
    void destroyDescriptorResources() noexcept;
    [[nodiscard]] std::uint32_t findMemoryType(
        std::uint32_t typeFilter,
        VkMemoryPropertyFlags properties) const;
    [[nodiscard]] static ChunkKey tileKey(ChunkCoord coord) noexcept;
    [[nodiscard]] static std::filesystem::path shaderPath(const char* fileName);

    VulkanContext* m_context{nullptr};
    TerrainHeightmapSettings m_settings;
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    VkCommandBuffer m_commandBuffer{VK_NULL_HANDLE};
    VkFence m_computeFence{VK_NULL_HANDLE};
    GpuBuffer m_outputBuffer;
    mutable std::mutex m_mutex;
    std::unordered_map<ChunkKey, TerrainHeightmapTile> m_tileCache;
};

} // namespace Meridian