#include "renderer/PathTracerRenderer.hpp"

#include "core/Logger.hpp"
#include "renderer/ShaderLibrary.hpp"
#include "renderer/VulkanContext.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Meridian {

namespace {

constexpr VkViewport kDefaultViewport{};

[[nodiscard]] std::int32_t worldToChunkCoord(float worldPosition, std::uint32_t chunkResolution) noexcept
{
    return static_cast<std::int32_t>(
        std::floor(worldPosition / std::max(1.0F, static_cast<float>(chunkResolution))));
}

[[nodiscard]] std::array<std::int32_t, 3> cameraChunkCoord(
    const CameraRenderState& camera,
    std::uint32_t chunkResolution) noexcept
{
    return {
        worldToChunkCoord(camera.position[0], chunkResolution),
        worldToChunkCoord(camera.position[1], chunkResolution),
        worldToChunkCoord(camera.position[2], chunkResolution),
    };
}

[[nodiscard]] bool shouldRenderChunk(
    const WorldChunkRenderData& chunk,
    const std::array<std::int32_t, 3>& cameraChunk,
    float renderDistanceChunks) noexcept
{
    const float deltaX = static_cast<float>(chunk.coord.x - cameraChunk[0]);
    const float deltaY = static_cast<float>(chunk.coord.y - cameraChunk[1]);
    const float deltaZ = static_cast<float>(chunk.coord.z - cameraChunk[2]);
    const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
    return distanceSquared <= renderDistanceChunks * renderDistanceChunks;
}

} // namespace

bool PathTracerRenderer::init(VulkanContext& context)
{
    shutdown();

    m_context = &context;
    m_settings.clamp();
    m_renderStateSnapshot = RenderStateSnapshot{};
    m_currentFrameSlot = 0;
    m_frameIndex = 0;

    if (!createDescriptorResources()) {
        return false;
    }

    if (!createPipeline()) {
        destroyDescriptorResources();
        return false;
    }

    return true;
}

void PathTracerRenderer::shutdown()
{
    destroyPipeline();
    destroyDescriptorResources();
    m_context = nullptr;
    m_currentFrameSlot = 0;
    m_frameIndex = 0;
}

void PathTracerRenderer::configureFrame(RenderFrameConfig& config)
{
    config.clearColor = {0.0F, 0.0F, 0.0F, 1.0F};
}

void PathTracerRenderer::beginFrame()
{
    m_settings.clamp();
    if (m_context == nullptr || m_frameResources.empty()) {
        return;
    }

    m_currentFrameSlot = m_context->getCurrentFrameSlot();
    if (m_currentFrameSlot >= m_frameResources.size()) {
        m_currentFrameSlot = 0;
    }

    FrameResources& frameResources = m_frameResources[m_currentFrameSlot];

    if (m_renderStateStore != nullptr) {
        m_renderStateSnapshot = m_renderStateStore->snapshot();
        const std::array<std::int32_t, 3> currentCameraChunkCoord = cameraChunkCoord(
            m_renderStateSnapshot.camera,
            frameResources.chunkResolution);
        const bool worldChanged =
            m_renderStateSnapshot.world.revision != frameResources.worldRevision ||
            m_renderStateSnapshot.worldRenderSettings.revision != frameResources.renderSettingsRevision ||
            currentCameraChunkCoord != frameResources.cameraChunkCoord;

        if (worldChanged) {
            if (!uploadWorldData(frameResources)) {
                MRD_WARN("Path tracer world upload failed");
            }
        }
    }
}

