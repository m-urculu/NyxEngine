#include "renderer/Renderer.h"
#include "renderer/VulkanContext.h"
#include "renderer/Swapchain.h"
#include "renderer/Pipeline.h"
#include "Logger.h"

#include <stdexcept>
#include <array>

namespace VulkanEngine {

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC
// ════════════════════════════════════════════════════════════════════════════

void Renderer::init(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline) {
    createCommandPool(context);
    createCommandBuffers(context.getDevice());
    createFramebuffers(context.getDevice(), swapchain, pipeline);
    createSyncObjects(context.getDevice(), static_cast<uint32_t>(swapchain.getImageViews().size()));
    LOG_INFO("Renderer initialized ({} frames in flight)", MAX_FRAMES_IN_FLIGHT);
}

void Renderer::cleanup(VkDevice device) {
    // Destroy per-frame sync objects
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device, m_imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device, m_inFlightFences[i], nullptr);
    }

    // Destroy per-image semaphores
    for (auto sem : m_renderFinishedSemaphores) {
        vkDestroySemaphore(device, sem, nullptr);
    }

    // Destroy framebuffers
    for (auto framebuffer : m_framebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }

    // Destroy command pool (this also frees all command buffers)
    vkDestroyCommandPool(device, m_commandPool, nullptr);
}

void Renderer::recreateFramebuffers(VkDevice device, Swapchain& swapchain, Pipeline& pipeline) {
    for (auto fb : m_framebuffers) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    createFramebuffers(device, swapchain, pipeline);

    // Recreate per-image sync objects for new swapchain image count
    uint32_t imageCount = static_cast<uint32_t>(swapchain.getImageViews().size());

    // Destroy old per-image semaphores
    for (auto sem : m_renderFinishedSemaphores) {
        vkDestroySemaphore(device, sem, nullptr);
    }

    // Create new ones
    m_renderFinishedSemaphores.resize(imageCount);
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < imageCount; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create semaphore");
        }
    }

    m_imagesInFlight.assign(imageCount, VK_NULL_HANDLE);
}

void Renderer::waitIdle(VkDevice device) {
    vkDeviceWaitIdle(device);
}

// ════════════════════════════════════════════════════════════════════════════
// FRAME RENDERING — The main render loop
// ════════════════════════════════════════════════════════════════════════════

bool Renderer::drawFrame(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline) {
    VkDevice device = context.getDevice();

    // ── Step 1: Wait for the previous frame using this slot to finish ──────
    // Fences are CPU-GPU synchronization. We wait here until the GPU is done
    // with the command buffer we're about to reuse.
    vkWaitForFences(device, 1, &m_inFlightFences[m_currentFrame],
                    VK_TRUE, UINT64_MAX);

    // ── Step 2: Acquire the next image from the swapchain ──────────────────
    // The semaphore will be signaled when the image is ready to be rendered to.
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        device, swapchain.getSwapchain(), UINT64_MAX,
        m_imageAvailableSemaphores[m_currentFrame],
        VK_NULL_HANDLE, &imageIndex);

    // If the swapchain is out of date (e.g., window resized), signal the caller
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;  // Caller should recreate swapchain
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    // Wait if a previous frame is still using this swapchain image.
    // This handles the case where we have more swapchain images than
    // frames-in-flight (e.g., 5 images but only 2 frames-in-flight).
    if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    // Mark this image as now being used by the current frame's fence
    m_imagesInFlight[imageIndex] = m_inFlightFences[m_currentFrame];

    // Only reset the fence if we are actually going to submit work
    vkResetFences(device, 1, &m_inFlightFences[m_currentFrame]);

    // ── Step 3: Record draw commands ───────────────────────────────────────
    vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
    recordCommandBuffer(m_commandBuffers[m_currentFrame], imageIndex, swapchain, pipeline);

    // ── Step 4: Submit the command buffer to the GPU ───────────────────────
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    // Wait for the image to be available before writing colors
    VkSemaphore waitSemaphores[]      = { m_imageAvailableSemaphores[m_currentFrame] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores    = waitSemaphores;
    submitInfo.pWaitDstStageMask  = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &m_commandBuffers[m_currentFrame];

    // Signal this semaphore when rendering is finished.
    // Indexed by imageIndex (not m_currentFrame) because the present engine holds
    // the semaphore until the image is re-acquired, and with mailbox mode there can
    // be more images than frames-in-flight.
    VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphores[imageIndex] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    // The fence is signaled when the command buffer finishes executing
    if (vkQueueSubmit(context.getGraphicsQueue(), 1, &submitInfo,
                      m_inFlightFences[m_currentFrame]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    // ── Step 5: Present the rendered image to the screen ───────────────────
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = signalSemaphores;  // Wait for rendering to finish

    VkSwapchainKHR swapchains[] = { swapchain.getSwapchain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains    = swapchains;
    presentInfo.pImageIndices  = &imageIndex;

    result = vkQueuePresentKHR(context.getPresentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return false;  // Caller should recreate swapchain
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    // Advance to the next frame-in-flight slot
    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE SETUP
// ════════════════════════════════════════════════════════════════════════════

void Renderer::createCommandPool(VulkanContext& context) {
    QueueFamilyIndices indices = context.findQueueFamilies(context.getPhysicalDevice());

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = indices.graphicsFamily.value();

    if (vkCreateCommandPool(context.getDevice(), &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }
}

void Renderer::createCommandBuffers(VkDevice device) {
    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = m_commandPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

    if (vkAllocateCommandBuffers(device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }
}

void Renderer::createFramebuffers(VkDevice device, Swapchain& swapchain, Pipeline& pipeline) {
    const auto& imageViews = swapchain.getImageViews();
    m_framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); i++) {
        VkImageView attachments[] = { imageViews[i] };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass      = pipeline.getRenderPass();
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments    = attachments;
        framebufferInfo.width           = swapchain.getExtent().width;
        framebufferInfo.height          = swapchain.getExtent().height;
        framebufferInfo.layers          = 1;

        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create framebuffer");
        }
    }
}

void Renderer::createSyncObjects(VkDevice device, uint32_t imageCount) {
    m_imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    // Per-image: one renderFinished semaphore and one fence tracker per swapchain image
    m_renderFinishedSemaphores.resize(imageCount);
    m_imagesInFlight.resize(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // Start signaled so first frame doesn't deadlock

    // Per-frame objects
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create synchronization objects");
        }
    }

    // Per-image semaphores
    for (uint32_t i = 0; i < imageCount; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create render-finished semaphore");
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// COMMAND RECORDING — Tells the GPU what to draw
// ════════════════════════════════════════════════════════════════════════════

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                                    Swapchain& swapchain, Pipeline& pipeline) {
    // Begin recording commands
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    // Begin the render pass — this transitions the image and clears it
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass  = pipeline.getRenderPass();
    renderPassInfo.framebuffer = m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchain.getExtent();

    // Clear color: dark blue-gray background
    VkClearValue clearColor = {{{0.01f, 0.01f, 0.02f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues    = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Bind the graphics pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getPipeline());

    // Draw 3 vertices (the triangle is hardcoded in the vertex shader)
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    // End the render pass and finish recording
    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }
}

} // namespace VulkanEngine
