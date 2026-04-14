#pragma once

#include "core/ISystem.hpp"

#include <volk.h>

#include <tracy/tracy/TracyVulkan.hpp>

#include <SDL3/SDL_vulkan.h>

#include <array>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <string>

namespace Meridian {

class IRenderFrontend;
class ShaderLibrary;

struct VulkanContextConfig {
    std::string appName{"Meridian"};
    bool enableValidation{true};
};

class VulkanContext final : public ISystem {
public:
    explicit VulkanContext(const VulkanContextConfig& config);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    [[nodiscard]] bool init(SDL_Window* windowHandle);
    void shutdown();
    void update(float deltaTimeSeconds) override;
    void render();
    void setRenderFrontend(IRenderFrontend* renderFrontend) noexcept;

    [[nodiscard]] VkInstance getInstance() const noexcept { return m_instance; }
    [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const noexcept { return m_physicalDevice; }
    [[nodiscard]] VkDevice getDevice() const noexcept { return m_device; }
    [[nodiscard]] VkSurfaceKHR getSurface() const noexcept { return m_surface; }
    [[nodiscard]] VkSwapchainKHR getSwapchain() const noexcept { return m_swapchain; }
    [[nodiscard]] VkQueue getGraphicsQueue() const noexcept { return m_graphicsQueue; }
    [[nodiscard]] VkQueue getComputeQueue() const noexcept { return m_computeQueue; }
    [[nodiscard]] VkQueue getPresentQueue() const noexcept { return m_presentQueue; }
    [[nodiscard]] VkFormat getSwapchainFormat() const noexcept { return m_swapchainImageFormat; }
    [[nodiscard]] VkExtent2D getSwapchainExtent() const noexcept { return m_swapchainExtent; }
    [[nodiscard]] VkRenderPass getRenderPass() const noexcept { return m_renderPass; }
    [[nodiscard]] uint32_t getMinImageCount() const noexcept { return m_minImageCount; }
    [[nodiscard]] std::size_t getSwapchainImageCount() const noexcept { return m_swapchainImages.size(); }
    [[nodiscard]] const std::vector<VkImageView>& getSwapchainImageViews() const noexcept
    {
        return m_swapchainImageViews;
    }
    [[nodiscard]] bool hasComputeSupport() const noexcept { return m_computeQueueFamily.has_value(); }
    [[nodiscard]] ShaderLibrary& getShaderLibrary() noexcept { return *m_shaderLibrary; }
    [[nodiscard]] uint32_t getGraphicsQueueFamily() const noexcept
    {
        return m_graphicsQueueFamily.value();
    }
    [[nodiscard]] uint32_t getComputeQueueFamily() const noexcept
    {
        return m_computeQueueFamily.value_or(m_graphicsQueueFamily.value());
    }
    [[nodiscard]] bool isVSyncEnabled() const noexcept { return m_vsyncEnabled; }
    [[nodiscard]] std::mutex& getQueueSubmitMutex() noexcept { return m_queueSubmitMutex; }
    [[nodiscard]] std::size_t getFramesInFlightCount() const noexcept { return m_inFlightFences.size(); }
    [[nodiscard]] std::size_t getCurrentFrameSlot() const noexcept
    {
        return m_inFlightFences.empty() ? 0 : (m_currentFrame % m_inFlightFences.size());
    }
    [[nodiscard]] TracyVkCtx tracyVkContext() const noexcept { return m_tracyVkContext; }
    [[nodiscard]] bool supportsFragmentShadingRate() const noexcept
    {
        return m_fragmentShadingRateSupported;
    }
    [[nodiscard]] std::span<const std::uint32_t> supportedFragmentShadingRates() const noexcept
    {
        return m_supportedFragmentShadingRates;
    }
    [[nodiscard]] std::uint32_t fragmentShadingRateTexelSize() const noexcept
    {
        return m_fragmentShadingRateTexelSize;
    }
    void setVSyncEnabled(bool enabled);
    [[nodiscard]] const char* getPresentModeName() const noexcept;
    void setObjectDebugName(
        std::uint64_t objectHandle,
        VkObjectType objectType,
        std::string_view name) const noexcept;
    void setFragmentShadingRateTexelSize(std::uint32_t texelSize) noexcept;
    void applyFragmentShadingRate(
        VkCommandBuffer commandBuffer,
        std::uint32_t texelSize) const noexcept;

private:
    [[nodiscard]] bool createInstance();
    [[nodiscard]] bool setupDebugMessenger();
    [[nodiscard]] bool createSurface(SDL_Window* window);
    [[nodiscard]] bool pickPhysicalDevice();
    [[nodiscard]] bool createLogicalDevice();
    [[nodiscard]] bool createSwapchain(SDL_Window* window);
    [[nodiscard]] bool createSwapchainImageViews();
    [[nodiscard]] bool createRenderPass();
    [[nodiscard]] bool createFramebuffers();
    [[nodiscard]] bool createCommandPool();
    [[nodiscard]] bool createCommandBuffers();
    [[nodiscard]] bool createSyncObjects();
    [[nodiscard]] bool createTracyContext();
    [[nodiscard]] bool renderFrame();
    [[nodiscard]] bool recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void initialiseFragmentShadingRateSupport() noexcept;
    void destroyTracyContext() noexcept;

    void destroySwapchain();
    void destroyRenderResources();
    void destroySyncObjects();
    [[nodiscard]] bool recreatePresentationResources();
    void requestPresentationRebuild();
    [[nodiscard]] const char* presentModeName() const noexcept;

    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;
        std::optional<uint32_t> computeFamily;

        [[nodiscard]] bool isComplete() const noexcept
        {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    [[nodiscard]] bool isDeviceSuitable(VkPhysicalDevice device) const;
    [[nodiscard]] bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;

    struct SwapchainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    [[nodiscard]] SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;
    [[nodiscard]] static VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& formats);
    [[nodiscard]] static VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& modes,
        bool vsyncEnabled);
    [[nodiscard]] static VkExtent2D chooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& caps,
        SDL_Window* window);

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* userData);

    VulkanContextConfig m_config;

    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};

    VkFormat m_swapchainImageFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D m_swapchainExtent{};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkFramebuffer> m_swapchainFramebuffers;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    std::vector<VkFence> m_imagesInFlight;
    TracyVkCtx m_tracyVkContext{nullptr};

    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    VkQueue m_computeQueue{VK_NULL_HANDLE};
    VkQueue m_presentQueue{VK_NULL_HANDLE};
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};

    std::optional<uint32_t> m_graphicsQueueFamily;
    std::optional<uint32_t> m_computeQueueFamily;
    std::optional<uint32_t> m_presentQueueFamily;
    bool m_validationEnabled{false};
    bool m_debugUtilsEnabled{false};
    bool m_fragmentShadingRateSupported{false};
    SDL_Window* m_windowHandle{nullptr};
    std::size_t m_currentFrame{0};
    bool m_vsyncEnabled{false};
    bool m_presentationRebuildRequested{false};
    uint32_t m_minImageCount{2};
    VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_FIFO_KHR};
    std::vector<std::uint32_t> m_supportedFragmentShadingRates{1U};
    std::uint32_t m_fragmentShadingRateTexelSize{1U};
    std::mutex m_queueSubmitMutex;
    IRenderFrontend* m_renderFrontend{nullptr};
    std::unique_ptr<ShaderLibrary> m_shaderLibrary;

    static constexpr std::array<const char*, 1> k_validationLayers{
        "VK_LAYER_KHRONOS_validation"};
    static constexpr std::array<const char*, 1> k_requiredDeviceExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};
};

} // namespace Meridian