void PathTracerRenderer::recordFrame(VkCommandBuffer commandBuffer)
{
    if (m_context == nullptr ||
        m_pipeline == VK_NULL_HANDLE ||
        m_pipelineLayout == VK_NULL_HANDLE ||
        m_frameResources.empty()) {
        return;
    }

    const FrameResources& frameResources = m_frameResources[m_currentFrameSlot];
    if (frameResources.descriptorSet == VK_NULL_HANDLE) {
        return;
    }

    const std::uint32_t fragmentShadingRate = m_context->fragmentShadingRateTexelSize();

    const TracyVkCtx tracyVkContext = m_context->tracyVkContext();
    if (tracyVkContext != nullptr) {
        TracyVkZone(tracyVkContext, commandBuffer, "Path Tracer Draw");
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
            frameResources.sceneMin[0],
            frameResources.sceneMin[1],
            frameResources.sceneMin[2],
            0.0F,
        },
        .sceneMax = {
            frameResources.sceneMax[0],
            frameResources.sceneMax[1],
            frameResources.sceneMax[2],
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
            static_cast<std::uint32_t>(frameResources.uploadedChunkCount),
        },
        .chunkGridOrigin = {
            frameResources.chunkGridOrigin[0],
            frameResources.chunkGridOrigin[1],
            frameResources.chunkGridOrigin[2],
            0,
        },
        .chunkGridSize = {
            frameResources.chunkGridSize[0],
            frameResources.chunkGridSize[1],
            frameResources.chunkGridSize[2],
            frameResources.chunkResolution,
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
        &frameResources.descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PushConstants),
        &pushConstants);
    m_context->applyFragmentShadingRate(commandBuffer, fragmentShadingRate);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    m_context->applyFragmentShadingRate(commandBuffer, 1U);
}

bool PathTracerRenderer::createDescriptorResources()
{
    if (m_context == nullptr) {
        return false;
    }

    const std::size_t frameCount = std::max<std::size_t>(1, m_context->getFramesInFlightCount());
    m_frameResources.assign(frameCount, FrameResources{});

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
        VkDescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(
            m_context->getDevice(),
            &layoutInfo,
            nullptr,
            &m_descriptorSetLayout) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer descriptor set layout");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_descriptorSetLayout),
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        "Path Tracer Descriptor Set Layout");

    const VkDescriptorPoolSize poolSizes[] = {
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = static_cast<std::uint32_t>(frameCount * 3U),
        },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<std::uint32_t>(frameCount);
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

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_descriptorPool),
        VK_OBJECT_TYPE_DESCRIPTOR_POOL,
        "Path Tracer Descriptor Pool");

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_descriptorSetLayout);
    std::vector<VkDescriptorSet> descriptorSets(frameCount, VK_NULL_HANDLE);
    allocInfo.descriptorSetCount = static_cast<std::uint32_t>(frameCount);
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(
            m_context->getDevice(),
            &allocInfo,
            descriptorSets.data()) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate path tracer descriptor set");
        return false;
    }

    for (std::size_t index = 0; index < frameCount; ++index) {
        m_frameResources[index].descriptorSet = descriptorSets[index];
        m_context->setObjectDebugName(
            reinterpret_cast<std::uint64_t>(descriptorSets[index]),
            VK_OBJECT_TYPE_DESCRIPTOR_SET,
            std::format("Path Tracer Descriptor Set {}", index));
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

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_pipelineLayout),
        VK_OBJECT_TYPE_PIPELINE_LAYOUT,
        "Path Tracer Pipeline Layout");

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

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_pipeline),
        VK_OBJECT_TYPE_PIPELINE,
        "Path Tracer Pipeline");

    return true;
}

