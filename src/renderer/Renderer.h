#pragma once

// Renderer.h — Frame rendering orchestrator

#include <vulkan/vulkan.h>
#include <vector>

namespace Talos {

class VulkanContext;
class Swapchain;
class Pipeline;
class Mesh;
class Descriptors;
class Registry;

class Renderer {
public:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    void init(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline);
    void cleanup(VkDevice device);

    bool drawFrame(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline,
                    Registry& registry, Descriptors& descriptors);

    void recreateFramebuffers(VkDevice device, Swapchain& swapchain, Pipeline& pipeline);

    void waitIdle(VkDevice device);

    uint32_t getCurrentFrame() const { return m_currentFrame; }

private:
    VkCommandPool                m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkFramebuffer>   m_framebuffers;

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkFence>     m_inFlightFences;

    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence>     m_imagesInFlight;

    uint32_t m_currentFrame = 0;

    void createCommandPool(VulkanContext& context);
    void createCommandBuffers(VkDevice device);
    void createFramebuffers(VkDevice device, Swapchain& swapchain, Pipeline& pipeline);
    void createSyncObjects(VkDevice device, uint32_t imageCount);

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                             Swapchain& swapchain, Pipeline& pipeline,
                             Registry& registry, Descriptors& descriptors);
};

} // namespace Talos
