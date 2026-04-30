#include "world/TerrainHeightmapGenerator.hpp"

#include "core/Logger.hpp"
#include "renderer/ShaderLibrary.hpp"
#include "renderer/VulkanContext.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <tracy/Tracy.hpp>

namespace Meridian {

namespace {

struct TerrainOutputSample {
    float height;
    float ridgeMap;
    float erosion;
    float treeCoverage;
};

} // namespace

void TerrainHeightmapSettings::clamp() noexcept
{
    tileResolution = std::max<std::uint32_t>(1U, tileResolution);
    octaveCount = std::clamp<std::uint32_t>(octaveCount, 1U, 8U);
    heightOctaveCount = std::clamp<std::uint32_t>(heightOctaveCount, 1U, 8U);
    if (maxWorldHeight < minWorldHeight) {
        std::swap(minWorldHeight, maxWorldHeight);
    }
    baseFrequency = std::clamp(baseFrequency, 0.0001F, 1.0F);
    heightFrequency = std::clamp(heightFrequency, 0.1F, 16.0F);
    heightAmplitude = std::clamp(heightAmplitude, 0.001F, 1.0F);
    heightLacunarity = std::clamp(heightLacunarity, 1.1F, 8.0F);
    heightGain = std::clamp(heightGain, 0.01F, 1.0F);
    erosionFrequency = std::clamp(erosionFrequency, 0.01F, 2.0F);
    erosionStrength = std::clamp(erosionStrength, 0.0F, 4.0F);
    octaveGain = std::clamp(octaveGain, 0.05F, 1.0F);
    lacunarity = std::clamp(lacunarity, 1.1F, 6.0F);
    cellSizeMultiplier = std::clamp(cellSizeMultiplier, 0.25F, 2.0F);
    slopeScale = std::clamp(slopeScale, 0.05F, 4.0F);
    stackedDetail = std::clamp(stackedDetail, 0.25F, 6.0F);
    normalizationFactor = std::clamp(normalizationFactor, 0.0F, 1.0F);
    straightSteeringStrength = std::clamp(straightSteeringStrength, 0.0F, 1.0F);
    gullyWeight = std::clamp(gullyWeight, 0.0F, 1.0F);
    ridgeRounding = std::clamp(ridgeRounding, 0.0F, 2.5F);
    creaseRounding = std::clamp(creaseRounding, 0.0F, 2.5F);
    inputRoundingMultiplier = std::clamp(inputRoundingMultiplier, 0.0F, 4.0F);
    octaveRoundingMultiplier = std::clamp(octaveRoundingMultiplier, 0.25F, 8.0F);
    onsetInitial = std::clamp(onsetInitial, 0.1F, 8.0F);
    onsetOctave = std::clamp(onsetOctave, 0.1F, 8.0F);
    ridgeMapOnsetInitial = std::clamp(ridgeMapOnsetInitial, 0.1F, 8.0F);
    ridgeMapOnsetOctave = std::clamp(ridgeMapOnsetOctave, 0.1F, 8.0F);
    heightOffsetBase = std::clamp(heightOffsetBase, -1.0F, 1.0F);
    heightOffsetFadeInfluence = std::clamp(heightOffsetFadeInfluence, 0.0F, 1.0F);
}

bool TerrainHeightmapGenerator::init(VulkanContext& context)
{
    shutdown();

    m_context = &context;
    m_tileCache.clear();

    if (!createDescriptorResources()) {
        shutdown();
        return false;
    }

    if (!createOutputBuffer()) {
        shutdown();
        return false;
    }

    if (!createPipeline()) {
        shutdown();
        return false;
    }

    if (!createCommandResources()) {
        shutdown();
        return false;
    }

    MRD_INFO(
        "TerrainHeightmapGenerator ready ({}x{}, {} octaves)",
        m_settings.tileResolution,
        m_settings.tileResolution,
        m_settings.octaveCount);
    return true;
}

TerrainHeightmapSettings TerrainHeightmapGenerator::settings() const
{
    std::scoped_lock lock(m_mutex);
    return m_settings;
}

std::vector<std::shared_ptr<const TerrainHeightmapTile>> TerrainHeightmapGenerator::cachedTiles() const
{
    std::scoped_lock lock(m_mutex);

    std::vector<std::shared_ptr<const TerrainHeightmapTile>> tiles;
    tiles.reserve(m_tileCache.size());
    for (const auto& [key, tile] : m_tileCache) {
        (void)key;
        tiles.push_back(tile);
    }

    return tiles;
}

void TerrainHeightmapGenerator::setSettings(const TerrainHeightmapSettings& settings)
{
    std::scoped_lock lock(m_mutex);
    m_settings = settings;
    m_settings.clamp();
    m_tileCache.clear();
}

void TerrainHeightmapGenerator::invalidateCache() noexcept
{
    std::scoped_lock lock(m_mutex);
    m_tileCache.clear();
}

void TerrainHeightmapGenerator::shutdown() noexcept
{
    if (m_context != nullptr && m_context->getDevice() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_context->getDevice());
    }