bool PathTracerRenderer::uploadWorldData(FrameResources& frameResources)
{
    if (m_context == nullptr || frameResources.descriptorSet == VK_NULL_HANDLE) {
        return false;
    }

    const float renderDistanceChunks =
        m_renderStateSnapshot.worldRenderSettings.renderDistanceChunks;
    const std::uint32_t chunkResolution = m_renderStateSnapshot.world.chunks.empty()
        ? frameResources.chunkResolution
        : m_renderStateSnapshot.world.chunks.front().voxelResolution;
    const std::array<std::int32_t, 3> currentCameraChunkCoord =
        cameraChunkCoord(m_renderStateSnapshot.camera, chunkResolution);

    std::vector<GpuChunkRecord> chunkRecords;
    std::vector<std::uint32_t> voxelMaterials;
    std::vector<std::uint32_t> chunkLookup;
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
    std::array<std::int32_t, 3> minChunkCoord{
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::max(),
    };
    std::array<std::int32_t, 3> maxChunkCoord{
        std::numeric_limits<std::int32_t>::lowest(),
        std::numeric_limits<std::int32_t>::lowest(),
        std::numeric_limits<std::int32_t>::lowest(),
    };

    for (const WorldChunkRenderData& chunk : m_renderStateSnapshot.world.chunks) {
        if (!shouldRenderChunk(chunk, currentCameraChunkCoord, renderDistanceChunks)) {
            continue;
        }

        const std::vector<std::uint32_t>* materialIds = chunk.materialIds.get();
        const std::size_t voxelCount = materialIds != nullptr ? materialIds->size() : 0U;

        const std::uint32_t voxelOffset = static_cast<std::uint32_t>(voxelMaterials.size());
        if (materialIds != nullptr && !materialIds->empty()) {
            voxelMaterials.insert(
                voxelMaterials.end(),
                materialIds->begin(),
                materialIds->end());
        }

        chunkRecords.push_back(GpuChunkRecord{
            .coordX = chunk.coord.x,
            .coordY = chunk.coord.y,
            .coordZ = chunk.coord.z,
            .voxelResolution = chunk.voxelResolution,
            .voxelOffset = voxelOffset,
            .voxelCount = static_cast<std::uint32_t>(voxelCount),
        });

        minChunkCoord[0] = std::min(minChunkCoord[0], chunk.coord.x);
        minChunkCoord[1] = std::min(minChunkCoord[1], chunk.coord.y);
        minChunkCoord[2] = std::min(minChunkCoord[2], chunk.coord.z);
        maxChunkCoord[0] = std::max(maxChunkCoord[0], chunk.coord.x);
        maxChunkCoord[1] = std::max(maxChunkCoord[1], chunk.coord.y);
        maxChunkCoord[2] = std::max(maxChunkCoord[2], chunk.coord.z);

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
        chunkLookup.push_back(0U);
        sceneMin = {-1.0F, -1.0F, -1.0F};
        sceneMax = {1.0F, 1.0F, 1.0F};
        frameResources.chunkGridOrigin = {0, 0, 0};
        frameResources.chunkGridSize = {1, 1, 1};
        frameResources.chunkResolution = 32;
        frameResources.uploadedChunkCount = 0;
        frameResources.uploadedVoxelCount = 0;
    } else {
        frameResources.chunkGridOrigin = minChunkCoord;
        frameResources.chunkGridSize = {
            static_cast<std::uint32_t>(maxChunkCoord[0] - minChunkCoord[0] + 1),
            static_cast<std::uint32_t>(maxChunkCoord[1] - minChunkCoord[1] + 1),
            static_cast<std::uint32_t>(maxChunkCoord[2] - minChunkCoord[2] + 1),
        };
        frameResources.chunkResolution = chunkRecords.front().voxelResolution;

        chunkLookup.assign(
            static_cast<std::size_t>(frameResources.chunkGridSize[0]) *
                static_cast<std::size_t>(frameResources.chunkGridSize[1]) *
                static_cast<std::size_t>(frameResources.chunkGridSize[2]),
            0U);

        for (std::size_t chunkIndex = 0; chunkIndex < chunkRecords.size(); ++chunkIndex) {
            const GpuChunkRecord& chunkRecord = chunkRecords[chunkIndex];
            const std::uint32_t relativeX =
                static_cast<std::uint32_t>(chunkRecord.coordX - frameResources.chunkGridOrigin[0]);
            const std::uint32_t relativeY =
                static_cast<std::uint32_t>(chunkRecord.coordY - frameResources.chunkGridOrigin[1]);
            const std::uint32_t relativeZ =
                static_cast<std::uint32_t>(chunkRecord.coordZ - frameResources.chunkGridOrigin[2]);
            const std::size_t lookupIndex =
                static_cast<std::size_t>(relativeX) +
                static_cast<std::size_t>(relativeY) *
                    static_cast<std::size_t>(frameResources.chunkGridSize[0]) +
                static_cast<std::size_t>(relativeZ) *
                    static_cast<std::size_t>(frameResources.chunkGridSize[0]) *
                    static_cast<std::size_t>(frameResources.chunkGridSize[1]);
            chunkLookup[lookupIndex] = static_cast<std::uint32_t>(chunkIndex) + 1U;
        }

        frameResources.uploadedChunkCount = m_renderStateSnapshot.world.chunks.size();
        frameResources.uploadedVoxelCount = m_renderStateSnapshot.world.uploadedVoxelCount;
    }

    frameResources.sceneMin = sceneMin;
    frameResources.sceneMax = sceneMax;
    frameResources.worldRevision = m_renderStateSnapshot.world.revision;
    frameResources.cameraChunkCoord = currentCameraChunkCoord;
    frameResources.renderDistanceChunks = renderDistanceChunks;
    frameResources.renderSettingsRevision = m_renderStateSnapshot.worldRenderSettings.revision;

    const VkDeviceSize chunkBufferSize =
        static_cast<VkDeviceSize>(chunkRecords.size() * sizeof(GpuChunkRecord));
    const VkDeviceSize voxelBufferSize =
        static_cast<VkDeviceSize>(voxelMaterials.size() * sizeof(std::uint32_t));
    const VkDeviceSize chunkLookupBufferSize =
        static_cast<VkDeviceSize>(chunkLookup.size() * sizeof(std::uint32_t));

    if (!ensureBufferCapacity(frameResources.chunkBuffer, chunkBufferSize) ||
        !ensureBufferCapacity(frameResources.voxelBuffer, voxelBufferSize) ||
        !ensureBufferCapacity(frameResources.chunkLookupBuffer, chunkLookupBufferSize)) {
        return false;
    }

    void* mappedData = nullptr;
    vkMapMemory(
        m_context->getDevice(),
        frameResources.chunkBuffer.memory,
        0,
        chunkBufferSize,
        0,
        &mappedData);
    std::memcpy(mappedData, chunkRecords.data(), static_cast<std::size_t>(chunkBufferSize));
    vkUnmapMemory(m_context->getDevice(), frameResources.chunkBuffer.memory);

    vkMapMemory(
        m_context->getDevice(),
        frameResources.voxelBuffer.memory,
        0,
        voxelBufferSize,
        0,
        &mappedData);
    std::memcpy(mappedData, voxelMaterials.data(), static_cast<std::size_t>(voxelBufferSize));
    vkUnmapMemory(m_context->getDevice(), frameResources.voxelBuffer.memory);

    vkMapMemory(
        m_context->getDevice(),
        frameResources.chunkLookupBuffer.memory,
        0,
        chunkLookupBufferSize,
        0,
        &mappedData);
    std::memcpy(mappedData, chunkLookup.data(), static_cast<std::size_t>(chunkLookupBufferSize));
    vkUnmapMemory(m_context->getDevice(), frameResources.chunkLookupBuffer.memory);

    const VkDescriptorBufferInfo chunkBufferInfo{
        .buffer = frameResources.chunkBuffer.buffer,
        .offset = 0,
        .range = chunkBufferSize,
    };
    const VkDescriptorBufferInfo voxelBufferInfo{
        .buffer = frameResources.voxelBuffer.buffer,
        .offset = 0,
        .range = voxelBufferSize,
    };
    const VkDescriptorBufferInfo chunkLookupBufferInfo{
        .buffer = frameResources.chunkLookupBuffer.buffer,
        .offset = 0,
        .range = chunkLookupBufferSize,
    };

    const VkWriteDescriptorSet writes[] = {
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frameResources.descriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &chunkBufferInfo,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frameResources.descriptorSet,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &voxelBufferInfo,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frameResources.descriptorSet,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &chunkLookupBufferInfo,
        },
    };

    vkUpdateDescriptorSets(m_context->getDevice(), 3, writes, 0, nullptr);
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

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(buffer.buffer),
        VK_OBJECT_TYPE_BUFFER,
        "Path Tracer Storage Buffer");
    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(buffer.memory),
        VK_OBJECT_TYPE_DEVICE_MEMORY,
        "Path Tracer Buffer Memory");

    buffer.size = sizeInBytes;
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
    for (FrameResources& frameResources : m_frameResources) {
        destroyBuffer(frameResources.chunkBuffer);
        destroyBuffer(frameResources.voxelBuffer);
        destroyBuffer(frameResources.chunkLookupBuffer);
        frameResources.descriptorSet = VK_NULL_HANDLE;
    }
    m_frameResources.clear();

    if (m_context != nullptr && m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context->getDevice(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
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