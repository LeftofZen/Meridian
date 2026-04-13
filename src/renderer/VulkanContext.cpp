#include "renderer/VulkanContext.hpp"

#include "core/Logger.hpp"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace Meridian {

VulkanContext::VulkanContext(const VulkanContextConfig& config) : m_config(config) {}

VulkanContext::~VulkanContext()
{
    shutdown();
}

bool VulkanContext::init(SDL_Window* window)
{
    if (window == nullptr) {
        MRD_ERROR("Renderer init failed: window handle was null");
        return false;
    }

    m_windowHandle = window;

    if (volkInitialize() != VK_SUCCESS) {
        MRD_ERROR("volk: failed to find Vulkan loader (is a Vulkan driver installed?)");
        return false;
    }

    if (!createInstance()) return false;
    volkLoadInstance(m_instance);

    if (m_validationEnabled && !setupDebugMessenger()) {
        MRD_WARN("Vulkan validation layers requested but debug messenger setup failed");
    }

    if (!createSurface(window)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    volkLoadDevice(m_device);

    if (!createSwapchain(window)) return false;
    if (!createSwapchainImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommandPool()) return false;
    if (!createCommandBuffers()) return false;
    if (!createDescriptorPool()) return false;
    if (!createSyncObjects()) return false;
    if (!initImGui(window)) return false;

    MRD_INFO("Vulkan context ready (graphics queue family {}, compute {})",
        m_graphicsQueueFamily.value(),
        hasComputeSupport() ? std::to_string(m_computeQueueFamily.value()) : "n/a");
    return true;
}

void VulkanContext::shutdown()
{
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        shutdownImGui();
        destroyRenderResources();
        destroySwapchain();
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_debugMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    m_windowHandle = nullptr;
}

void VulkanContext::update(float /*deltaTimeSeconds*/)
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    if (!renderFrame()) {
        MRD_WARN("Renderer frame skipped");
    }
}

void VulkanContext::setFrameStats(
    std::span<const SystemFrameStat> frameStats,
    float frameDeltaMilliseconds,
    float frameCpuMilliseconds)
{
    m_frameStats.assign(frameStats.begin(), frameStats.end());
    m_frameDeltaMilliseconds = frameDeltaMilliseconds;
    m_frameCpuMilliseconds = frameCpuMilliseconds;
}

bool VulkanContext::createInstance()
{
    uint32_t sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts) {
        MRD_ERROR("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
        return false;
    }

    std::vector<const char*> extensions(sdlExts, sdlExts + sdlExtCount);

    m_validationEnabled = false;
    if (m_config.enableValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

        for (const auto& layer : layers) {
            if (std::strcmp(layer.layerName, k_validationLayers[0]) == 0) {
                m_validationEnabled = true;
                break;
            }
        }

        if (m_validationEnabled) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            MRD_WARN("Validation layer '{}' not available; running without validation",
                k_validationLayers[0]);
        }
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = m_config.appName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "Meridian";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (m_validationEnabled) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(k_validationLayers.size());
        createInfo.ppEnabledLayerNames = k_validationLayers.data();

        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;
        createInfo.pNext = &debugCreateInfo;
    }

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
        MRD_ERROR("vkCreateInstance failed");
        return false;
    }
    MRD_INFO("VkInstance created");
    return true;
}

bool VulkanContext::setupDebugMessenger()
{
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;

    if (vkCreateDebugUtilsMessengerEXT(m_instance, &createInfo, nullptr, &m_debugMessenger) !=
        VK_SUCCESS) {
        return false;
    }
    return true;
}

bool VulkanContext::createSurface(SDL_Window* window)
{
    if (!SDL_Vulkan_CreateSurface(window, m_instance, nullptr, &m_surface)) {
        MRD_ERROR("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return false;
    }
    MRD_INFO("VkSurfaceKHR created");
    return true;
}

bool VulkanContext::pickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        MRD_ERROR("No Vulkan-capable GPU found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (const auto& dev : devices) {
        if (!isDeviceSuitable(dev)) continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_physicalDevice = dev;
            break;
        }
        if (fallback == VK_NULL_HANDLE) fallback = dev;
    }

    if (m_physicalDevice == VK_NULL_HANDLE) m_physicalDevice = fallback;

    if (m_physicalDevice == VK_NULL_HANDLE) {
        MRD_ERROR("No suitable Vulkan GPU found");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    MRD_INFO("Physical device selected: {}", props.deviceName);
    return true;
}