    m_tileCache.clear();
    destroyCommandResources();
    destroyPipeline();
    destroyDescriptorResources();
    destroyBuffer(m_outputBuffer);
    m_context = nullptr;
}

std::shared_ptr<TerrainHeightmapTile> TerrainHeightmapGenerator::generateTile(ChunkCoord coord)
{
    ZoneScopedN("TerrainHeightmapGenerator::generateTile");
    std::scoped_lock lock(m_mutex);

    const ChunkCoord tileCoord{.x = coord.x, .y = 0, .z = coord.z};
    const ChunkKey key = tileKey(tileCoord);
    if (const auto existing = m_tileCache.find(key); existing != m_tileCache.end()) {
        ZoneText("cache-hit", 9);
        return existing->second;
    }

    ZoneText("cache-miss", 10);

    std::shared_ptr<TerrainHeightmapTile> tile = std::make_shared<TerrainHeightmapTile>(TerrainHeightmapTile{
        .coord = tileCoord,
        .resolution = m_settings.tileResolution,
        .grayscale = std::vector<float>(
            static_cast<std::size_t>(m_settings.tileResolution) *
            m_settings.tileResolution),
        .ridgeMap = std::vector<float>(
            static_cast<std::size_t>(m_settings.tileResolution) *
            m_settings.tileResolution,
            0.5F),
        .erosion = std::vector<float>(
            static_cast<std::size_t>(m_settings.tileResolution) *
            m_settings.tileResolution,
            0.5F),
        .treeCoverage = std::vector<float>(
            static_cast<std::size_t>(m_settings.tileResolution) *
            m_settings.tileResolution,
            0.0F),
    });

    if (m_context == nullptr || m_pipeline == VK_NULL_HANDLE || m_commandBuffer == VK_NULL_HANDLE) {
        return tile;
    }

    const VkDevice device = m_context->getDevice();
    {
        ZoneScopedN("TerrainHeightmapGenerator::waitForFence");
        vkWaitForFences(device, 1, &m_computeFence, VK_TRUE, UINT64_MAX);
    }
    vkResetFences(device, 1, &m_computeFence);
    vkResetCommandPool(device, m_commandPool, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
        MRD_ERROR("Failed to begin terrain heightmap command buffer");
        return tile;
    }

    const PushConstants pushConstants{
        .chunkX = coord.x,
        .chunkZ = coord.z,
        .resolution = m_settings.tileResolution,
        .seed = m_settings.worldSeed,
        .params0 = {
            m_settings.baseFrequency,
            m_settings.heightFrequency,
            m_settings.heightAmplitude,
            m_settings.erosionFrequency,
        },
        .params1 = {
            m_settings.erosionStrength,
            m_settings.gullyWeight,
            m_settings.stackedDetail,
            m_settings.octaveGain,
        },
        .params2 = {
            m_settings.lacunarity,
            m_settings.cellSizeMultiplier,
            m_settings.normalizationFactor,
            m_settings.slopeScale,
        },
        .params3 = {
            m_settings.straightSteeringStrength,
            m_settings.ridgeRounding,
            m_settings.creaseRounding,
            m_settings.inputRoundingMultiplier,
        },
        .params4 = {
            m_settings.octaveRoundingMultiplier,
            m_settings.onsetInitial,
            m_settings.onsetOctave,
            m_settings.ridgeMapOnsetInitial,
        },
        .params5 = {
            m_settings.ridgeMapOnsetOctave,
            m_settings.heightOffsetBase,
            m_settings.heightOffsetFadeInfluence,
            m_settings.heightLacunarity,
        },
        .params6 = {
            m_settings.heightGain,
            static_cast<float>(m_settings.heightOctaveCount),
            static_cast<float>(m_settings.octaveCount),
            static_cast<float>(kWorldChunkSize),
        },
    };

    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(
        m_commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        m_pipelineLayout,
        0,
        1,
        &m_descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        m_commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(PushConstants),
        &pushConstants);

    const std::uint32_t groupCount = (m_settings.tileResolution + 7U) / 8U;
    {
        ZoneScopedN("TerrainHeightmapGenerator::dispatchCompute");
        vkCmdDispatch(m_commandBuffer, groupCount, groupCount, 1);
    }

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = m_outputBuffer.buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(
        m_commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);

    if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
        MRD_ERROR("Failed to end terrain heightmap command buffer");
        return tile;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;

    VkQueue queue = m_context->hasComputeSupport() ? m_context->getComputeQueue() :
                                                   m_context->getGraphicsQueue();
    {
        ZoneScopedN("TerrainHeightmapGenerator::submitCompute");
        std::scoped_lock queueLock(m_context->getQueueSubmitMutex());
        if (vkQueueSubmit(queue, 1, &submitInfo, m_computeFence) != VK_SUCCESS) {
            MRD_ERROR("Failed to submit terrain heightmap compute work");
            return tile;
        }
    }

    {
        ZoneScopedN("TerrainHeightmapGenerator::waitForComputeResult");
        vkWaitForFences(device, 1, &m_computeFence, VK_TRUE, UINT64_MAX);
    }

    void* mappedData = nullptr;
    if (vkMapMemory(device, m_outputBuffer.memory, 0, m_outputBuffer.size, 0, &mappedData) !=
        VK_SUCCESS) {
        MRD_ERROR("Failed to map terrain heightmap output buffer");
        return tile;
    }

    {
        ZoneScopedN("TerrainHeightmapGenerator::readbackTile");
        const auto* outputSamples = static_cast<const TerrainOutputSample*>(mappedData);
        for (std::size_t sampleIndex = 0; sampleIndex < tile->grayscale.size(); ++sampleIndex) {
            tile->grayscale[sampleIndex] = outputSamples[sampleIndex].height;
            tile->ridgeMap[sampleIndex] = outputSamples[sampleIndex].ridgeMap;
            tile->erosion[sampleIndex] = outputSamples[sampleIndex].erosion;
            tile->treeCoverage[sampleIndex] = outputSamples[sampleIndex].treeCoverage;
        }
    }
    vkUnmapMemory(device, m_outputBuffer.memory);

    m_tileCache.emplace(key, tile);
    return tile;
}

