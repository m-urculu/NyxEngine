#pragma once

// VulkanContext.h — Core Vulkan setup
//
// This class handles ALL the Vulkan initialization boilerplate:
// 1. Create a Vulkan Instance (the connection between your app and the Vulkan library)
// 2. Setup debug messenger (catches Vulkan errors in debug mode)
// 3. Create a window surface (connects Vulkan to the GLFW window)
// 4. Pick the best physical GPU
// 5. Create a logical device (your app's interface to the GPU)
// 6. Get queue handles (for submitting commands to the GPU)
//
// Think of this as the "foundation" — everything else in Vulkan builds on top of these.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <optional>

// Forward declaration — we don't need to include the full GLFW header here
struct GLFWwindow;

namespace Nyx {

// Stores the queue family indices we need.
// A "queue family" is a group of GPU queues that support certain operations.
struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;  // Supports drawing commands
    std::optional<uint32_t> presentFamily;   // Supports presenting to the screen

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

// Stores what the swapchain can do on this device.
// We query this before creating the swapchain.
struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities;
    std::vector<VkSurfaceFormatKHR> formats;       // Color formats (e.g., BGRA8)
    std::vector<VkPresentModeKHR>   presentModes;  // V-Sync modes
};

class VulkanContext {
public:
    void init(GLFWwindow* window);
    void cleanup();

    // ── Accessors ──────────────────────────────────────────────────────────
    VkInstance       getInstance()       const { return m_instance; }
    VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
    VkDevice         getDevice()         const { return m_device; }
    VkSurfaceKHR     getSurface()        const { return m_surface; }
    VkQueue          getGraphicsQueue()  const { return m_graphicsQueue; }
    VkQueue          getPresentQueue()   const { return m_presentQueue; }
    VmaAllocator     getAllocator()      const { return m_allocator; }
    VkCommandPool    getCommandPool()    const { return m_commandPool; }

    QueueFamilyIndices    findQueueFamilies(VkPhysicalDevice device) const;
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;

    // Single-time command buffer helpers (for staging uploads, layout transitions, etc.)
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);

private:
    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VkQueue                  m_graphicsQueue  = VK_NULL_HANDLE;
    VkQueue                  m_presentQueue   = VK_NULL_HANDLE;
    VmaAllocator             m_allocator      = VK_NULL_HANDLE;
    VkCommandPool            m_commandPool    = VK_NULL_HANDLE;

    // ── Setup steps (called by init) ───────────────────────────────────────
    void createInstance();
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createCommandPool();

    // ── Helpers ────────────────────────────────────────────────────────────
    bool checkValidationLayerSupport() const;
    std::vector<const char*> getRequiredExtensions() const;
    bool isDeviceSuitable(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;

    // Vulkan debug callback — called when Vulkan has a validation error
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT             messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void*                                        pUserData
    );
};

} // namespace Nyx
