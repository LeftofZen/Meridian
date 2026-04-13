#include "renderer/PathTracerRenderer.hpp"

#include "core/Logger.hpp"
#include "renderer/ShaderLibrary.hpp"
#include "renderer/VulkanContext.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Meridian {

namespace {

constexpr VkViewport kDefaultViewport{};

} // namespace

bool PathTracerRenderer::init(VulkanContext& context)
{
    shutdown();

    m_context = &context;
    m_settings.clamp();
    m_frameIndex = 0;
    m_uploadedChunkCount = 0;
    m_uploadedVoxelCount = 0;
    m_sceneMin = {-1.0F, -1.0F, -1.0F};
    m_sceneMax = {1.0F, 1.0F, 1.0F};

    if (!createDescriptorResources()) {
        return false;
    }

    if (!createPipeline()) {
        destroyDescriptorResources();
        return false;
    }

    return uploadWorldData();
}

void PathTracerRenderer::shutdown()
{
    destroyPipeline();
    destroyDescriptorResources();
    m_context = nullptr;
    m_frameIndex = 0;
    m_uploadedChunkCount = 0;
    m_uploadedVoxelCount = 0;
}

void PathTracerRenderer::configureFrame(RenderFrameConfig& config)
{
    config.clearColor = {0.0F, 0.0F, 0.0F, 1.0F};
}

void PathTracerRenderer::beginFrame()
{
    m_settings.clamp();

    if (m_renderStateStore != nullptr) {
        m_renderStateSnapshot = m_renderStateStore->snapshot();
        const bool worldChanged =
            m_renderStateSnapshot.world.residentChunkCount != m_uploadedChunkCount ||
            m_renderStateSnapshot.world.uploadedVoxelCount != m_uploadedVoxelCount;

        if (worldChanged) {
            vkDeviceWaitIdle(m_context->getDevice());
            if (!uploadWorldData()) {
                MRD_WARN("Path tracer world upload failed");
            }
        }
    }
}

void PathTracerRenderer::recordFrame(VkCommandBuffer commandBuffer)
{
    if (m_context == nullptr || m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE) {
        return;
    }

    const VkExtent2D extent = m_context->getSwapchainExtent();

    VkViewport viewport = kDefaultViewport;
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;

    const PushConstants pushConstants{
        .sceneMin = {
            m_sceneMin[0],
            m_sceneMin[1],
            m_sceneMin[2],
            0.0F,
        },
        .sceneMax = {
            m_sceneMax[0],
            m_sceneMax[1],
            m_sceneMax[2],
            0.0F,
        },
        .frameData = {
            static_cast<float>(extent.width),
            static_cast<float>(extent.height),
            0.0F,
            0.0F,
        },
        .cameraPosition = {
            m_renderStateSnapshot.camera.position[0],
            m_renderStateSnapshot.camera.position[1],
            m_renderStateSnapshot.camera.position[2],
            0.0F,
        },
        .cameraForward = {
            m_renderStateSnapshot.camera.forward[0],
            m_renderStateSnapshot.camera.forward[1],
            m_renderStateSnapshot.camera.forward[2],
            0.0F,
        },
        .settings = {
            m_frameIndex++,
            static_cast<std::uint32_t>(m_settings.samplesPerPixel),
            static_cast<std::uint32_t>(m_settings.maxBounces),
            static_cast<std::uint32_t>(m_uploadedChunkCount),
        },
    };

    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout,
        0,
        1,
        &m_descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PushConstants),
        &pushConstants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

bool PathTracerRenderer::createDescriptorResources()
{
    if (m_context == nullptr) {
        return false;
    }

    const VkDescriptorSetLayoutBinding bindings[] = {
        VkDescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(
            m_context->getDevice(),
            &layoutInfo,
            nullptr,
            &m_descriptorSetLayout) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer descriptor set layout");
        return false;
    }

    const VkDescriptorPoolSize poolSizes[] = {
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 2,
        },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(
            m_context->getDevice(),
            &poolInfo,
            nullptr,
            &m_descriptorPool) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer descriptor pool");
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
        MRD_ERROR("Failed to allocate path tracer descriptor set");
        return false;
    }

    return true;
}

