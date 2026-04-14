#include "renderer/VulkanContext.hpp"

#include "renderer/IRenderFrontend.hpp"
#include "renderer/ShaderLibrary.hpp"

#include "core/Logger.hpp"

#include <SDL3/SDL.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace Meridian {

namespace {

void logVkResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        MRD_ERROR("Vulkan backend error: {}", static_cast<int>(result));
    }
}

[[nodiscard]] bool hasExtension(
    std::span<const VkExtensionProperties> extensions,
    const char* extensionName) noexcept
{
    return std::ranges::any_of(extensions, [extensionName](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, extensionName) == 0;
    });
}

} // namespace

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
    initialiseFragmentShadingRateSupport();
    m_shaderLibrary = std::make_unique<ShaderLibrary>(m_device);

    if (!createSwapchain(window)) return false;
    if (!createSwapchainImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommandPool()) return false;
    if (!createTracyContext()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    if (m_renderFrontend != nullptr && !m_renderFrontend->init(window, *this)) {
        MRD_ERROR("Render frontend init failed");
        return false;
    }

    MRD_INFO("Vulkan context ready (graphics queue family {}, compute {})",
        m_graphicsQueueFamily.value(),
        hasComputeSupport() ? std::to_string(m_computeQueueFamily.value()) : "n/a");
    return true;
}

void VulkanContext::shutdown()
{
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        if (m_renderFrontend != nullptr) {
            m_renderFrontend->shutdown();
        }
        destroyTracyContext();
        m_shaderLibrary.reset();
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

void VulkanContext::setRenderFrontend(IRenderFrontend* renderFrontend) noexcept
{
    m_renderFrontend = renderFrontend;
}

void VulkanContext::setVSyncEnabled(bool enabled)
{
    if (m_vsyncEnabled == enabled) {
        return;
    }

    m_vsyncEnabled = enabled;
    requestPresentationRebuild();
}

const char* VulkanContext::getPresentModeName() const noexcept
{
    return presentModeName();
}

void VulkanContext::update(float /*deltaTimeSeconds*/)
{
    render();
}

void VulkanContext::render()
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    if (m_presentationRebuildRequested) {
        if (!recreatePresentationResources()) {
            MRD_WARN("Renderer presentation rebuild failed");
            return;
        }
    }

    if (!renderFrame()) {
        MRD_WARN("Renderer frame skipped");
    }
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

    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateInstanceExtensionProperties(
        nullptr,
        &extensionCount,
        availableExtensions.data());

    m_debugUtilsEnabled = hasExtension(
        availableExtensions,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (m_debugUtilsEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

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

        if (!m_validationEnabled) {
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

    uint32_t availableExtensionCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &availableExtensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(availableExtensionCount);
    vkEnumerateDeviceExtensionProperties(
        m_physicalDevice,
        nullptr,
        &availableExtensionCount,
        availableExtensions.data());

    std::vector<const char*> deviceExtensions(
        k_requiredDeviceExtensions.begin(),
        k_requiredDeviceExtensions.end());

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

    VkPhysicalDeviceFragmentShadingRateFeaturesKHR supportedFragmentShadingRateFeatures{};
    supportedFragmentShadingRateFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 supportedFeatures2{};
    supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures2.pNext = &supportedFragmentShadingRateFeatures;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &supportedFeatures2);

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

    VkPhysicalDeviceFragmentShadingRateFeaturesKHR enabledFragmentShadingRateFeatures{};
    enabledFragmentShadingRateFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
    if (hasExtension(availableExtensions, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) &&
        supportedFragmentShadingRateFeatures.pipelineFragmentShadingRate == VK_TRUE) {
        deviceExtensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        enabledFragmentShadingRateFeatures.pipelineFragmentShadingRate = VK_TRUE;
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();
    if (enabledFragmentShadingRateFeatures.pipelineFragmentShadingRate == VK_TRUE) {
        createInfo.pNext = &enabledFragmentShadingRateFeatures;
    }

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
        MRD_ERROR("vkCreateDevice failed");
        return false;
    }

    m_fragmentShadingRateSupported =
        enabledFragmentShadingRateFeatures.pipelineFragmentShadingRate == VK_TRUE;
    m_supportedFragmentShadingRates = {1U};
    m_fragmentShadingRateTexelSize = 1U;

    m_graphicsQueueFamily = indices.graphicsFamily.value();
    m_presentQueueFamily = indices.presentFamily.value();
    m_computeQueueFamily = indices.computeFamily;

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily.value(), 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentQueueFamily.value(), 0, &m_presentQueue);
    if (m_computeQueueFamily.has_value()) {
        vkGetDeviceQueue(m_device, m_computeQueueFamily.value(), 0, &m_computeQueue);
    }

    setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_graphicsQueue),
        VK_OBJECT_TYPE_QUEUE,
        "Graphics Queue");
    if (m_presentQueue != m_graphicsQueue) {
        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_presentQueue),
            VK_OBJECT_TYPE_QUEUE,
            "Present Queue");
    }
    if (m_computeQueue != VK_NULL_HANDLE && m_computeQueue != m_graphicsQueue) {
        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_computeQueue),
            VK_OBJECT_TYPE_QUEUE,
            "Compute Queue");
    }

    MRD_INFO("VkDevice created");
    return true;
}