bool VulkanContext::createLogicalDevice()
{
    const auto indices = findQueueFamilies(m_physicalDevice);

    std::set<uint32_t> uniqueFamilies;
    uniqueFamilies.insert(indices.graphicsFamily.value());
    uniqueFamilies.insert(indices.presentFamily.value());
    if (indices.computeFamily.has_value()) {
        uniqueFamilies.insert(indices.computeFamily.value());
    }

    const float priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueFamilies.size());
    for (const uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queueCreateInfos.push_back(qi);
    }

    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supportedFeatures);

    VkPhysicalDeviceFeatures deviceFeatures{};
    if (supportedFeatures.samplerAnisotropy == VK_TRUE) {
        deviceFeatures.samplerAnisotropy = VK_TRUE;
    } else {
        MRD_WARN("Physical device does not support sampler anisotropy; leaving it disabled");
    }

    if (supportedFeatures.fillModeNonSolid == VK_TRUE) {
        deviceFeatures.fillModeNonSolid = VK_TRUE;
    } else {
        MRD_WARN("Physical device does not support non-solid fill modes; leaving them disabled");
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(k_deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = k_deviceExtensions.data();

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
        MRD_ERROR("vkCreateDevice failed");
        return false;
    }

    m_graphicsQueueFamily = indices.graphicsFamily.value();
    m_presentQueueFamily = indices.presentFamily.value();
    m_computeQueueFamily = indices.computeFamily;

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily.value(), 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentQueueFamily.value(), 0, &m_presentQueue);
    if (m_computeQueueFamily.has_value()) {
        vkGetDeviceQueue(m_device, m_computeQueueFamily.value(), 0, &m_computeQueue);
    }

    MRD_INFO("VkDevice created");
    return true;
}

bool VulkanContext::createSwapchain(SDL_Window* window)
{
    const auto support = querySwapchainSupport(m_physicalDevice);
    const auto surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    const auto presentMode = chooseSwapPresentMode(support.presentModes);
    const auto extent = chooseSwapExtent(support.capabilities, window);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 &&
        imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    const std::array<uint32_t, 2> queueIndices = {
        m_graphicsQueueFamily.value(), m_presentQueueFamily.value()};

    if (m_graphicsQueueFamily != m_presentQueueFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueIndices.data();
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        MRD_ERROR("vkCreateSwapchainKHR failed");
        return false;
    }

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, nullptr);
    m_swapchainImages.resize(imgCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, m_swapchainImages.data());

    m_swapchainImageFormat = surfaceFormat.format;
    m_swapchainExtent = extent;
    m_minImageCount = support.capabilities.minImageCount;

    MRD_INFO("VkSwapchainKHR created ({} images, {}x{})",
        imgCount, extent.width, extent.height);
    return true;
}

bool VulkanContext::createSwapchainImageViews()
{
    m_swapchainImageViews.resize(m_swapchainImages.size());
    for (std::size_t i = 0; i < m_swapchainImages.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = m_swapchainImages[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = m_swapchainImageFormat;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &createInfo, nullptr, &m_swapchainImageViews[i]) !=
            VK_SUCCESS) {
            MRD_ERROR("vkCreateImageView failed for swapchain image {}", i);
            return false;
        }
    }
    MRD_INFO("Swapchain image views created");
    return true;
}

bool VulkanContext::createRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        MRD_ERROR("vkCreateRenderPass failed");
        return false;
    }

    return true;
}

bool VulkanContext::createFramebuffers()
{
    m_swapchainFramebuffers.resize(m_swapchainImageViews.size());

    for (std::size_t index = 0; index < m_swapchainImageViews.size(); ++index) {
        VkImageView attachment = m_swapchainImageViews[index];
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = m_swapchainExtent.width;
        framebufferInfo.height = m_swapchainExtent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(
                m_device,
                &framebufferInfo,
                nullptr,
                &m_swapchainFramebuffers[index]) != VK_SUCCESS) {
            MRD_ERROR("vkCreateFramebuffer failed for swapchain image {}", index);
            return false;
        }
    }

    return true;
}