bool PathTracerRenderer::createPipeline()
{
    if (m_context == nullptr) {
        return false;
    }

    ShaderLibrary& shaderLibrary = m_context->getShaderLibrary();
    const VkShaderModule vertexShader = shaderLibrary.loadModule(
        "fullscreen_triangle.vert",
        shaderPath("fullscreen_triangle.vert.spv"));
    const VkShaderModule fragmentShader = shaderLibrary.loadModule(
        "basic_pathtracer.frag",
        shaderPath("basic_pathtracer.frag.spv"));
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        MRD_ERROR("Path tracer shader load failed");
        return false;
    }

    const VkPipelineShaderStageCreateInfo shaderStages[] = {
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertexShader,
            .pName = "main",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragmentShader,
            .pName = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(
            m_context->getDevice(),
            &pipelineLayoutInfo,
            nullptr,
            &m_pipelineLayout) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer pipeline layout");
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_context->getRenderPass();
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(
            m_context->getDevice(),
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &m_pipeline) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer graphics pipeline");
        destroyPipeline();
        return false;
    }

    return true;
}

bool PathTracerRenderer::uploadWorldData()
{
    if (m_context == nullptr || m_descriptorSet == VK_NULL_HANDLE) {
        return false;
    }

    std::vector<GpuChunkRecord> chunkRecords;
    std::vector<std::uint32_t> voxelMaterials;
    chunkRecords.reserve(m_renderStateSnapshot.world.chunks.size());
    voxelMaterials.reserve(m_renderStateSnapshot.world.uploadedVoxelCount);

    std::array<float, 3> sceneMin{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3> sceneMax{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    for (const WorldChunkRenderData& chunk : m_renderStateSnapshot.world.chunks) {
        const std::uint32_t voxelOffset = static_cast<std::uint32_t>(voxelMaterials.size());
        voxelMaterials.insert(
            voxelMaterials.end(),
            chunk.materialIds.begin(),
            chunk.materialIds.end());

        chunkRecords.push_back(GpuChunkRecord{
            .coordX = chunk.coord.x,
            .coordY = chunk.coord.y,
            .coordZ = chunk.coord.z,
            .voxelResolution = chunk.voxelResolution,
            .voxelOffset = voxelOffset,
            .voxelCount = static_cast<std::uint32_t>(chunk.materialIds.size()),
        });

        const float chunkMinX = static_cast<float>(chunk.coord.x * static_cast<int>(chunk.voxelResolution));
        const float chunkMinY = static_cast<float>(chunk.coord.y * static_cast<int>(chunk.voxelResolution));
        const float chunkMinZ = static_cast<float>(chunk.coord.z * static_cast<int>(chunk.voxelResolution));
        const float chunkMaxX = chunkMinX + static_cast<float>(chunk.voxelResolution);
        const float chunkMaxY = chunkMinY + static_cast<float>(chunk.voxelResolution);
        const float chunkMaxZ = chunkMinZ + static_cast<float>(chunk.voxelResolution);

        sceneMin[0] = std::min(sceneMin[0], chunkMinX);
        sceneMin[1] = std::min(sceneMin[1], chunkMinY);
        sceneMin[2] = std::min(sceneMin[2], chunkMinZ);
        sceneMax[0] = std::max(sceneMax[0], chunkMaxX);
        sceneMax[1] = std::max(sceneMax[1], chunkMaxY);
        sceneMax[2] = std::max(sceneMax[2], chunkMaxZ);
    }

    if (chunkRecords.empty()) {
        chunkRecords.push_back(GpuChunkRecord{});
        voxelMaterials.push_back(0U);
        sceneMin = {-1.0F, -1.0F, -1.0F};
        sceneMax = {1.0F, 1.0F, 1.0F};
        m_uploadedChunkCount = 0;
        m_uploadedVoxelCount = 0;
    } else {
        m_uploadedChunkCount = m_renderStateSnapshot.world.chunks.size();
        m_uploadedVoxelCount = m_renderStateSnapshot.world.uploadedVoxelCount;
    }

    m_sceneMin = sceneMin;
    m_sceneMax = sceneMax;

    const VkDeviceSize chunkBufferSize =
        static_cast<VkDeviceSize>(chunkRecords.size() * sizeof(GpuChunkRecord));
    const VkDeviceSize voxelBufferSize =
        static_cast<VkDeviceSize>(voxelMaterials.size() * sizeof(std::uint32_t));

    if (!ensureBufferCapacity(m_chunkBuffer, chunkBufferSize) ||
        !ensureBufferCapacity(m_voxelBuffer, voxelBufferSize)) {
        return false;
    }

    void* mappedData = nullptr;
    vkMapMemory(m_context->getDevice(), m_chunkBuffer.memory, 0, chunkBufferSize, 0, &mappedData);
    std::memcpy(mappedData, chunkRecords.data(), static_cast<std::size_t>(chunkBufferSize));
    vkUnmapMemory(m_context->getDevice(), m_chunkBuffer.memory);

    vkMapMemory(m_context->getDevice(), m_voxelBuffer.memory, 0, voxelBufferSize, 0, &mappedData);
    std::memcpy(mappedData, voxelMaterials.data(), static_cast<std::size_t>(voxelBufferSize));
    vkUnmapMemory(m_context->getDevice(), m_voxelBuffer.memory);

    const VkDescriptorBufferInfo chunkBufferInfo{
        .buffer = m_chunkBuffer.buffer,
        .offset = 0,
        .range = chunkBufferSize,
    };
    const VkDescriptorBufferInfo voxelBufferInfo{
        .buffer = m_voxelBuffer.buffer,
        .offset = 0,
        .range = voxelBufferSize,
    };

    const VkWriteDescriptorSet writes[] = {
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = m_descriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &chunkBufferInfo,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = m_descriptorSet,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &voxelBufferInfo,
        },
    };

    vkUpdateDescriptorSets(m_context->getDevice(), 2, writes, 0, nullptr);
    return true;
}

bool PathTracerRenderer::ensureBufferCapacity(GpuBuffer& buffer, VkDeviceSize sizeInBytes)
{
    const VkDeviceSize requiredSize = std::max<VkDeviceSize>(sizeInBytes, sizeof(std::uint32_t));
    if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= requiredSize) {
        return true;
    }

    destroyBuffer(buffer);
    return createBuffer(
        requiredSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        buffer);
}

