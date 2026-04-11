#pragma once

#include <volk.h>

#include <SDL3/SDL_vulkan.h>

#include <array>
#include <optional>
#include <vector>

#include <string>

namespace Meridian {

struct VulkanContextConfig {
    std::string appName{"Meridian"};
    bool enableValidation{true};
};

class VulkanContext {
public:
    explicit VulkanContext(const VulkanContextConfig& config);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    [[nodiscard]] bool init(SDL_Window* windowHandle);
    void shutdown();

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
    [[nodiscard]] const std::vector<VkImageView>& getSwapchainImageViews() const noexcept
    {
        return m_swapchainImageViews;
    }
    [[nodiscard]] bool hasComputeSupport() const noexcept { return m_computeQueueFamily.has_value(); }
    [[nodiscard]] uint32_t getGraphicsQueueFamily() const noexcept
    {
        return m_graphicsQueueFamily.value();
    }

private:
    [[nodiscard]] bool createInstance();
    [[nodiscard]] bool setupDebugMessenger();
    [[nodiscard]] bool createSurface(SDL_Window* window);
    [[nodiscard]] bool pickPhysicalDevice();
    [[nodiscard]] bool createLogicalDevice();
    [[nodiscard]] bool createSwapchain(SDL_Window* window);
    [[nodiscard]] bool createSwapchainImageViews();

    void destroySwapchain();

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
        const std::vector<VkPresentModeKHR>& modes);
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

    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    VkQueue m_computeQueue{VK_NULL_HANDLE};
    VkQueue m_presentQueue{VK_NULL_HANDLE};

    std::optional<uint32_t> m_graphicsQueueFamily;
    std::optional<uint32_t> m_computeQueueFamily;
    std::optional<uint32_t> m_presentQueueFamily;
    bool m_validationEnabled{false};

    static constexpr std::array<const char*, 1> k_validationLayers{
        "VK_LAYER_KHRONOS_validation"};
    static constexpr std::array<const char*, 1> k_deviceExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};
};

} // namespace Meridian