bool TerrainHeightmapGenerator::createDescriptorResources()
{
    if (m_context == nullptr) {
        return false;
    }

    const VkDescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(
            m_context->getDevice(),
            &layoutInfo,
            nullptr,
            &m_descriptorSetLayout) != VK_SUCCESS) {
        MRD_ERROR("Failed to create terrain heightmap descriptor set layout");
        return false;
    }

    const VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(
            m_context->getDevice(),
            &poolInfo,
            nullptr,
            &m_descriptorPool) != VK_SUCCESS) {
        MRD_ERROR("Failed to create terrain heightmap descriptor pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(
            m_context->getDevice(),
            &allocInfo,
            &m_descriptorSet) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate terrain heightmap descriptor set");
        return false;
    }

    return true;
}

bool TerrainHeightmapGenerator::createPipeline()
{
    if (m_context == nullptr) {
        return false;
    }

    ShaderLibrary& shaderLibrary = m_context->getShaderLibrary();
    const VkShaderModule computeShader = shaderLibrary.loadBuiltInModule("terrain_heightmap.comp");
    if (computeShader == VK_NULL_HANDLE) {
        MRD_ERROR("Terrain heightmap compute shader load failed");
        return false;
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(
            m_context->getDevice(),
            &layoutInfo,
            nullptr,
            &m_pipelineLayout) != VK_SUCCESS) {
        MRD_ERROR("Failed to create terrain heightmap pipeline layout");
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShader;
    pipelineInfo.stage.pName = "main";

    if (vkCreateComputePipelines(
            m_context->getDevice(),
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &m_pipeline) != VK_SUCCESS) {
        MRD_ERROR("Failed to create terrain heightmap compute pipeline");
        return false;
    }

    return true;
}

bool TerrainHeightmapGenerator::createCommandResources()
{
    if (m_context == nullptr) {
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_context->getComputeQueueFamily();

    if (vkCreateCommandPool(m_context->getDevice(), &poolInfo, nullptr, &m_commandPool) !=
        VK_SUCCESS) {
        MRD_ERROR("Failed to create terrain heightmap command pool");
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(
            m_context->getDevice(),
            &allocInfo,
            &m_commandBuffer) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate terrain heightmap command buffer");
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateFence(m_context->getDevice(), &fenceInfo, nullptr, &m_computeFence) !=
        VK_SUCCESS) {
        MRD_ERROR("Failed to create terrain heightmap fence");
        return false;
    }

    return true;
}

bool TerrainHeightmapGenerator::createOutputBuffer()
{
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(m_settings.tileResolution) *
        m_settings.tileResolution *
        sizeof(TerrainOutputSample);

    if (!createBuffer(
            byteSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_outputBuffer)) {
        return false;
    }

    updateDescriptorSet();
    return true;
}

bool TerrainHeightmapGenerator::createBuffer(
    VkDeviceSize sizeInBytes,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    GpuBuffer& buffer)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeInBytes;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_context->getDevice(), &bufferInfo, nullptr, &buffer.buffer) != VK_SUCCESS) {
        MRD_ERROR("Failed to create terrain heightmap buffer");
        return false;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_context->getDevice(), buffer.buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &buffer.memory) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate terrain heightmap buffer memory");
        destroyBuffer(buffer);
        return false;
    }

    if (vkBindBufferMemory(m_context->getDevice(), buffer.buffer, buffer.memory, 0) != VK_SUCCESS) {
        MRD_ERROR("Failed to bind terrain heightmap buffer memory");
        destroyBuffer(buffer);
        return false;
    }

    buffer.size = memoryRequirements.size;
    return true;
}

