#include "renderer/PathTracerRenderer.hpp"

#include "core/Logger.hpp"
#include "renderer/ShaderLibrary.hpp"
#include "renderer/VulkanContext.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace Meridian {

namespace {

constexpr VkViewport kDefaultViewport{};
constexpr VkFormat kTraceColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kTraceGuideFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

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

[[nodiscard]] std::array<float, 4> packVec4(const std::array<float, 3>& value, float w) noexcept
{
    return {value[0], value[1], value[2], w};
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

    if (!createTraceRenderPass()) {
        destroyDescriptorResources();
        return false;
    }

    if (!createTraceTargets()) {
        destroyPipeline();
        destroyDescriptorResources();
        return false;
    }

    if (!createPipeline()) {
        destroyPipeline();
        destroyDescriptorResources();
        return false;
    }

    if (!createDenoisePipeline()) {
        destroyDescriptorResources();
        destroyPipeline();
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
        const bool sceneChanged =
            m_renderStateSnapshot.world.revision != frameResources.worldRevision ||
            m_renderStateSnapshot.lighting.revision != frameResources.lightingRevision ||
            m_renderStateSnapshot.worldRenderSettings.revision != frameResources.renderSettingsRevision ||
            currentCameraChunkCoord != frameResources.cameraChunkCoord;

        if (sceneChanged) {
            if (!uploadSceneData(frameResources)) {
                MRD_WARN("Path tracer scene upload failed");
            }
        }
    }
}

void PathTracerRenderer::recordPreRender(VkCommandBuffer commandBuffer)
{
    if (m_context == nullptr ||
        m_traceRenderPass == VK_NULL_HANDLE ||
        m_pipeline == VK_NULL_HANDLE ||
        m_pipelineLayout == VK_NULL_HANDLE ||
        m_frameResources.empty()) {
        return;
    }

    const FrameResources& frameResources = m_frameResources[m_currentFrameSlot];
    if (frameResources.descriptorSet == VK_NULL_HANDLE ||
        frameResources.traceFramebuffer == VK_NULL_HANDLE) {
        return;
    }

    const std::uint32_t fragmentShadingRate = m_context->fragmentShadingRateTexelSize();
    const TracyVkCtx tracyVkContext = m_context->tracyVkContext();
    if (tracyVkContext != nullptr) {
        TracyVkZone(tracyVkContext, commandBuffer, "Path Tracer Offscreen");
    }

    const VkExtent2D extent = m_context->getSwapchainExtent();

    const std::array<VkClearValue, 2> clearValues{
        VkClearValue{.color = {{0.0F, 0.0F, 0.0F, 1.0F}}},
        VkClearValue{.color = {{0.5F, 0.5F, 1.0F, 0.0F}}},
    };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_traceRenderPass;
    renderPassInfo.framebuffer = frameResources.traceFramebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;
    renderPassInfo.clearValueCount = static_cast<std::uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

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
            m_renderStateSnapshot.camera.projection.verticalFovDegrees,
            m_renderStateSnapshot.camera.projection.aspectRatio,
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
            static_cast<std::uint32_t>(m_settings.maxDdaSteps),
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

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
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
    vkCmdEndRenderPass(commandBuffer);
    m_context->applyFragmentShadingRate(commandBuffer, 1U);
}

void PathTracerRenderer::recordFrame(VkCommandBuffer commandBuffer)
{
    if (m_context == nullptr ||
        m_denoisePipeline == VK_NULL_HANDLE ||
        m_denoisePipelineLayout == VK_NULL_HANDLE ||
        m_frameResources.empty()) {
        return;
    }

    const FrameResources& frameResources = m_frameResources[m_currentFrameSlot];
    if (frameResources.denoiseDescriptorSet == VK_NULL_HANDLE) {
        return;
    }

    const TracyVkCtx tracyVkContext = m_context->tracyVkContext();
    if (tracyVkContext != nullptr) {
        TracyVkZone(tracyVkContext, commandBuffer, "Path Tracer Denoise");
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

    const DenoisePushConstants pushConstants{
        .toggles = {
            m_settings.denoiserEnabled ? 1U : 0U,
            static_cast<std::uint32_t>(m_settings.denoiserDebugView),
            0U,
            0U,
        },
        .filterParameters0 = {
            m_settings.denoiserKernelStep,
            m_settings.denoiserColorPhi,
            m_settings.denoiserNormalPhi,
            m_settings.denoiserDifferenceGain,
        },
    };

    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_denoisePipeline);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_denoisePipelineLayout,
        0,
        1,
        &frameResources.denoiseDescriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_denoisePipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(DenoisePushConstants),
        &pushConstants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
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
        VkDescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
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
            .descriptorCount = static_cast<std::uint32_t>(frameCount * 4U),
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

    const VkDescriptorSetLayoutBinding denoiseBindings[] = {
        VkDescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo denoiseLayoutInfo{};
    denoiseLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    denoiseLayoutInfo.bindingCount = 2;
    denoiseLayoutInfo.pBindings = denoiseBindings;

    if (vkCreateDescriptorSetLayout(
            m_context->getDevice(),
            &denoiseLayoutInfo,
            nullptr,
            &m_denoiseDescriptorSetLayout) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer denoise descriptor set layout");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_denoiseDescriptorSetLayout),
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        "Path Tracer Denoise Descriptor Set Layout");

    const VkDescriptorPoolSize denoisePoolSize{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = static_cast<std::uint32_t>(frameCount * 2U),
    };

    VkDescriptorPoolCreateInfo denoisePoolInfo{};
    denoisePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    denoisePoolInfo.maxSets = static_cast<std::uint32_t>(frameCount);
    denoisePoolInfo.poolSizeCount = 1;
    denoisePoolInfo.pPoolSizes = &denoisePoolSize;

    if (vkCreateDescriptorPool(
            m_context->getDevice(),
            &denoisePoolInfo,
            nullptr,
            &m_denoiseDescriptorPool) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer denoise descriptor pool");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_denoiseDescriptorPool),
        VK_OBJECT_TYPE_DESCRIPTOR_POOL,
        "Path Tracer Denoise Descriptor Pool");

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0F;
    samplerInfo.minLod = 0.0F;
    samplerInfo.maxLod = 0.0F;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    if (vkCreateSampler(m_context->getDevice(), &samplerInfo, nullptr, &m_denoiseSampler) !=
        VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer denoise sampler");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_denoiseSampler),
        VK_OBJECT_TYPE_SAMPLER,
        "Path Tracer Denoise Sampler");

    VkDescriptorSetAllocateInfo denoiseAllocInfo{};
    denoiseAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    denoiseAllocInfo.descriptorPool = m_denoiseDescriptorPool;
    std::vector<VkDescriptorSetLayout> denoiseLayouts(frameCount, m_denoiseDescriptorSetLayout);
    std::vector<VkDescriptorSet> denoiseDescriptorSets(frameCount, VK_NULL_HANDLE);
    denoiseAllocInfo.descriptorSetCount = static_cast<std::uint32_t>(frameCount);
    denoiseAllocInfo.pSetLayouts = denoiseLayouts.data();

    if (vkAllocateDescriptorSets(
            m_context->getDevice(),
            &denoiseAllocInfo,
            denoiseDescriptorSets.data()) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate path tracer denoise descriptor set");
        return false;
    }

    for (std::size_t index = 0; index < frameCount; ++index) {
        m_frameResources[index].denoiseDescriptorSet = denoiseDescriptorSets[index];
        m_context->setObjectDebugName(
            reinterpret_cast<std::uint64_t>(denoiseDescriptorSets[index]),
            VK_OBJECT_TYPE_DESCRIPTOR_SET,
            std::format("Path Tracer Denoise Descriptor Set {}", index));
    }

    return true;
}

bool PathTracerRenderer::createTraceRenderPass()
{
    if (m_context == nullptr) {
        return false;
    }

    const std::array<VkAttachmentDescription, 2> attachments{
        VkAttachmentDescription{
            .format = kTraceColorFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        VkAttachmentDescription{
            .format = kTraceGuideFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
    };

    const std::array<VkAttachmentReference, 2> colorAttachmentRefs{
        VkAttachmentReference{
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
        VkAttachmentReference{
            .attachment = 1,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
    };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(colorAttachmentRefs.size());
    subpass.pColorAttachments = colorAttachmentRefs.data();

    VkSubpassDependency dependencyFromExternal{};
    dependencyFromExternal.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencyFromExternal.dstSubpass = 0;
    dependencyFromExternal.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencyFromExternal.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencyFromExternal.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkSubpassDependency dependencyToExternal{};
    dependencyToExternal.srcSubpass = 0;
    dependencyToExternal.dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencyToExternal.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencyToExternal.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencyToExternal.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencyToExternal.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    const std::array<VkSubpassDependency, 2> dependencies{
        dependencyFromExternal,
        dependencyToExternal,
    };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    if (vkCreateRenderPass(m_context->getDevice(), &renderPassInfo, nullptr, &m_traceRenderPass) !=
        VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer offscreen render pass");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_traceRenderPass),
        VK_OBJECT_TYPE_RENDER_PASS,
        "Path Tracer Offscreen Render Pass");

    return true;
}

bool PathTracerRenderer::createTraceTargets()
{
    for (FrameResources& frameResources : m_frameResources) {
        if (!createTraceTarget(frameResources)) {
            return false;
        }
    }

    return true;
}

bool PathTracerRenderer::createPipeline()
{
    if (m_context == nullptr) {
        return false;
    }

    ShaderLibrary& shaderLibrary = m_context->getShaderLibrary();
    const VkShaderModule vertexShader = shaderLibrary.loadBuiltInModule("fullscreen_triangle.vert");
    const VkShaderModule fragmentShader = shaderLibrary.loadBuiltInModule("basic_pathtracer.frag");
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

    VkPipelineColorBlendAttachmentState colorBlendAttachment0{};
    colorBlendAttachment0.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment0.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment1{};
    colorBlendAttachment1.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment1.blendEnable = VK_FALSE;

    const std::array<VkPipelineColorBlendAttachmentState, 2> colorBlendAttachments{
        colorBlendAttachment0,
        colorBlendAttachment1,
    };

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = static_cast<std::uint32_t>(colorBlendAttachments.size());
    colorBlending.pAttachments = colorBlendAttachments.data();

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
    pipelineInfo.renderPass = m_traceRenderPass;
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

bool PathTracerRenderer::createDenoisePipeline()
{
    if (m_context == nullptr) {
        return false;
    }

    ShaderLibrary& shaderLibrary = m_context->getShaderLibrary();
    const VkShaderModule vertexShader = shaderLibrary.loadBuiltInModule("fullscreen_triangle.vert");
    const VkShaderModule fragmentShader = shaderLibrary.loadBuiltInModule("pathtracer_denoise.frag");
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        MRD_ERROR("Path tracer denoise shader load failed");
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

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_denoiseDescriptorSetLayout;

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(DenoisePushConstants);
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(
            m_context->getDevice(),
            &pipelineLayoutInfo,
            nullptr,
            &m_denoisePipelineLayout) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer denoise pipeline layout");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_denoisePipelineLayout),
        VK_OBJECT_TYPE_PIPELINE_LAYOUT,
        "Path Tracer Denoise Pipeline Layout");

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
    pipelineInfo.layout = m_denoisePipelineLayout;
    pipelineInfo.renderPass = m_context->getRenderPass();
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(
            m_context->getDevice(),
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &m_denoisePipeline) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer denoise graphics pipeline");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_denoisePipeline),
        VK_OBJECT_TYPE_PIPELINE,
        "Path Tracer Denoise Pipeline");

    return true;
}

bool PathTracerRenderer::uploadSceneData(FrameResources& frameResources)
{
    if (m_context == nullptr || frameResources.descriptorSet == VK_NULL_HANDLE) {
        return false;
    }

    const std::uint32_t chunkResolution = m_renderStateSnapshot.world.chunks.empty()
        ? frameResources.chunkResolution
        : m_renderStateSnapshot.world.chunks.front().voxelResolution;
    const std::array<std::int32_t, 3> currentCameraChunkCoord =
        cameraChunkCoord(m_renderStateSnapshot.camera, chunkResolution);

    std::vector<GpuChunkRecord> chunkRecords;
    std::vector<std::uint32_t> octreeNodeWords;
    std::vector<std::uint32_t> chunkLookup;
    GpuLightScene lightScene{};
    chunkRecords.reserve(m_renderStateSnapshot.world.chunks.size());
    octreeNodeWords.reserve(m_renderStateSnapshot.world.uploadedOctreeNodeCount * 9U);

    lightScene.sun.directionAndIntensity = packVec4(
        m_renderStateSnapshot.lighting.sun.direction,
        m_renderStateSnapshot.lighting.sun.intensity);
    lightScene.sun.color = packVec4(m_renderStateSnapshot.lighting.sun.color, 0.0F);

    const std::size_t pointLightCount = std::min(
        m_renderStateSnapshot.lighting.pointLights.size(),
        static_cast<std::size_t>(kMaxPointLights));
    const std::size_t areaLightCount = std::min(
        m_renderStateSnapshot.lighting.areaLights.size(),
        static_cast<std::size_t>(kMaxAreaLights));
    lightScene.counts = {
        static_cast<std::uint32_t>(pointLightCount),
        static_cast<std::uint32_t>(areaLightCount),
        0U,
        0U,
    };

    for (std::size_t index = 0; index < pointLightCount; ++index) {
        const PointLightRenderState& light = m_renderStateSnapshot.lighting.pointLights[index];
        lightScene.pointLights[index].positionAndRange = packVec4(light.positionMeters, light.rangeMeters);
        lightScene.pointLights[index].colorAndIntensity = packVec4(light.color, light.intensity);
    }

    for (std::size_t index = 0; index < areaLightCount; ++index) {
        const AreaLightRenderState& light = m_renderStateSnapshot.lighting.areaLights[index];
        lightScene.areaLights[index].centerAndIntensity = packVec4(light.centerMeters, light.intensity);
        lightScene.areaLights[index].rightExtentAndDoubleSided = packVec4(
            light.rightExtentMeters,
            light.doubleSided ? 1.0F : 0.0F);
        lightScene.areaLights[index].upExtent = packVec4(light.upExtentMeters, 0.0F);
        lightScene.areaLights[index].color = packVec4(light.color, 0.0F);
    }

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
        const std::vector<std::uint32_t>* packedOctreeNodes = chunk.packedOctreeNodes.get();
        const std::size_t nodeWordCount = packedOctreeNodes != nullptr ? packedOctreeNodes->size() : 0U;
        const std::size_t nodeCount = nodeWordCount / 9U;

        const std::uint32_t octreeNodeOffset =
            static_cast<std::uint32_t>(octreeNodeWords.size() / 9U);
        if (packedOctreeNodes != nullptr && !packedOctreeNodes->empty()) {
            octreeNodeWords.insert(
                octreeNodeWords.end(),
                packedOctreeNodes->begin(),
                packedOctreeNodes->end());
        }

        chunkRecords.push_back(GpuChunkRecord{
            .coordX = chunk.coord.x,
            .coordY = chunk.coord.y,
            .coordZ = chunk.coord.z,
            .voxelResolution = chunk.voxelResolution,
            .octreeNodeOffset = octreeNodeOffset,
            .octreeNodeCount = static_cast<std::uint32_t>(nodeCount),
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
        octreeNodeWords.push_back(0U);
        chunkLookup.push_back(0U);
        sceneMin = {-1.0F, -1.0F, -1.0F};
        sceneMax = {1.0F, 1.0F, 1.0F};
        frameResources.chunkGridOrigin = {0, 0, 0};
        frameResources.chunkGridSize = {0, 0, 0};
        frameResources.chunkResolution = 32;
        frameResources.uploadedChunkCount = 0;
        frameResources.uploadedOctreeNodeCount = 0;
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
        frameResources.uploadedOctreeNodeCount = m_renderStateSnapshot.world.uploadedOctreeNodeCount;
    }

    frameResources.sceneMin = sceneMin;
    frameResources.sceneMax = sceneMax;
    frameResources.worldRevision = m_renderStateSnapshot.world.revision;
    frameResources.lightingRevision = m_renderStateSnapshot.lighting.revision;
    frameResources.cameraChunkCoord = currentCameraChunkCoord;
    frameResources.renderSettingsRevision = m_renderStateSnapshot.worldRenderSettings.revision;

    const VkDeviceSize chunkBufferSize =
        static_cast<VkDeviceSize>(chunkRecords.size() * sizeof(GpuChunkRecord));
    const VkDeviceSize octreeBufferSize =
        static_cast<VkDeviceSize>(octreeNodeWords.size() * sizeof(std::uint32_t));
    const VkDeviceSize chunkLookupBufferSize =
        static_cast<VkDeviceSize>(chunkLookup.size() * sizeof(std::uint32_t));
    const VkDeviceSize lightBufferSize = static_cast<VkDeviceSize>(sizeof(GpuLightScene));

    if (!ensureBufferCapacity(frameResources.chunkBuffer, chunkBufferSize) ||
        !ensureBufferCapacity(frameResources.octreeBuffer, octreeBufferSize) ||
        !ensureBufferCapacity(frameResources.chunkLookupBuffer, chunkLookupBufferSize) ||
        !ensureBufferCapacity(frameResources.lightBuffer, lightBufferSize)) {
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
        frameResources.octreeBuffer.memory,
        0,
        octreeBufferSize,
        0,
        &mappedData);
    std::memcpy(mappedData, octreeNodeWords.data(), static_cast<std::size_t>(octreeBufferSize));
    vkUnmapMemory(m_context->getDevice(), frameResources.octreeBuffer.memory);

    vkMapMemory(
        m_context->getDevice(),
        frameResources.chunkLookupBuffer.memory,
        0,
        chunkLookupBufferSize,
        0,
        &mappedData);
    std::memcpy(mappedData, chunkLookup.data(), static_cast<std::size_t>(chunkLookupBufferSize));
    vkUnmapMemory(m_context->getDevice(), frameResources.chunkLookupBuffer.memory);

    vkMapMemory(
        m_context->getDevice(),
        frameResources.lightBuffer.memory,
        0,
        lightBufferSize,
        0,
        &mappedData);
    std::memcpy(mappedData, &lightScene, static_cast<std::size_t>(lightBufferSize));
    vkUnmapMemory(m_context->getDevice(), frameResources.lightBuffer.memory);

    const VkDescriptorBufferInfo chunkBufferInfo{
        .buffer = frameResources.chunkBuffer.buffer,
        .offset = 0,
        .range = chunkBufferSize,
    };
    const VkDescriptorBufferInfo octreeBufferInfo{
        .buffer = frameResources.octreeBuffer.buffer,
        .offset = 0,
        .range = octreeBufferSize,
    };
    const VkDescriptorBufferInfo chunkLookupBufferInfo{
        .buffer = frameResources.chunkLookupBuffer.buffer,
        .offset = 0,
        .range = chunkLookupBufferSize,
    };
    const VkDescriptorBufferInfo lightBufferInfo{
        .buffer = frameResources.lightBuffer.buffer,
        .offset = 0,
        .range = lightBufferSize,
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
            .pBufferInfo = &octreeBufferInfo,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frameResources.descriptorSet,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &chunkLookupBufferInfo,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frameResources.descriptorSet,
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &lightBufferInfo,
        },
    };

    vkUpdateDescriptorSets(m_context->getDevice(), 4, writes, 0, nullptr);
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

bool PathTracerRenderer::createImage(
    VkExtent2D extent,
    VkFormat format,
    VkImageUsageFlags usage,
    GpuImage& image)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {
        .width = extent.width,
        .height = extent.height,
        .depth = 1,
    };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(m_context->getDevice(), &imageInfo, nullptr, &image.image) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer intermediate image");
        return false;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(m_context->getDevice(), image.image, &memoryRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &image.memory) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate path tracer intermediate image memory");
        destroyImage(image);
        return false;
    }

    if (vkBindImageMemory(m_context->getDevice(), image.image, image.memory, 0) != VK_SUCCESS) {
        MRD_ERROR("Failed to bind path tracer intermediate image memory");
        destroyImage(image);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_context->getDevice(), &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer intermediate image view");
        destroyImage(image);
        return false;
    }

    return true;
}

bool PathTracerRenderer::createTraceTarget(FrameResources& frameResources)
{
    if (m_context == nullptr || m_traceRenderPass == VK_NULL_HANDLE) {
        return false;
    }

    if (!createImage(
            m_context->getSwapchainExtent(),
            kTraceColorFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            frameResources.traceColor)) {
        return false;
    }

    if (!createImage(
            m_context->getSwapchainExtent(),
            kTraceGuideFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            frameResources.traceGuide)) {
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(frameResources.traceColor.image),
        VK_OBJECT_TYPE_IMAGE,
        "Path Tracer Intermediate Image");
    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(frameResources.traceColor.memory),
        VK_OBJECT_TYPE_DEVICE_MEMORY,
        "Path Tracer Intermediate Image Memory");
    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(frameResources.traceColor.view),
        VK_OBJECT_TYPE_IMAGE_VIEW,
        "Path Tracer Intermediate Image View");

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(frameResources.traceGuide.image),
        VK_OBJECT_TYPE_IMAGE,
        "Path Tracer Guide Image");
    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(frameResources.traceGuide.memory),
        VK_OBJECT_TYPE_DEVICE_MEMORY,
        "Path Tracer Guide Image Memory");
    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(frameResources.traceGuide.view),
        VK_OBJECT_TYPE_IMAGE_VIEW,
        "Path Tracer Guide Image View");

    const std::array<VkImageView, 2> attachments{
        frameResources.traceColor.view,
        frameResources.traceGuide.view,
    };
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_traceRenderPass;
    framebufferInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = m_context->getSwapchainExtent().width;
    framebufferInfo.height = m_context->getSwapchainExtent().height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(
            m_context->getDevice(),
            &framebufferInfo,
            nullptr,
            &frameResources.traceFramebuffer) != VK_SUCCESS) {
        MRD_ERROR("Failed to create path tracer offscreen framebuffer");
        return false;
    }

    m_context->setObjectDebugName(
        reinterpret_cast<std::uint64_t>(frameResources.traceFramebuffer),
        VK_OBJECT_TYPE_FRAMEBUFFER,
        "Path Tracer Offscreen Framebuffer");

    const VkDescriptorImageInfo denoiseImageInfo{
        .sampler = m_denoiseSampler,
        .imageView = frameResources.traceColor.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    const VkDescriptorImageInfo denoiseGuideImageInfo{
        .sampler = m_denoiseSampler,
        .imageView = frameResources.traceGuide.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    const VkWriteDescriptorSet denoiseWrites[] = {
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frameResources.denoiseDescriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &denoiseImageInfo,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frameResources.denoiseDescriptorSet,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &denoiseGuideImageInfo,
        },
    };
    vkUpdateDescriptorSets(
        m_context->getDevice(),
        static_cast<std::uint32_t>(std::size(denoiseWrites)),
        denoiseWrites,
        0,
        nullptr);

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

    if (m_context != nullptr && m_denoisePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_denoisePipeline, nullptr);
        m_denoisePipeline = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_denoisePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_denoisePipelineLayout, nullptr);
        m_denoisePipelineLayout = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_traceRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_context->getDevice(), m_traceRenderPass, nullptr);
        m_traceRenderPass = VK_NULL_HANDLE;
    }
}

