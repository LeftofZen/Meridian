#include "renderer/RenderFramePipeline.hpp"

#include "core/Logger.hpp"
#include "renderer/VulkanContext.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <chrono>

namespace Meridian {

namespace {

[[nodiscard]] float elapsedMilliseconds(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) noexcept
{
    return std::chrono::duration<float, std::milli>(end - start).count();
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

    m_frameConfig = RenderFrameConfig{};
    m_frameProfilingData.reset(m_features.size());

    std::vector<SDL_Event> pendingEvents;
    {
        std::scoped_lock lock(m_eventMutex);
        pendingEvents.swap(m_pendingEvents);
    }

    auto phaseStart = std::chrono::steady_clock::now();
    for (const SDL_Event& event : pendingEvents) {
        SDL_Event mutableEvent = event;
        ImGui_ImplSDL3_ProcessEvent(&mutableEvent);

        for (IRenderFeature* feature : m_features) {
            if (feature != nullptr) {
                feature->handleEvent(event);
            }
        }
    }
    auto phaseEnd = std::chrono::steady_clock::now();
    m_frameProfilingData.eventProcessingMilliseconds = elapsedMilliseconds(phaseStart, phaseEnd);

    phaseStart = std::chrono::steady_clock::now();
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    phaseEnd = std::chrono::steady_clock::now();
    m_frameProfilingData.imguiNewFrameMilliseconds = elapsedMilliseconds(phaseStart, phaseEnd);

    for (std::size_t index = 0; index < m_features.size(); ++index) {
        IRenderFeature* feature = m_features[index];
        if (feature != nullptr) {
            m_frameProfilingData.featureSamples[index].name = feature->profileName();
            phaseStart = std::chrono::steady_clock::now();
            feature->configureFrame(m_frameConfig);
            phaseEnd = std::chrono::steady_clock::now();
            const float elapsed = elapsedMilliseconds(phaseStart, phaseEnd);
            m_frameProfilingData.featureSamples[index].configureTimeMilliseconds = elapsed;
            m_frameProfilingData.configureTotalMilliseconds += elapsed;
        }
    }

    for (std::size_t index = 0; index < m_features.size(); ++index) {
        IRenderFeature* feature = m_features[index];
        if (feature != nullptr) {
            phaseStart = std::chrono::steady_clock::now();
            feature->beginFrame();
            phaseEnd = std::chrono::steady_clock::now();
            const float elapsed = elapsedMilliseconds(phaseStart, phaseEnd);
            m_frameProfilingData.featureSamples[index].beginTimeMilliseconds = elapsed;
            m_frameProfilingData.beginTotalMilliseconds += elapsed;
        }
    }
}

void RenderFramePipeline::recordFrame(VkCommandBuffer commandBuffer)
{
    if (!m_initialised.load(std::memory_order_acquire)) {
        return;
    }

    auto phaseStart = std::chrono::steady_clock::now();
    ImGui::Render();
    auto phaseEnd = std::chrono::steady_clock::now();
    m_frameProfilingData.imguiRenderMilliseconds = elapsedMilliseconds(phaseStart, phaseEnd);

    for (std::size_t index = 0; index < m_features.size(); ++index) {
        IRenderFeature* feature = m_features[index];
        if (feature != nullptr) {
            phaseStart = std::chrono::steady_clock::now();
            feature->recordFrame(commandBuffer);
            phaseEnd = std::chrono::steady_clock::now();
            const float elapsed = elapsedMilliseconds(phaseStart, phaseEnd);
            m_frameProfilingData.featureSamples[index].recordTimeMilliseconds = elapsed;
            m_frameProfilingData.recordTotalMilliseconds += elapsed;
        }
    }

    phaseStart = std::chrono::steady_clock::now();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    phaseEnd = std::chrono::steady_clock::now();
    m_frameProfilingData.imguiDrawMilliseconds = elapsedMilliseconds(phaseStart, phaseEnd);

    if (m_renderStateStore != nullptr) {
        const std::array<RenderPhaseTimingSample, 7> renderPhaseSamples{{
            {"Event Processing", m_frameProfilingData.eventProcessingMilliseconds},
            {"ImGui New Frame", m_frameProfilingData.imguiNewFrameMilliseconds},
            {"Feature Configure", m_frameProfilingData.configureTotalMilliseconds},
            {"Feature Begin", m_frameProfilingData.beginTotalMilliseconds},
            {"ImGui Render", m_frameProfilingData.imguiRenderMilliseconds},
            {"Feature Record", m_frameProfilingData.recordTotalMilliseconds},
            {"ImGui Draw Data", m_frameProfilingData.imguiDrawMilliseconds},
        }};
        m_renderStateStore->updateRenderFrontendStats(
            renderPhaseSamples,
            m_frameProfilingData.featureSamples);
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
