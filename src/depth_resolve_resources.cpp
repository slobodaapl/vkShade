#include "depth_resolve_resources.hpp"

#include "format.hpp"
#include "logical_swapchain.hpp"

#include <algorithm>

namespace vkShade
{
    void invalidateDepthResolveOutput(LogicalSwapchain* swapchain)
    {
        std::fill(swapchain->depthResolveVersions.begin(), swapchain->depthResolveVersions.end(), 0);
        swapchain->depthResolveLatest.reset();
        swapchain->depthResolveVersion = 0;
    }

    void
    recordPreviousDepthResolve(LogicalDevice* device, LogicalSwapchain* swapchain, VkCommandBuffer commandBuffer, uint32_t source, uint32_t target)
    {
        const VkImageAspectFlags aspect = swapchain->depthResolveUsesShader ? VK_IMAGE_ASPECT_COLOR_BIT
                                                                            : (isStencilFormat(swapchain->depthResolveFormat)
                                                                                   ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                                                   : VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageMemoryBarrier     barriers[2]{};
        for (auto& barrier : barriers)
        {
            barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange    = {aspect, 0, 1, 0, 1};
            barrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        barriers[0].image         = swapchain->depthResolveImages[source];
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image         = swapchain->depthResolveImages[target];
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        device->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy              copy{};
        const VkImageAspectFlags copyAspect = swapchain->depthResolveUsesShader ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        copy.srcSubresource                 = {copyAspect, 0, 0, 1};
        copy.dstSubresource                 = {copyAspect, 0, 0, 1};
        copy.extent                         = swapchain->depthResolveExtent;
        device->vkd.CmdCopyImage(commandBuffer,
                                 barriers[0].image,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 barriers[1].image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1,
                                 &copy);
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        device->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    }

    void destroyDepthResolveResources(LogicalSwapchain* swapchain)
    {
        auto* device = swapchain->pLogicalDevice;
        for (auto framebuffer : swapchain->depthResolveFramebuffers)
            device->vkd.DestroyFramebuffer(device->device, framebuffer, nullptr);
        swapchain->depthResolveFramebuffers.clear();
        if (swapchain->depthResolvePipeline)
            device->vkd.DestroyPipeline(device->device, swapchain->depthResolvePipeline, nullptr);
        if (swapchain->depthResolvePipelineLayout)
            device->vkd.DestroyPipelineLayout(device->device, swapchain->depthResolvePipelineLayout, nullptr);
        if (swapchain->depthResolveRenderPass)
            device->vkd.DestroyRenderPass(device->device, swapchain->depthResolveRenderPass, nullptr);
        if (swapchain->depthResolveDescriptorPool)
            device->vkd.DestroyDescriptorPool(device->device, swapchain->depthResolveDescriptorPool, nullptr);
        if (swapchain->depthResolveDescriptorSetLayout)
            device->vkd.DestroyDescriptorSetLayout(device->device, swapchain->depthResolveDescriptorSetLayout, nullptr);
        if (swapchain->depthResolveSampler)
            device->vkd.DestroySampler(device->device, swapchain->depthResolveSampler, nullptr);
        swapchain->depthResolvePipeline            = VK_NULL_HANDLE;
        swapchain->depthResolvePipelineLayout      = VK_NULL_HANDLE;
        swapchain->depthResolveRenderPass          = VK_NULL_HANDLE;
        swapchain->depthResolveDescriptorPool      = VK_NULL_HANDLE;
        swapchain->depthResolveDescriptorSetLayout = VK_NULL_HANDLE;
        swapchain->depthResolveSampler             = VK_NULL_HANDLE;

        for (auto view : swapchain->depthResolveImageViews)
            if (view)
                device->vkd.DestroyImageView(device->device, view, nullptr);
        for (auto image : swapchain->depthResolveImages)
            if (image)
                device->vkd.DestroyImage(device->device, image, nullptr);
        for (auto memory : swapchain->depthResolveMemories)
            if (memory)
                device->vkd.FreeMemory(device->device, memory, nullptr);
        swapchain->depthResolveImageViews.clear();
        swapchain->depthResolveImages.clear();
        swapchain->depthResolveMemories.clear();
        swapchain->depthResolveInitialized.clear();
        swapchain->depthResolveVersions.clear();
        swapchain->depthResolveLatest.reset();
        swapchain->depthResolveVersion = 0;
        swapchain->depthResolveDescriptorSets.clear();
        swapchain->depthResolveFormat          = VK_FORMAT_UNDEFINED;
        swapchain->depthResolveExtent          = {0, 0, 1};
        swapchain->depthResolveUsesShader      = false;
        swapchain->depthResolveSourceImage     = VK_NULL_HANDLE;
        swapchain->depthResolveSourceImageView = VK_NULL_HANDLE;
        swapchain->depthResolveSourceId        = 0;
    }
} // namespace vkShade