void VulkanContext::initialiseFragmentShadingRateSupport() noexcept
{
    m_supportedFragmentShadingRates = {1U};
    m_fragmentShadingRateTexelSize = 1U;

    if (!m_fragmentShadingRateSupported || vkGetPhysicalDeviceFragmentShadingRatesKHR == nullptr ||
        vkCmdSetFragmentShadingRateKHR == nullptr) {
        m_fragmentShadingRateSupported = false;
        return;
    }

    uint32_t rateCount = 0;
    if (vkGetPhysicalDeviceFragmentShadingRatesKHR(m_physicalDevice, &rateCount, nullptr) != VK_SUCCESS ||
        rateCount == 0) {
        m_fragmentShadingRateSupported = false;
        return;
    }

    std::vector<VkPhysicalDeviceFragmentShadingRateKHR> shadingRates(rateCount);
    for (VkPhysicalDeviceFragmentShadingRateKHR& shadingRate : shadingRates) {
        shadingRate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
    }

    if (vkGetPhysicalDeviceFragmentShadingRatesKHR(
            m_physicalDevice,
            &rateCount,
            shadingRates.data()) != VK_SUCCESS) {
        m_fragmentShadingRateSupported = false;
        return;
    }

    std::vector<std::uint32_t> supportedRates{1U};
    for (const std::uint32_t candidate : {2U, 4U}) {
        const bool isSupported = std::ranges::any_of(
            shadingRates,
            [candidate](const VkPhysicalDeviceFragmentShadingRateKHR& shadingRate) {
                return shadingRate.fragmentSize.width == candidate &&
                    shadingRate.fragmentSize.height == candidate &&
                    (shadingRate.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0U;
            });
        if (isSupported) {
            supportedRates.push_back(candidate);
        }
    }

    m_supportedFragmentShadingRates = std::move(supportedRates);
    if (std::ranges::find(m_supportedFragmentShadingRates, 2U) != m_supportedFragmentShadingRates.end()) {
        m_fragmentShadingRateTexelSize = 2U;
    }
    MRD_INFO(
        "Fragment shading rate support: {}",
        m_supportedFragmentShadingRates.size() > 1 ? "enabled" : "1x1 only");
}

bool VulkanContext::createSwapchain(SDL_Window* window)
{
    const auto support = querySwapchainSupport(m_physicalDevice);
    const auto surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    const auto presentMode = chooseSwapPresentMode(support.presentModes, m_vsyncEnabled);
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

    setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_swapchain),
        VK_OBJECT_TYPE_SWAPCHAIN_KHR,
        "Main Swapchain");
    for (std::size_t index = 0; index < m_swapchainImages.size(); ++index) {
        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_swapchainImages[index]),
            VK_OBJECT_TYPE_IMAGE,
            std::format("Swapchain Image {}", index));
    }

    m_swapchainImageFormat = surfaceFormat.format;
    m_swapchainExtent = extent;
    m_minImageCount = support.capabilities.minImageCount;
    m_presentMode = presentMode;

    MRD_INFO("VkSwapchainKHR created ({} images, {}x{}, present mode {})",
        imgCount,
        extent.width,
        extent.height,
        presentModeName());
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

        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_swapchainImageViews[i]),
            VK_OBJECT_TYPE_IMAGE_VIEW,
            std::format("Swapchain Image View {}", i));
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

    setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_renderPass),
        VK_OBJECT_TYPE_RENDER_PASS,
        "Main Render Pass");

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

        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_swapchainFramebuffers[index]),
            VK_OBJECT_TYPE_FRAMEBUFFER,
            std::format("Swapchain Framebuffer {}", index));
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

    setObjectDebugName(
        reinterpret_cast<std::uint64_t>(m_commandPool),
        VK_OBJECT_TYPE_COMMAND_POOL,
        "Graphics Command Pool");

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

    for (std::size_t index = 0; index < m_commandBuffers.size(); ++index) {
        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_commandBuffers[index]),
            VK_OBJECT_TYPE_COMMAND_BUFFER,
            std::format("Frame Command Buffer {}", index));
    }

    return true;
}

