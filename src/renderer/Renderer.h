#pragma once

// Renderer.h — Frame rendering orchestrator

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include "renderer/HdrTarget.h"
#include "renderer/BloomPass.h"

namespace Nyx {

class VulkanContext;
class Swapchain;
class Pipeline;
class Mesh;
class Descriptors;
class Registry;
class UIPipeline;
class TitleBar;
class ShadowMap;
class ContentBrowser;
class Console;
class CodeEditor;
class ImagePipeline;
class MaterialPreviewPipeline;
class SceneHierarchy;
class Inspector;
class Gizmo;

class Renderer {
public:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    void init(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline);
    void cleanup(VkDevice device, VmaAllocator allocator);

    bool drawFrame(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline,
                    Registry& registry, Descriptors& descriptors,
                    ShadowMap* shadowMap = nullptr,
                    UIPipeline* uiPipeline = nullptr, TitleBar* titleBar = nullptr,
                    ContentBrowser* contentBrowser = nullptr, Console* console = nullptr,
                    CodeEditor* editor = nullptr,
                    ImagePipeline* imagePipeline = nullptr,
                    MaterialPreviewPipeline* matPipeline = nullptr,
                    SceneHierarchy* hierarchy = nullptr,
                    Inspector* inspector = nullptr,
                    Gizmo* gizmo = nullptr);

    void recreateFramebuffers(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline);

    void waitIdle(VkDevice device);

    uint32_t getCurrentFrame() const { return m_currentFrame; }

    VkImageView getHdrView() const;     // for BloomPass / external consumers (phase 2)

private:
    VkCommandPool                m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;

    // HDR scene target — meshes/sky write linear HDR here; composite RP samples it.
    // One target shared across frames (the swapchain's image-in-flight fence chain
    // already serializes work between frames at the queue level).
    std::unique_ptr<HdrTarget> m_hdrTarget;
    std::unique_ptr<BloomPass> m_bloomPass;
    VkFramebuffer              m_sceneFramebuffer = VK_NULL_HANDLE;       // scene RP: HDR + depth
    std::vector<VkFramebuffer> m_compositeFramebuffers;                   // composite RP: per swapchain image

    // Composite descriptor (set 0, binding 0 = HDR sampler). Single set — points to
    // the shared HDR target.
    VkDescriptorPool m_compositePool = VK_NULL_HANDLE;
    VkDescriptorSet  m_compositeSet  = VK_NULL_HANDLE;

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkFence>     m_inFlightFences;

    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence>     m_imagesInFlight;

    uint32_t m_currentFrame = 0;

    void createCommandPool(VulkanContext& context);
    void createCommandBuffers(VkDevice device);
    void createFramebuffers(VulkanContext& context, Swapchain& swapchain, Pipeline& pipeline);
    void createCompositeDescriptor(VkDevice device, Pipeline& pipeline);
    void writeCompositeDescriptor(VkDevice device);                        // rebind HDR view after resize
    void createSyncObjects(VkDevice device, uint32_t imageCount);

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                             Swapchain& swapchain, Pipeline& pipeline,
                             Registry& registry, Descriptors& descriptors,
                             ShadowMap* shadowMap,
                             UIPipeline* uiPipeline, TitleBar* titleBar,
                             ContentBrowser* contentBrowser, Console* console, CodeEditor* editor,
                             ImagePipeline* imagePipeline, MaterialPreviewPipeline* matPipeline,
                             SceneHierarchy* hierarchy, Inspector* inspector, Gizmo* gizmo);
};

} // namespace Nyx
