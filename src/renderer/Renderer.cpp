#include "renderer/Renderer.h"
#include "renderer/VulkanContext.h"
#include "renderer/Swapchain.h"
#include "renderer/Pipeline.h"
#include "renderer/ShadowMap.h"
#include "renderer/HdrTarget.h"
#include "renderer/Mesh.h"
#include "renderer/Descriptors.h"
#include "renderer/DepthBuffer.h"
#include "renderer/UniformTypes.h"
#include "ecs/components/EnvironmentComponent.h"
#include "ui/UIPipeline.h"
#include "ui/TitleBar.h"
#include "ui/ContentBrowser.h"
#include "ui/Console.h"
#include "ui/CodeEditor.h"
#include "ui/SceneHierarchy.h"
#include "ui/Inspector.h"
#include "ui/Gizmo.h"
#include "renderer/ImagePipeline.h"
#include "renderer/MaterialPreviewPipeline.h"
#include "ecs/Registry.h"
#include "ecs/components/TransformComponent.h"
#include "ecs/components/MeshComponent.h"
#include "ecs/components/LightComponent.h"

#include <cstring>
#include "ecs/components/MaterialComponent.h"
#include "ecs/components/SkinComponent.h"
#include "Logger.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <stdexcept>
#include <array>

namespace Nyx {

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC
// ════════════════════════════════════════════════════════════════════════════

void Renderer::init(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline) {
    createCommandPool(context);
    createCommandBuffers(context.getDevice());
    createFramebuffers(context, swapchain, pipeline);
    createCompositeDescriptor(context.getDevice(), pipeline);
    createSyncObjects(context.getDevice(), static_cast<uint32_t>(swapchain.getImageViews().size()));
    LOG_INFO("Renderer initialized ({} frames in flight)", MAX_FRAMES_IN_FLIGHT);
}

void Renderer::cleanup(VkDevice device, VmaAllocator allocator) {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device, m_imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device, m_inFlightFences[i], nullptr);
    }

    for (auto sem : m_renderFinishedSemaphores) {
        vkDestroySemaphore(device, sem, nullptr);
    }

    if (m_compositePool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_compositePool, nullptr);
        m_compositePool = VK_NULL_HANDLE;
        m_compositeSet  = VK_NULL_HANDLE;
    }

    for (auto fb : m_compositeFramebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    m_compositeFramebuffers.clear();
    if (m_sceneFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_sceneFramebuffer, nullptr);
        m_sceneFramebuffer = VK_NULL_HANDLE;
    }
    // HdrTarget + BloomPass own VMA-allocated images plus framebuffers / render
    // passes / pipelines — these MUST be destroyed before vkDestroyDevice. Their
    // unique_ptr destructors alone won't release the GPU handles; call their
    // explicit cleanup() first, then drop the wrappers.
    if (m_bloomPass) { m_bloomPass->cleanup(device, allocator); m_bloomPass.reset(); }
    if (m_hdrTarget) { m_hdrTarget->cleanup(device, allocator); m_hdrTarget.reset(); }

    vkDestroyCommandPool(device, m_commandPool, nullptr);
}