void PathTracerRenderer::destroyDescriptorResources() noexcept
{
    for (FrameResources& frameResources : m_frameResources) {
        destroyBuffer(frameResources.chunkBuffer);
        destroyBuffer(frameResources.octreeBuffer);
        destroyBuffer(frameResources.chunkLookupBuffer);
        destroyBuffer(frameResources.lightBuffer);
        if (m_context != nullptr && frameResources.traceFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_context->getDevice(), frameResources.traceFramebuffer, nullptr);
        }
        frameResources.traceFramebuffer = VK_NULL_HANDLE;
        destroyImage(frameResources.traceColor);
        destroyImage(frameResources.traceGuide);
        frameResources.descriptorSet = VK_NULL_HANDLE;
        frameResources.denoiseDescriptorSet = VK_NULL_HANDLE;
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

    if (m_context != nullptr && m_denoiseSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->getDevice(), m_denoiseSampler, nullptr);
        m_denoiseSampler = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_denoiseDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context->getDevice(), m_denoiseDescriptorPool, nullptr);
        m_denoiseDescriptorPool = VK_NULL_HANDLE;
    }

    if (m_context != nullptr && m_denoiseDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_denoiseDescriptorSetLayout, nullptr);
        m_denoiseDescriptorSetLayout = VK_NULL_HANDLE;
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

void PathTracerRenderer::destroyImage(GpuImage& image) noexcept
{
    if (m_context != nullptr && image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), image.view, nullptr);
    }
    if (m_context != nullptr && image.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->getDevice(), image.image, nullptr);
    }
    if (m_context != nullptr && image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), image.memory, nullptr);
    }

    image = {};
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

} // namespace Meridian