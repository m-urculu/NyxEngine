#pragma once

// Renderer.h — Frame rendering orchestrator
//
// This class ties together all the rendering pieces:
// - Creates framebuffers (one per swapchain image)
// - Manages command buffers (GPU command lists)
// - Handles synchronization (semaphores and fences)
// - Implements the frame rendering loop:
//   1. Wait for previous frame to finish
//   2. Acquire next swapchain image
//   3. Record draw commands
//   4. Submit commands to GPU
//   5. Present the rendered image
//
// "Frames in flight" means we can prepare frame N+1 while the GPU
// renders frame N. This overlapping keeps both CPU and GPU busy.

#include <vulkan/vulkan.h>
#include <vector>

namespace VulkanEngine {

class VulkanContext;
class Swapchain;
class Pipeline;

class Renderer {
public:
    // Max frames being processed simultaneously
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    void init(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline);
    void cleanup(VkDevice device);

    // Draw one frame. Returns false if swapchain needs recreation.
    bool drawFrame(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline);

    // Recreate framebuffers (after swapchain resize)
    void recreateFramebuffers(VkDevice device, Swapchain& swapchain, Pipeline& pipeline);

    // Wait for all GPU work to finish (used before cleanup)
    void waitIdle(VkDevice device);

private:
    VkCommandPool                m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkFramebuffer>   m_framebuffers;

    // ── Synchronization objects ───────────────────────────────────────────
    // Per frame-in-flight (indexed by m_currentFrame):
    std::vector<VkSemaphore> m_imageAvailableSemaphores;  // Signaled when swapchain image is ready
    std::vector<VkFence>     m_inFlightFences;            // CPU-GPU sync (wait for frame to finish)

    // Per swapchain image (indexed by imageIndex from vkAcquireNextImageKHR):
    // The present engine holds a reference to the renderFinished semaphore until
    // the image is re-acquired. With mailbox mode, the swapchain may have more
    // images (e.g., 5) than frames-in-flight (2), so we need one semaphore per
    // image to avoid reusing a semaphore still held by the present engine.
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence>     m_imagesInFlight;  // Maps image → fence of frame using it

    uint32_t m_currentFrame = 0;  // Which frame-in-flight we're on (0 or 1)

    void createCommandPool(VulkanContext& context);
    void createCommandBuffers(VkDevice device);
    void createFramebuffers(VkDevice device, Swapchain& swapchain, Pipeline& pipeline);
    void createSyncObjects(VkDevice device, uint32_t imageCount);

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                             Swapchain& swapchain, Pipeline& pipeline);
};

} // namespace VulkanEngine
