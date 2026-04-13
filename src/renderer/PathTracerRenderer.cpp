#include "renderer/PathTracerRenderer.hpp"

#include "core/Logger.hpp"
#include "renderer/ShaderLibrary.hpp"
#include "renderer/VulkanContext.hpp"

#include <array>

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
    return createPipeline();
}

void PathTracerRenderer::shutdown()
{
    destroyPipeline();
    m_context = nullptr;
    m_frameIndex = 0;
}

void PathTracerRenderer::configureFrame(RenderFrameConfig& config)
{
    config.clearColor = {0.0F, 0.0F, 0.0F, 1.0F};
}

void PathTracerRenderer::beginFrame()
{
    m_settings.clamp();
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
        .resolution = {
            static_cast<float>(extent.width),
            static_cast<float>(extent.height),
        },
        .frameIndex = m_frameIndex++,
        .samplesPerPixel = static_cast<std::uint32_t>(m_settings.samplesPerPixel),
        .maxBounces = static_cast<std::uint32_t>(m_settings.maxBounces),
    };

    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdPushConstants(
        commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PushConstants),
        &pushConstants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
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

std::filesystem::path PathTracerRenderer::shaderPath(const char* fileName)
{
    return std::filesystem::path{MERIDIAN_SHADER_OUTPUT_DIR} / fileName;
}

} // namespace Meridian