bool VulkanContext::createCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily.value();

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        MRD_ERROR("vkCreateCommandPool failed");
        return false;
    }

    return true;
}

bool VulkanContext::createCommandBuffers()
{
    m_commandBuffers.resize(m_swapchainFramebuffers.size());

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

    if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        MRD_ERROR("vkAllocateCommandBuffers failed");
        return false;
    }

    return true;
}

bool VulkanContext::createSyncObjects()
{
    const std::size_t frameCount = m_swapchainImages.size();
    m_imageAvailableSemaphores.resize(frameCount);
    m_renderFinishedSemaphores.resize(frameCount);
    m_inFlightFences.resize(frameCount);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (std::size_t index = 0; index < frameCount; ++index) {
        if (vkCreateSemaphore(
                m_device,
                &semaphoreInfo,
                nullptr,
                &m_imageAvailableSemaphores[index]) != VK_SUCCESS ||
            vkCreateSemaphore(
                m_device,
                &semaphoreInfo,
                nullptr,
                &m_renderFinishedSemaphores[index]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[index]) != VK_SUCCESS) {
            MRD_ERROR("Failed to create renderer sync objects");
            return false;
        }
    }

    return true;
}

bool VulkanContext::createDescriptorPool()
{
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 32;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = poolSize.descriptorCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_imguiDescriptorPool) != VK_SUCCESS) {
        MRD_ERROR("vkCreateDescriptorPool failed for ImGui");
        return false;
    }

    return true;
}

bool VulkanContext::initImGui(SDL_Window* window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        MRD_ERROR("ImGui SDL3 backend init failed");
        return false;
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = m_instance;
    initInfo.PhysicalDevice = m_physicalDevice;
    initInfo.Device = m_device;
    initInfo.QueueFamily = m_graphicsQueueFamily.value();
    initInfo.Queue = m_graphicsQueue;
    initInfo.DescriptorPool = m_imguiDescriptorPool;
    initInfo.MinImageCount = m_minImageCount;
    initInfo.ImageCount = static_cast<uint32_t>(m_swapchainImages.size());
    initInfo.Allocator = nullptr;
    initInfo.PipelineInfoMain.RenderPass = m_renderPass;
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = checkVkResult;
    initInfo.MinAllocationSize = 1024 * 1024;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        MRD_ERROR("ImGui Vulkan backend init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_imguiInitialised = true;
    return true;
}

bool VulkanContext::renderFrame()
{
    if (!m_imguiInitialised || m_swapchainImages.empty()) {
        return false;
    }

    const std::size_t frameIndex = m_currentFrame % m_inFlightFences.size();
    vkWaitForFences(m_device, 1, &m_inFlightFences[frameIndex], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(
        m_device,
        m_swapchain,
        UINT64_MAX,
        m_imageAvailableSemaphores[frameIndex],
        VK_NULL_HANDLE,
        &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (acquireResult != VK_SUCCESS) {
        checkVkResult(acquireResult);
        return false;
    }

    vkResetFences(m_device, 1, &m_inFlightFences[frameIndex]);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    buildFrameStatsWindow();
    ImGui::Render();

    VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];
    vkResetCommandBuffer(commandBuffer, 0);
    if (!recordCommandBuffer(commandBuffer, imageIndex)) {
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[frameIndex];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[frameIndex];

    if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[frameIndex]) != VK_SUCCESS) {
        MRD_ERROR("vkQueueSubmit failed");
        return false;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[frameIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
        checkVkResult(presentResult);
        return false;
    }

    m_currentFrame = (frameIndex + 1) % m_inFlightFences.size();
    return true;
}

bool VulkanContext::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        MRD_ERROR("vkBeginCommandBuffer failed");
        return false;
    }

    const std::array<float, 4> clearColor{0.08F, 0.09F, 0.11F, 1.0F};
    VkClearValue clearValue{};
    std::copy(clearColor.begin(), clearColor.end(), clearValue.color.float32);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = m_swapchainExtent;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        MRD_ERROR("vkEndCommandBuffer failed");
        return false;
    }

    return true;
}