bool VulkanContext::createTracyContext()
{
    if (m_device == VK_NULL_HANDLE || m_commandPool == VK_NULL_HANDLE) {
        return false;
    }

    destroyTracyContext();

    VkCommandBuffer tracyCommandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &allocInfo, &tracyCommandBuffer) != VK_SUCCESS) {
        MRD_ERROR("Failed to allocate Tracy Vulkan setup command buffer");
        return false;
    }

    m_tracyVkContext = TracyVkContext(m_physicalDevice, m_device, m_graphicsQueue, tracyCommandBuffer);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &tracyCommandBuffer);

    if (m_tracyVkContext == nullptr) {
        MRD_ERROR("Failed to create Tracy Vulkan profiling context");
        return false;
    }

    static constexpr char kGraphicsQueueName[] = "Graphics Queue";
    TracyVkContextName(m_tracyVkContext, kGraphicsQueueName, sizeof(kGraphicsQueueName) - 1);
    return true;
}

void VulkanContext::destroyTracyContext() noexcept
{
    if (m_tracyVkContext != nullptr) {
        TracyVkDestroy(m_tracyVkContext);
        m_tracyVkContext = nullptr;
    }
}

bool VulkanContext::createSyncObjects()
{
    const std::size_t frameCount = std::max<std::size_t>(
        1,
        std::min<std::size_t>(2, m_swapchainImages.size()));
    m_imageAvailableSemaphores.resize(frameCount);
    m_renderFinishedSemaphores.resize(m_swapchainImages.size());
    m_inFlightFences.resize(frameCount);
    m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);

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
            vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[index]) != VK_SUCCESS) {
            MRD_ERROR("Failed to create renderer sync objects");
            return false;
        }

        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_imageAvailableSemaphores[index]),
            VK_OBJECT_TYPE_SEMAPHORE,
            std::format("Image Available Semaphore {}", index));
        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_inFlightFences[index]),
            VK_OBJECT_TYPE_FENCE,
            std::format("Frame Fence {}", index));
    }

    for (std::size_t index = 0; index < m_renderFinishedSemaphores.size(); ++index) {
        if (vkCreateSemaphore(
                m_device,
                &semaphoreInfo,
                nullptr,
                &m_renderFinishedSemaphores[index]) != VK_SUCCESS) {
            MRD_ERROR("Failed to create renderer sync objects");
            return false;
        }

        setObjectDebugName(
            reinterpret_cast<std::uint64_t>(m_renderFinishedSemaphores[index]),
            VK_OBJECT_TYPE_SEMAPHORE,
            std::format("Render Finished Semaphore {}", index));
    }

    return true;
}

bool VulkanContext::renderFrame()
{
    if (m_swapchainImages.empty()) {
        return false;
    }

    ZoneScopedN("VulkanContext::renderFrame");

    const std::size_t frameIndex = m_currentFrame % m_inFlightFences.size();
    {
        ZoneScopedN("Wait For Frame Fence");
        vkWaitForFences(m_device, 1, &m_inFlightFences[frameIndex], VK_TRUE, UINT64_MAX);
    }

    uint32_t imageIndex = 0;
    const VkResult acquireResult = [&]() {
        ZoneScopedN("Acquire Swapchain Image");
        return vkAcquireNextImageKHR(
            m_device,
            m_swapchain,
            UINT64_MAX,
            m_imageAvailableSemaphores[frameIndex],
            VK_NULL_HANDLE,
            &imageIndex);
    }();

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
        requestPresentationRebuild();
        return false;
    }
    if (acquireResult != VK_SUCCESS) {
        logVkResult(acquireResult);
        return false;
    }

    if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        {
            ZoneScopedN("Wait For Image Fence");
            vkWaitForFences(m_device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
    }

    m_imagesInFlight[imageIndex] = m_inFlightFences[frameIndex];

    vkResetFences(m_device, 1, &m_inFlightFences[frameIndex]);

    if (m_renderFrontend != nullptr) {
        {
            ZoneScopedN("Render Frontend Begin");
            m_renderFrontend->beginFrame();
        }
    }

    VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];
    {
        ZoneScopedN("Record Command Buffer");
        vkResetCommandBuffer(commandBuffer, 0);
        if (!recordCommandBuffer(commandBuffer, imageIndex)) {
            return false;
        }
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
    submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[imageIndex];

    {
        ZoneScopedN("Submit And Present");
        {
            ZoneScopedN("vkQueueSubmit");
            std::scoped_lock lock(m_queueSubmitMutex);
            if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[frameIndex]) != VK_SUCCESS) {
                MRD_ERROR("vkQueueSubmit failed");
                return false;
            }
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &imageIndex;

        VkResult presentResult = VK_SUCCESS;
        {
            ZoneScopedN("vkQueuePresentKHR");
            std::scoped_lock lock(m_queueSubmitMutex);
            presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
        }
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            requestPresentationRebuild();
            return false;
        }
        if (presentResult != VK_SUCCESS) {
            logVkResult(presentResult);
            return false;
        }
    }

    m_currentFrame = (frameIndex + 1) % m_inFlightFences.size();
    return true;
}