bool PathTracerRenderer::createBuffer(
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
        MRD_ERROR("Failed to create path tracer storage buffer");
        return false;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_context->getDevice(), buffer.buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &buffer.memory) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate path tracer buffer memory");
        destroyBuffer(buffer);
        return false;
    }

    if (vkBindBufferMemory(m_context->getDevice(), buffer.buffer, buffer.memory, 0) != VK_SUCCESS) {
        MRD_ERROR("Failed to bind path tracer buffer memory");
        destroyBuffer(buffer);
        return false;
    }

    buffer.size = memoryRequirements.size;
    return true;
}

void PathTracerRenderer::destroyPipeline() noexcept
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

void PathTracerRenderer::destroyDescriptorResources() noexcept
{
    destroyBuffer(m_chunkBuffer);
    destroyBuffer(m_voxelBuffer);

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

void PathTracerRenderer::destroyBuffer(GpuBuffer& buffer) noexcept
{
    if (m_context != nullptr && buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context->getDevice(), buffer.buffer, nullptr);
    }
    if (m_context != nullptr && buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), buffer.memory, nullptr);
    }

    buffer = {};
}

std::uint32_t PathTracerRenderer::findMemoryType(
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

    throw std::runtime_error("Failed to find suitable path tracer memory type");
}

std::filesystem::path PathTracerRenderer::shaderPath(const char* fileName)
{
    return std::filesystem::path{MERIDIAN_SHADER_OUTPUT_DIR} / fileName;
}

} // namespace Meridian