void TerrainHeightmapGenerator::updateDescriptorSet()
{
    if (m_context == nullptr || m_descriptorSet == VK_NULL_HANDLE || m_outputBuffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    const VkDescriptorBufferInfo bufferInfo{
        .buffer = m_outputBuffer.buffer,
        .offset = 0,
        .range = m_outputBuffer.size,
    };

    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = m_descriptorSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bufferInfo,
    };

    vkUpdateDescriptorSets(m_context->getDevice(), 1, &write, 0, nullptr);
}

void TerrainHeightmapGenerator::destroyBuffer(GpuBuffer& buffer) noexcept
{
    if (m_context != nullptr && buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context->getDevice(), buffer.buffer, nullptr);
    }
    if (m_context != nullptr && buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), buffer.memory, nullptr);
    }

    buffer = {};
}

void TerrainHeightmapGenerator::destroyCommandResources() noexcept
{
    if (m_context != nullptr && m_computeFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_context->getDevice(), m_computeFence, nullptr);
        m_computeFence = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_context->getDevice(), m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }

    m_commandBuffer = VK_NULL_HANDLE;
}

void TerrainHeightmapGenerator::destroyPipeline() noexcept
{
    if (m_context != nullptr && m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
}

void TerrainHeightmapGenerator::destroyDescriptorResources() noexcept
{
    if (m_context != nullptr && m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context->getDevice(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    m_descriptorSet = VK_NULL_HANDLE;
}

std::uint32_t TerrainHeightmapGenerator::findMemoryType(
    std::uint32_t typeFilter,
    VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_context->getPhysicalDevice(), &memoryProperties);

    for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
        if ((typeFilter & (1U << index)) != 0U &&
            (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
            return index;
        }
    }

    throw std::runtime_error("Failed to find suitable terrain heightmap memory type");
}

ChunkKey TerrainHeightmapGenerator::tileKey(ChunkCoord coord) noexcept
{
    return makeChunkKey(ChunkCoord{.x = coord.x, .y = 0, .z = coord.z});
}

} // namespace Meridian