bool VulkanContext::recreatePresentationResources()
{
    if (m_device == VK_NULL_HANDLE || m_windowHandle == nullptr) {
        return false;
    }

    vkDeviceWaitIdle(m_device);

    if (m_renderFrontend != nullptr) {
        m_renderFrontend->shutdown();
    }
    destroyTracyContext();
    destroyRenderResources();
    destroySwapchain();

    if (!createSwapchain(m_windowHandle)) return false;
    if (!createSwapchainImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommandPool()) return false;
    if (!createTracyContext()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    if (m_renderFrontend != nullptr && !m_renderFrontend->init(m_windowHandle, *this)) {
        MRD_ERROR("Render frontend reinit failed");
        return false;
    }

    m_currentFrame = 0;
    m_presentationRebuildRequested = false;
    m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
    return true;
}

void VulkanContext::requestPresentationRebuild()
{
    m_presentationRebuildRequested = true;
}

void VulkanContext::setObjectDebugName(
    std::uint64_t objectHandle,
    VkObjectType objectType,
    std::string_view name) const noexcept
{
    if (!m_debugUtilsEnabled || m_device == VK_NULL_HANDLE || objectHandle == 0U || name.empty() ||
        vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }

    VkDebugUtilsObjectNameInfoEXT nameInfo{};
    nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    nameInfo.objectType = objectType;
    nameInfo.objectHandle = objectHandle;
    nameInfo.pObjectName = name.data();
    vkSetDebugUtilsObjectNameEXT(m_device, &nameInfo);
}

void VulkanContext::setFragmentShadingRateTexelSize(std::uint32_t texelSize) noexcept
{
    const auto supportedRate = std::find(
        m_supportedFragmentShadingRates.begin(),
        m_supportedFragmentShadingRates.end(),
        texelSize);
    m_fragmentShadingRateTexelSize = supportedRate != m_supportedFragmentShadingRates.end()
        ? *supportedRate
        : 1U;
}

void VulkanContext::applyFragmentShadingRate(
    VkCommandBuffer commandBuffer,
    std::uint32_t texelSize) const noexcept
{
    if (!m_fragmentShadingRateSupported || commandBuffer == VK_NULL_HANDLE ||
        vkCmdSetFragmentShadingRateKHR == nullptr) {
        return;
    }

    VkExtent2D fragmentSize{
        .width = texelSize,
        .height = texelSize,
    };
    const VkFragmentShadingRateCombinerOpKHR combinerOps[2] = {
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
    };
    vkCmdSetFragmentShadingRateKHR(commandBuffer, &fragmentSize, combinerOps);
}

bool VulkanContext::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        MRD_ERROR("vkBeginCommandBuffer failed");
        return false;
    }

    std::array<float, 4> clearColor{0.08F, 0.09F, 0.11F, 1.0F};
    if (m_renderFrontend != nullptr) {
        clearColor = m_renderFrontend->getFrameConfig().clearColor;
    }
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

    {
        if (m_tracyVkContext != nullptr) {
            TracyVkZone(m_tracyVkContext, commandBuffer, "Swapchain Render Pass");
        }

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        if (m_renderFrontend != nullptr) {
            m_renderFrontend->recordFrame(commandBuffer);
        }
        vkCmdEndRenderPass(commandBuffer);
    }

    if (m_tracyVkContext != nullptr) {
        TracyVkCollect(m_tracyVkContext, commandBuffer);
    }

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
    m_imagesInFlight.clear();
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

const char* VulkanContext::presentModeName() const noexcept
{
    switch (m_presentMode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "Immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "Mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO Relaxed";
    default:
        return "Other";
    }
}

VulkanContext::QueueFamilyIndices VulkanContext::findQueueFamilies(
    VkPhysicalDevice device) const
{
    QueueFamilyIndices indices;
    std::optional<uint32_t> dedicatedComputeFamily;

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

        if (!dedicatedComputeFamily.has_value() &&
            (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U &&
            (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
            dedicatedComputeFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
        if (!indices.presentFamily.has_value() && presentSupport == VK_TRUE) {
            indices.presentFamily = i;
        }

    }

    if (dedicatedComputeFamily.has_value()) {
        indices.computeFamily = dedicatedComputeFamily;
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

    for (const char* required : k_requiredDeviceExtensions) {
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
    const std::vector<VkPresentModeKHR>& modes,
    bool vsyncEnabled)
{
    if (vsyncEnabled) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    if (!vsyncEnabled) {
        for (const auto& mode : modes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return mode;
        }
        for (const auto& mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
        }
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
