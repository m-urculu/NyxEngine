#include "renderer/Renderer.h"
#include "renderer/VulkanContext.h"
#include "renderer/Swapchain.h"
#include "renderer/Pipeline.h"
#include "renderer/Mesh.h"
#include "renderer/Descriptors.h"
#include "renderer/DepthBuffer.h"
#include "renderer/UniformTypes.h"
#include "ui/UIPipeline.h"
#include "ui/TitleBar.h"
#include "ecs/Registry.h"
#include "ecs/components/TransformComponent.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/MaterialComponent.h"
#include "Logger.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <stdexcept>
#include <array>

namespace Talos {

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
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device, m_imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device, m_inFlightFences[i], nullptr);
    }

    for (auto sem : m_renderFinishedSemaphores) {
        vkDestroySemaphore(device, sem, nullptr);
    }

    for (auto framebuffer : m_framebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }

    vkDestroyCommandPool(device, m_commandPool, nullptr);
}

void Renderer::recreateFramebuffers(VkDevice device, Swapchain& swapchain, Pipeline& pipeline) {
    for (auto fb : m_framebuffers) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    createFramebuffers(device, swapchain, pipeline);

    uint32_t imageCount = static_cast<uint32_t>(swapchain.getImageViews().size());

    for (auto sem : m_renderFinishedSemaphores) {
        vkDestroySemaphore(device, sem, nullptr);
    }

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
// FRAME RENDERING
// ════════════════════════════════════════════════════════════════════════════

bool Renderer::drawFrame(VulkanContext& context, Swapchain& swapchain,
                          Pipeline& pipeline, Registry& registry, Descriptors& descriptors,
                          UIPipeline* uiPipeline, TitleBar* titleBar) {
    VkDevice device = context.getDevice();

    vkWaitForFences(device, 1, &m_inFlightFences[m_currentFrame],
                    VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        device, swapchain.getSwapchain(), UINT64_MAX,
        m_imageAvailableSemaphores[m_currentFrame],
        VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    m_imagesInFlight[imageIndex] = m_inFlightFences[m_currentFrame];

    vkResetFences(device, 1, &m_inFlightFences[m_currentFrame]);

    vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
    recordCommandBuffer(m_commandBuffers[m_currentFrame], imageIndex, swapchain, pipeline, registry, descriptors, uiPipeline, titleBar);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[]      = { m_imageAvailableSemaphores[m_currentFrame] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores    = waitSemaphores;
    submitInfo.pWaitDstStageMask  = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &m_commandBuffers[m_currentFrame];

    VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphores[imageIndex] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    if (vkQueueSubmit(context.getGraphicsQueue(), 1, &submitInfo,
                      m_inFlightFences[m_currentFrame]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = signalSemaphores;

    VkSwapchainKHR swapchains[] = { swapchain.getSwapchain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains    = swapchains;
    presentInfo.pImageIndices  = &imageIndex;

    result = vkQueuePresentKHR(context.getPresentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return false;
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

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
        std::array<VkImageView, 2> attachments = {
            imageViews[i],
            swapchain.getDepthBuffer().getImageView()
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass      = pipeline.getRenderPass();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments    = attachments.data();
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

    m_renderFinishedSemaphores.resize(imageCount);
    m_imagesInFlight.resize(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create synchronization objects");
        }
    }

    for (uint32_t i = 0; i < imageCount; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create render-finished semaphore");
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// COMMAND RECORDING
// ════════════════════════════════════════════════════════════════════════════

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                                    Swapchain& swapchain, Pipeline& pipeline,
                                    Registry& registry, Descriptors& descriptors,
                                    UIPipeline* uiPipeline, TitleBar* titleBar) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass  = pipeline.getRenderPass();
    renderPassInfo.framebuffer = m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchain.getExtent();

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = {{0.01f, 0.01f, 0.02f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues    = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getPipeline());

    // Set dynamic viewport and scissor from current swapchain extent
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(swapchain.getExtent().width);
    viewport.height   = static_cast<float>(swapchain.getExtent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain.getExtent();
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // Bind global descriptor set (set 0) once
    VkDescriptorSet globalSet = descriptors.getSet(m_currentFrame);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.getPipelineLayout(), 0, 1, &globalSet, 0, nullptr);

    // Iterate all entities with Transform + Mesh, push constants per draw
    auto& meshPool = registry.pool<MeshComponent>();
    for (size_t i = 0; i < meshPool.size(); i++) {
        Entity entity = meshPool.getEntity(i);
        const MeshComponent& mc = meshPool[i];

        if (!mc.mesh) continue;
        if (!registry.has<TransformComponent>(entity)) continue;

        const TransformComponent& tc = registry.get<TransformComponent>(entity);

        // Push per-object model + normalMatrix
        PushConstants pc{};
        pc.model        = tc.worldMatrix;
        pc.normalMatrix = glm::transpose(glm::inverse(tc.worldMatrix));

        vkCmdPushConstants(commandBuffer, pipeline.getPipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pc);

        // Bind per-material descriptor set (set 1) if entity has MaterialComponent
        if (registry.has<MaterialComponent>(entity)) {
            const MaterialComponent& mat = registry.get<MaterialComponent>(entity);
            if (mat.descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline.getPipelineLayout(), 1, 1,
                                        &mat.descriptorSet, 0, nullptr);
            }
        }

        mc.mesh->draw(commandBuffer);
    }

    // ── Draw UI overlay ────────────────────────────────────────────────────
    if (uiPipeline && titleBar && titleBar->isVisible()) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline->getPipeline());

        glm::vec2 screenSize(static_cast<float>(swapchain.getExtent().width),
                             static_cast<float>(swapchain.getExtent().height));
        vkCmdPushConstants(commandBuffer, uiPipeline->getPipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec2), &screenSize);

        titleBar->draw(commandBuffer);
    }

    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }
}

} // namespace Talos