void Renderer::recreateFramebuffers(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline) {
    // Tear down all swapchain-extent resources, recreate at new size, rewrite the
    // composite descriptor to point at the new HDR view.
    VkDevice device = context.getDevice();
    for (auto fb : m_compositeFramebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    m_compositeFramebuffers.clear();
    if (m_sceneFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_sceneFramebuffer, nullptr);
        m_sceneFramebuffer = VK_NULL_HANDLE;
    }

    createFramebuffers(context, swapchain, pipeline);
    writeCompositeDescriptor(device);

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

VkImageView Renderer::getHdrView() const {
    return m_hdrTarget ? m_hdrTarget->getView() : VK_NULL_HANDLE;
}

void Renderer::waitIdle(VkDevice device) {
    vkDeviceWaitIdle(device);
}

// ════════════════════════════════════════════════════════════════════════════
// FRAME RENDERING
// ════════════════════════════════════════════════════════════════════════════

bool Renderer::drawFrame(VulkanContext& context, Swapchain& swapchain,
                          Pipeline& pipeline, Registry& registry, Descriptors& descriptors,
                          ShadowMap* shadowMap,
                          PointShadowMap* pointShadowPool,
                          const PointShadowJob* pointShadowJobs,
                          size_t pointShadowJobCount,
                          UIPipeline* uiPipeline, TitleBar* titleBar,
                          ContentBrowser* contentBrowser, Console* console, CodeEditor* editor,
                          ImagePipeline* imagePipeline, MaterialPreviewPipeline* matPipeline,
                          SceneHierarchy* hierarchy, Inspector* inspector, Gizmo* gizmo) {
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
    recordCommandBuffer(m_commandBuffers[m_currentFrame], imageIndex, swapchain, pipeline,
                        registry, descriptors, shadowMap,
                        pointShadowPool, pointShadowJobs, pointShadowJobCount,
                        uiPipeline, titleBar, contentBrowser, console, editor,
                        imagePipeline, matPipeline, hierarchy, inspector, gizmo);

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

void Renderer::createFramebuffers(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline) {
    VkDevice device   = context.getDevice();
    VkExtent2D extent = swapchain.getExtent();

    // (Re)create the HDR target at the current swapchain extent.
    if (!m_hdrTarget) m_hdrTarget = std::make_unique<HdrTarget>();
    if (m_hdrTarget->getExtent().width != extent.width || m_hdrTarget->getExtent().height != extent.height) {
        if (m_hdrTarget->getImage() == VK_NULL_HANDLE) m_hdrTarget->init(context, extent);
        else                                           m_hdrTarget->resize(context, extent);
    }

    // Bloom mip chain — sized to half the scene extent at mip 0.
    if (!m_bloomPass) {
        m_bloomPass = std::make_unique<BloomPass>();
        m_bloomPass->init(context, m_hdrTarget->getView(), m_hdrTarget->getSampler(), extent);
    } else {
        m_bloomPass->resize(context, m_hdrTarget->getView(), extent);
    }

    // Scene framebuffer — HDR colour + shared depth. One framebuffer (both attachments
    // are shared; the per-frame fence chain serializes work).
    {
        std::array<VkImageView, 2> atts = {
            m_hdrTarget->getView(),
            swapchain.getDepthBuffer().getImageView(),
        };
        VkFramebufferCreateInfo fbi{};
        fbi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass      = pipeline.getRenderPass();
        fbi.attachmentCount = static_cast<uint32_t>(atts.size());
        fbi.pAttachments    = atts.data();
        fbi.width           = extent.width;
        fbi.height          = extent.height;
        fbi.layers          = 1;
        if (vkCreateFramebuffer(device, &fbi, nullptr, &m_sceneFramebuffer) != VK_SUCCESS)
            throw std::runtime_error("Failed to create scene framebuffer");
    }

    // Composite framebuffers — one per swapchain image (each holds its own image view).
    const auto& imageViews = swapchain.getImageViews();
    m_compositeFramebuffers.resize(imageViews.size());
    for (size_t i = 0; i < imageViews.size(); i++) {
        VkFramebufferCreateInfo fbi{};
        fbi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass      = pipeline.getCompositeRenderPass();
        fbi.attachmentCount = 1;
        fbi.pAttachments    = &imageViews[i];
        fbi.width           = extent.width;
        fbi.height          = extent.height;
        fbi.layers          = 1;
        if (vkCreateFramebuffer(device, &fbi, nullptr, &m_compositeFramebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create composite framebuffer");
    }
}

void Renderer::createCompositeDescriptor(VkDevice device, Pipeline& pipeline) {
    VkDescriptorPoolSize size{};
    size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 2;   // binding 0 = HDR, binding 1 = bloom

    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.poolSizeCount = 1;
    pool.pPoolSizes    = &size;
    pool.maxSets       = 1;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &m_compositePool) != VK_SUCCESS)
        throw std::runtime_error("Failed to create composite descriptor pool");

    VkDescriptorSetLayout layout = pipeline.getCompositeSetLayout();
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_compositePool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &layout;
    if (vkAllocateDescriptorSets(device, &alloc, &m_compositeSet) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate composite descriptor set");

    writeCompositeDescriptor(device);
}

void Renderer::writeCompositeDescriptor(VkDevice device) {
    if (m_compositeSet == VK_NULL_HANDLE || !m_hdrTarget || !m_bloomPass) return;
    std::array<VkDescriptorImageInfo, 2> infos{};
    infos[0].imageView   = m_hdrTarget->getView();
    infos[0].sampler     = m_hdrTarget->getSampler();
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].imageView   = m_bloomPass->getResultView();
    infos[1].sampler     = m_bloomPass->getSampler();
    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < 2; i++) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_compositeSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
                                    ShadowMap* shadowMap,
                                    PointShadowMap* pointShadowPool,
                                    const PointShadowJob* pointShadowJobs,
                                    size_t pointShadowJobCount,
                                    UIPipeline* uiPipeline, TitleBar* titleBar,
                                    ContentBrowser* contentBrowser, Console* console, CodeEditor* editor,
                                    ImagePipeline* imagePipeline, MaterialPreviewPipeline* matPipeline,
                                    SceneHierarchy* hierarchy, Inspector* inspector, Gizmo* gizmo) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    // ── Shadow pass (sun shadow map) ────────────────────────────────────────────
    // Runs BEFORE the main render pass: depth-only render of every opaque mesh from
    // the sun's POV. The framebuffer's render-pass dependency transitions the depth
    // image to DEPTH_READ_ONLY at the end, so mesh.frag can sample it directly via
    // set 0 binding 1 in the main pass. Skinned/cutout meshes don't cast shadows yet.
    if (shadowMap) {
        VkDescriptorSet globalSet = descriptors.getSet(m_currentFrame);
        shadowMap->beginRenderPass(commandBuffer);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowMap->getPipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                shadowMap->getPipelineLayout(), 0, 1, &globalSet, 0, nullptr);
        vkCmdSetDepthBias(commandBuffer, 1.5f, 0.0f, 1.75f);   // constant, clamp, slope (pipeline dyn state)

        auto& meshPool = registry.pool<MeshComponent>();
        for (size_t i = 0; i < meshPool.size(); i++) {
            Entity entity = meshPool.getEntity(i);
            const MeshComponent& mc = meshPool[i];
            if (!mc.mesh) continue;
            if (!registry.has<TransformComponent>(entity)) continue;
            // Light gizmo sphere — visual marker, not real geometry. Skip the
            // shadow pass so it doesn't drop a shadow under the light itself.
            if (registry.has<LightComponent>(entity)) continue;

            const bool isCutout  = registry.has<MaterialComponent>(entity)
                                && registry.get<MaterialComponent>(entity).alphaCutoff > 0.0f;
            const bool isSkinned = registry.has<SkinComponent>(entity)
                                && registry.get<SkinComponent>(entity).jointSet != VK_NULL_HANDLE;
            if (isCutout || isSkinned) continue;    // not handled by this pre-pass yet

            PushConstants pc{};
            pc.model        = registry.get<TransformComponent>(entity).worldMatrix;
            pc.normalMatrix = glm::mat4(1.0f);
            vkCmdPushConstants(commandBuffer, shadowMap->getPipelineLayout(),
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pc);
            mc.mesh->draw(commandBuffer);
        }
        shadowMap->endRenderPass(commandBuffer);
    }

    // ── Point-light shadow passes ───────────────────────────────────────────────
    // For each enabled point-shadow job, render the scene 6 times into the cube
    // map's faces using the engine-computed viewProj matrices. Linear distance
    // from the light is written to R32_SFLOAT via VK_BLEND_OP_MIN — no depth.
    if (pointShadowPool && pointShadowJobs && pointShadowJobCount > 0) {
        struct PointShadowPC {
            float viewProj[16];
            float model[16];
            float lightPosAndFar[4];
        };
        for (size_t j = 0; j < pointShadowJobCount; ++j) {
            const PointShadowJob& job = pointShadowJobs[j];
            PointShadowMap& sm = pointShadowPool[job.slot];
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sm.getPipeline());
            for (uint32_t f = 0; f < 6; ++f) {
                sm.beginFace(commandBuffer, f);
                auto& meshPool = registry.pool<MeshComponent>();
                for (size_t i = 0; i < meshPool.size(); i++) {
                    Entity entity = meshPool.getEntity(i);
                    const MeshComponent& mc = meshPool[i];
                    if (!mc.mesh) continue;
                    if (!registry.has<TransformComponent>(entity)) continue;
                    if (registry.has<LightComponent>(entity)) continue;   // skip gizmos
                    const bool isCutout  = registry.has<MaterialComponent>(entity)
                                        && registry.get<MaterialComponent>(entity).alphaCutoff > 0.0f;
                    const bool isSkinned = registry.has<SkinComponent>(entity)
                                        && registry.get<SkinComponent>(entity).jointSet != VK_NULL_HANDLE;
                    if (isCutout || isSkinned) continue;

                    PointShadowPC pc{};
                    std::memcpy(pc.viewProj, &job.viewProj[f], sizeof(pc.viewProj));
                    glm::mat4 model = registry.get<TransformComponent>(entity).worldMatrix;
                    std::memcpy(pc.model, &model, sizeof(pc.model));
                    pc.lightPosAndFar[0] = job.lightPos.x;
                    pc.lightPosAndFar[1] = job.lightPos.y;
                    pc.lightPosAndFar[2] = job.lightPos.z;
                    pc.lightPosAndFar[3] = job.farRadius;
                    vkCmdPushConstants(commandBuffer, sm.getPipelineLayout(),
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(pc), &pc);
                    mc.mesh->draw(commandBuffer);
                }
                sm.endFace(commandBuffer);
            }
        }
    }

    // ── Scene render pass (HDR linear target) ───────────────────────────────────
    // Writes the HDR scene into m_hdrTarget; the composite pass below tonemaps it
    // into the swapchain. The (imageIndex) slot is only used by the composite FB.
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass  = pipeline.getRenderPass();
    renderPassInfo.framebuffer = m_sceneFramebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchain.getExtent();

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = {{0.01f, 0.01f, 0.02f, 1.0f}};  // matches dark sky behind composite
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

    // ── Procedural skybox ───────────────────────────────────────────────────────
    // Drawn first: fullscreen tri at depth=1 with depth-write OFF, so opaque meshes
    // will overdraw it where present. The same analytic sky function drives this
    // and the IBL terms in mesh.frag, so reflections match the visible background.
    VkDescriptorSet globalSet = descriptors.getSet(m_currentFrame);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getSkyPipeline());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.getSkyPipelineLayout(), 0, 1, &globalSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    // ── Depth pre-pass (opaque only) ────────────────────────────────────────────
    // Lays down depth for every opaque mesh first so the main pass's heavy PBR
    // shader runs at most once per pixel (depth EQUAL + no write). Major win on the
    // head/helmet region where multiple primitives stack up. Skinned and cutout are
    // skipped here — they write their own depth in the main pass.
    auto& meshPool = registry.pool<MeshComponent>();
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.getDepthPrePassPipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.getDepthPrePassPipelineLayout(), 0, 1, &globalSet, 0, nullptr);
        for (size_t i = 0; i < meshPool.size(); i++) {
            Entity entity = meshPool.getEntity(i);
            const MeshComponent& mc = meshPool[i];
            if (!mc.mesh) continue;
            if (!registry.has<TransformComponent>(entity)) continue;
            if (registry.has<LightComponent>(entity)) continue;   // light gizmo → drawn as a screen icon, not geometry

            const bool isCutout  = registry.has<MaterialComponent>(entity)
                                && registry.get<MaterialComponent>(entity).alphaCutoff > 0.0f;
            const bool isSkinned = registry.has<SkinComponent>(entity)
                                && registry.get<SkinComponent>(entity).jointSet != VK_NULL_HANDLE;
            if (isCutout || isSkinned) continue;     // not part of the opaque pre-pass

            PushConstants pc{};
            pc.model        = registry.get<TransformComponent>(entity).worldMatrix;
            pc.normalMatrix = glm::mat4(1.0f);       // unused by depth_only.vert
            vkCmdPushConstants(commandBuffer, pipeline.getDepthPrePassPipelineLayout(),
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pc);
            mc.mesh->draw(commandBuffer);
        }
    }

    // Bind the global set again under the mesh pipeline layout (Vulkan re-binds are
    // cheap; mesh layouts are set-compatible at set 0 so the binding stays valid).
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.getPipelineLayout(), 0, 1, &globalSet, 0, nullptr);

    // Iterate all entities with Transform + Mesh, push constants per draw. Three
    // pipeline kinds: opaque (cull back, depth EQUAL + no write thanks to the pre-pass
    // above), skinned (joints, set 2, depth LESS + write), and cutout (alpha-masked,
    // two-sided/no-cull, depth LESS + write). All share sets 0/1, so the global set
    // bound above stays valid across pipeline switches. Cutout takes precedence (hair
    // cards are static), so a cutout+skinned mesh renders unskinned for now.
    enum Kind { K_NONE = -1, K_OPAQUE, K_SKINNED, K_CUTOUT };
    int boundKind = K_NONE;
    for (size_t i = 0; i < meshPool.size(); i++) {
        Entity entity = meshPool.getEntity(i);
        const MeshComponent& mc = meshPool[i];

        if (!mc.mesh) continue;
        if (!registry.has<TransformComponent>(entity)) continue;
        if (registry.has<LightComponent>(entity)) continue;   // light gizmo → drawn as a screen icon, not geometry

        const TransformComponent& tc = registry.get<TransformComponent>(entity);

        const bool isCutout  = registry.has<MaterialComponent>(entity)
                            && registry.get<MaterialComponent>(entity).alphaCutoff > 0.0f;
        const bool isSkinned = !isCutout && registry.has<SkinComponent>(entity)
                            && registry.get<SkinComponent>(entity).jointSet != VK_NULL_HANDLE;

        int kind = isCutout ? K_CUTOUT : (isSkinned ? K_SKINNED : K_OPAQUE);
        VkPipelineLayout layout = (kind == K_SKINNED) ? pipeline.getSkinnedPipelineLayout()
                                                      : pipeline.getPipelineLayout();

        if (kind != boundKind) {
            VkPipeline pipe = (kind == K_SKINNED) ? pipeline.getSkinnedPipeline()
                            : (kind == K_CUTOUT)  ? pipeline.getCutoutPipeline()
                                                  : pipeline.getPipeline();
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            boundKind = kind;
        }

        // Push per-object model + normalMatrix (ignored by the skinned shader, but
        // kept for layout compatibility).
        PushConstants pc{};
        pc.model        = tc.worldMatrix;
        pc.normalMatrix = glm::transpose(glm::inverse(tc.worldMatrix));

        vkCmdPushConstants(commandBuffer, layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pc);

        // Bind per-material descriptor set (set 1) if entity has MaterialComponent
        if (registry.has<MaterialComponent>(entity)) {
            const MaterialComponent& mat = registry.get<MaterialComponent>(entity);
            if (mat.descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        layout, 1, 1, &mat.descriptorSet, 0, nullptr);
            }
        }

        // Bind joint matrices (set 2) for skinned meshes
        if (isSkinned) {
            VkDescriptorSet jointSet = registry.get<SkinComponent>(entity).jointSet;
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout, 2, 1, &jointSet, 0, nullptr);
        }

        mc.mesh->draw(commandBuffer);
    }

    // End of scene RP — m_hdrTarget transitions to SHADER_READ_ONLY via subpass dep.
    vkCmdEndRenderPass(commandBuffer);

    // ── Resolve EnvironmentComponent so bloom + composite use editor-driven values.
    EnvironmentComponent env{};   // defaults if no env entity exists
    {
        auto& envPool = registry.pool<EnvironmentComponent>();
        if (envPool.size() > 0) env = envPool[0];
    }

    // ── Bloom: downsample brightpass from HDR → mip chain → upsample tent → mip 0
    if (m_bloomPass) m_bloomPass->render(commandBuffer, env.bloomThreshold, env.bloomKnee);

    // ── Composite render pass: tonemap HDR + bloom → swapchain, then UI overlay ─
    {
        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderPassBeginInfo cri{};
        cri.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        cri.renderPass        = pipeline.getCompositeRenderPass();
        cri.framebuffer       = m_compositeFramebuffers[imageIndex];
        cri.renderArea.offset = {0, 0};
        cri.renderArea.extent = swapchain.getExtent();
        cri.clearValueCount   = 1;
        cri.pClearValues      = &clear;
        vkCmdBeginRenderPass(commandBuffer, &cri, VK_SUBPASS_CONTENTS_INLINE);

        // Composite tri samples HDR + tonemaps. Needs viewport/scissor set again
        // (dynamic state isn't preserved across render passes in Vulkan).
        VkViewport vp{};
        vp.x = 0.0f; vp.y = 0.0f;
        vp.width  = static_cast<float>(swapchain.getExtent().width);
        vp.height = static_cast<float>(swapchain.getExtent().height);
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &vp);
        VkRect2D sc{};
        sc.offset = {0, 0};
        sc.extent = swapchain.getExtent();
        vkCmdSetScissor(commandBuffer, 0, 1, &sc);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getCompositePipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.getCompositePipelineLayout(), 0, 1, &m_compositeSet, 0, nullptr);

        // Push live bloom strength + exposure + tonemap mode from the environment.
        struct CompositePC { float bloomStrength; float exposure; float tonemap; float pad; };
        CompositePC cpc{
            env.bloomStrength,
            env.exposure,
            static_cast<float>(static_cast<uint32_t>(env.tonemapper)),
            0.0f,
        };
        vkCmdPushConstants(commandBuffer, pipeline.getCompositePipelineLayout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cpc), &cpc);

        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    // ── Draw UI overlay (title bar + content browser, same pipeline) ────────
    if (uiPipeline) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline->getPipeline());

        glm::vec2 screenSize(static_cast<float>(swapchain.getExtent().width),
                             static_cast<float>(swapchain.getExtent().height));
        vkCmdPushConstants(commandBuffer, uiPipeline->getPipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec2), &screenSize);

        // FPS graph sits just above the 3D view and BELOW the editor/panels.
        if (titleBar && titleBar->isVisible())             titleBar->drawFps(commandBuffer);
        // Gizmo overlays the 3D view, below the editor/panels (so they occlude it).
        if (gizmo)                                         gizmo->draw(commandBuffer);
        if (editor && editor->isVisible())                 editor->draw(commandBuffer);
        if (hierarchy && hierarchy->isVisible())           hierarchy->draw(commandBuffer);
        if (inspector && inspector->isVisible())           inspector->draw(commandBuffer);
        if (console && console->isVisible())               console->draw(commandBuffer);
        // Content browser drawn after the right dock so its drag-ghost shows on top.
        if (contentBrowser && contentBrowser->isVisible()) contentBrowser->draw(commandBuffer);
        if (titleBar && titleBar->isVisible())             titleBar->draw(commandBuffer);     // caption on top
    }

    // ── Active asset tab preview (separate pipelines) — flat image or sphere ──
    if (editor) {
        if (imagePipeline && editor->activeIsImage()) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, imagePipeline->getPipeline());
            glm::vec2 screenSize(static_cast<float>(swapchain.getExtent().width),
                                 static_cast<float>(swapchain.getExtent().height));
            editor->drawImage(commandBuffer, screenSize);
        } else if (matPipeline && editor->activeIsMaterial()) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, matPipeline->getPipeline());
            editor->drawSphere(commandBuffer);
        }
    }

    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }
}

} // namespace Nyx