void VulkanContext::destroyRenderResources()
{
    destroySyncObjects();

    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }

    for (VkFramebuffer framebuffer : m_swapchainFramebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
    }
    m_swapchainFramebuffers.clear();
    m_commandBuffers.clear();

    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    if (m_imguiDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);
        m_imguiDescriptorPool = VK_NULL_HANDLE;
    }
}

void VulkanContext::destroySyncObjects()
{
    for (VkSemaphore semaphore : m_imageAvailableSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
    m_imageAvailableSemaphores.clear();

    for (VkSemaphore semaphore : m_renderFinishedSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
    m_renderFinishedSemaphores.clear();

    for (VkFence fence : m_inFlightFences) {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, fence, nullptr);
        }
    }
    m_inFlightFences.clear();
}

void VulkanContext::shutdownImGui()
{
    if (!m_imguiInitialised) {
        return;
    }

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_imguiInitialised = false;
}

void VulkanContext::buildFrameStatsWindow()
{
    ImGui::SetNextWindowSize(ImVec2(420.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("System Frame Times");
    ImGui::Text(
        "Frame delta: %.3f ms (%.1f FPS)",
        m_frameDeltaMilliseconds,
        m_frameDeltaMilliseconds > 0.0F ? 1000.0F / m_frameDeltaMilliseconds : 0.0F);
    ImGui::Text("CPU frame: %.3f ms", m_frameCpuMilliseconds);
    ImGui::Separator();

    if (ImGui::BeginTable(
            "system-frame-times",
            2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("System");
        ImGui::TableSetupColumn("Update (ms)");
        ImGui::TableHeadersRow();

        for (const SystemFrameStat& frameStat : m_frameStats) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(frameStat.name.data());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", frameStat.updateTimeMilliseconds);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void VulkanContext::checkVkResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        MRD_ERROR("Vulkan backend error: {}", static_cast<int>(result));
    }
}

void VulkanContext::destroySwapchain()
{
    for (auto& view : m_swapchainImageViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(m_device, view, nullptr);
    }
    m_swapchainImageViews.clear();
    m_swapchainImages.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

VulkanContext::QueueFamilyIndices VulkanContext::findQueueFamilies(
    VkPhysicalDevice device) const
{
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        const auto& family = families[i];

        if (!indices.graphicsFamily.has_value() &&
            (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
            indices.graphicsFamily = i;
        }

        if (!indices.computeFamily.has_value() &&
            (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
            indices.computeFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
        if (!indices.presentFamily.has_value() && presentSupport == VK_TRUE) {
            indices.presentFamily = i;
        }

        if (indices.isComplete()) break;
    }

    return indices;
}

bool VulkanContext::isDeviceSuitable(VkPhysicalDevice device) const
{
    const auto indices = findQueueFamilies(device);
    if (!indices.isComplete()) return false;
    if (!checkDeviceExtensionSupport(device)) return false;

    const auto support = querySwapchainSupport(device);
    return !support.formats.empty() && !support.presentModes.empty();
}

bool VulkanContext::checkDeviceExtensionSupport(VkPhysicalDevice device) const
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    for (const char* required : k_deviceExtensions) {
        bool found = false;
        for (const auto& ext : available) {
            if (std::strcmp(ext.extensionName, required) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

VulkanContext::SwapchainSupportDetails VulkanContext::querySwapchainSupport(
    VkPhysicalDevice device) const
{
    SwapchainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            device, m_surface, &formatCount, details.formats.data());
    }

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &modeCount, nullptr);
    if (modeCount != 0) {
        details.presentModes.resize(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            device, m_surface, &modeCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR VulkanContext::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats)
{
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return fmt;
        }
    }
    return formats.front();
}

VkPresentModeKHR VulkanContext::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& modes)
{
    for (const auto& mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanContext::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& caps,
    SDL_Window* window)
{
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return caps.currentExtent;
    }

    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);

    VkExtent2D extent{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    extent.width = std::clamp(
        extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(
        extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        MRD_ERROR("[Vulkan] {}", callbackData->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        MRD_WARN("[Vulkan] {}", callbackData->pMessage);
    }
    return VK_FALSE;
}

} // namespace Meridian
