#include "renderer/RenderFramePipeline.hpp"

#include "core/Logger.hpp"
#include "renderer/VulkanContext.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>
#include <tracy/Tracy.hpp>

#include <cstring>

namespace Meridian {

namespace {

PFN_vkVoidFunction imguiVulkanLoader(const char* functionName, void* userData)
{
    const auto* context = static_cast<const VulkanContext*>(userData);
    if (context == nullptr || functionName == nullptr) {
        return nullptr;
    }

    return vkGetInstanceProcAddr(context->getInstance(), functionName);
}

} // namespace

RenderFramePipeline::~RenderFramePipeline()
{
    shutdown();
}

void RenderFramePipeline::addFeature(IRenderFeature& feature)
{
    m_features.push_back(&feature);
}

bool RenderFramePipeline::init(SDL_Window* window, VulkanContext& context)
{
    shutdown();

    m_context = &context;
    m_windowHandle = window;
    m_frameConfig = RenderFrameConfig{};

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        MRD_ERROR("ImGui SDL3 backend init failed");
        ImGui::DestroyContext();
        return false;
    }

    if (!createDescriptorPool()) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = context.getInstance();
    initInfo.PhysicalDevice = context.getPhysicalDevice();
    initInfo.Device = context.getDevice();
    initInfo.QueueFamily = context.getGraphicsQueueFamily();
    initInfo.Queue = context.getGraphicsQueue();
    initInfo.DescriptorPool = m_descriptorPool;
    initInfo.MinImageCount = context.getMinImageCount();
    initInfo.ImageCount = static_cast<uint32_t>(context.getSwapchainImageCount());
    initInfo.Allocator = nullptr;
    initInfo.PipelineInfoMain.RenderPass = context.getRenderPass();
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = checkVkResult;
    initInfo.MinAllocationSize = 1024 * 1024;

    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imguiVulkanLoader, &context)) {
        MRD_ERROR("ImGui Vulkan loader bootstrap failed");
        destroyDescriptorPool();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        MRD_ERROR("ImGui Vulkan backend init failed");
        destroyDescriptorPool();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    for (IRenderFeature* feature : m_features) {
        if (feature != nullptr && !feature->init(context)) {
            MRD_ERROR("Render feature init failed");
            shutdown();
            return false;
        }
    }

    m_initialised.store(true, std::memory_order_release);
    return true;
}

void RenderFramePipeline::shutdown()
{
    if (!m_initialised.load(std::memory_order_acquire)) {
        m_context = nullptr;
        m_windowHandle = nullptr;
        return;
    }

    for (auto it = m_features.rbegin(); it != m_features.rend(); ++it) {
        if (*it != nullptr) {
            (*it)->shutdown();
        }
    }

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    destroyDescriptorPool();

    m_initialised.store(false, std::memory_order_release);
    m_context = nullptr;
    m_windowHandle = nullptr;
}

void RenderFramePipeline::handleEvent(const SDL_Event& event)
{
    if (!m_initialised.load(std::memory_order_acquire)) {
        return;
    }

    std::scoped_lock lock(m_eventMutex);
    m_pendingEvents.push_back(event);
}

void RenderFramePipeline::beginFrame()
{
    if (!m_initialised.load(std::memory_order_acquire)) {
        return;
    }

    ZoneScopedN("RenderFramePipeline::beginFrame");
    m_frameConfig = RenderFrameConfig{};

    std::vector<SDL_Event> pendingEvents;
    {
        std::scoped_lock lock(m_eventMutex);
        pendingEvents.swap(m_pendingEvents);
    }

    {
        ZoneScopedN("Process Render Events");
        for (const SDL_Event& event : pendingEvents) {
            SDL_Event mutableEvent = event;
            ImGui_ImplSDL3_ProcessEvent(&mutableEvent);

            for (IRenderFeature* feature : m_features) {
                if (feature != nullptr) {
                    feature->handleEvent(event);
                }
            }
        }
    }

    {
        ZoneScopedN("ImGui New Frame");
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    for (std::size_t index = 0; index < m_features.size(); ++index) {
        IRenderFeature* feature = m_features[index];
        if (feature != nullptr) {
            ZoneScopedN("IRenderFeature::configureFrame");
            const char* featureName = feature->name();
            ZoneName(featureName, std::strlen(featureName));
            feature->configureFrame(m_frameConfig);
        }
    }

    for (IRenderFeature* feature : m_features) {
        if (feature != nullptr) {
            ZoneScopedN("IRenderFeature::beginFrame");
            const char* featureName = feature->name();
            ZoneName(featureName, std::strlen(featureName));
            feature->beginFrame();
        }
    }
}

void RenderFramePipeline::recordPreRender(VkCommandBuffer commandBuffer)
{
    if (!m_initialised.load(std::memory_order_acquire)) {
        return;
    }

    const TracyVkCtx tracyVkContext = m_context != nullptr ? m_context->tracyVkContext() : nullptr;
    for (IRenderFeature* feature : m_features) {
        if (feature != nullptr) {
            ZoneScopedN("IRenderFeature::recordPreRender");
            const char* featureName = feature->name();
            ZoneName(featureName, std::strlen(featureName));
            if (tracyVkContext != nullptr) {
                TracyVkZone(tracyVkContext, commandBuffer, "Render Feature Pre-pass");
            }
            feature->recordPreRender(commandBuffer);
        }
    }
}

void RenderFramePipeline::recordFrame(VkCommandBuffer commandBuffer)
{
    if (!m_initialised.load(std::memory_order_acquire)) {
        return;
    }

    ZoneScopedN("RenderFramePipeline::recordFrame");
    const TracyVkCtx tracyVkContext = m_context != nullptr ? m_context->tracyVkContext() : nullptr;

    if (tracyVkContext != nullptr) {
        TracyVkZone(tracyVkContext, commandBuffer, "Render Frontend");
    }

    {
        ZoneScopedN("ImGui::Render");
        ImGui::Render();
    }

    for (IRenderFeature* feature : m_features) {
        if (feature != nullptr) {
            ZoneScopedN("IRenderFeature::recordFrame");
            const char* featureName = feature->name();
            ZoneName(featureName, std::strlen(featureName));
            if (tracyVkContext != nullptr) {
                TracyVkZone(tracyVkContext, commandBuffer, "Render Feature");
            }
            feature->recordFrame(commandBuffer);
        }
    }

    {
        ZoneScopedN("ImGui Draw Data");
        if (tracyVkContext != nullptr) {
            TracyVkZone(tracyVkContext, commandBuffer, "ImGui Draw Data");
        }
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }
}

bool RenderFramePipeline::createDescriptorPool()
{
    if (m_context == nullptr) {
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 32;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = poolSize.descriptorCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(m_context->getDevice(), &poolInfo, nullptr, &m_descriptorPool) !=
        VK_SUCCESS) {
        MRD_ERROR("vkCreateDescriptorPool failed for render frame pipeline");
        return false;
    }

    return true;
}

void RenderFramePipeline::destroyDescriptorPool()
{
    if (m_context != nullptr && m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context->getDevice(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
}

void RenderFramePipeline::checkVkResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        MRD_ERROR("Vulkan backend error: {}", static_cast<int>(result));
    }
}

} // namespace Meridian
