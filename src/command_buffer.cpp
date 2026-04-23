#include "command_buffer.hpp"

#include <algorithm>

#include "format.hpp"
#include "image_view.hpp"
#include "logical_swapchain.hpp"
#include "util.hpp"

namespace vkShade
{
    static bool hasDepthResolveResources(const LogicalSwapchain* pLogicalSwapchain, size_t commandBufferCount)
    {
        return pLogicalSwapchain != nullptr
               && pLogicalSwapchain->depthResolveImages.size() == commandBufferCount
               && pLogicalSwapchain->depthResolveImageViews.size() == commandBufferCount
               && pLogicalSwapchain->depthResolveMemories.size() == commandBufferCount
               && pLogicalSwapchain->depthResolveInitialized.size() == commandBufferCount;
    }

    static bool hasDepthState(const DepthState& state)
    {
        return state.image != VK_NULL_HANDLE && state.imageView != VK_NULL_HANDLE && state.format != VK_FORMAT_UNDEFINED;
    }

    static VkImageLayout getObservedDepthAttachmentLayout(const DepthState& depthState)
    {
        return depthState.observedLayout != VK_IMAGE_LAYOUT_UNDEFINED
            ? depthState.observedLayout
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    static VkPipelineStageFlags getObservedDepthSourceStages(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }

    static VkAccessFlags getObservedDepthSourceAccess(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    static VkPipelineStageFlags getObservedDepthRestoreStages(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }

    static VkAccessFlags getObservedDepthRestoreAccess(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    static bool shouldKeepObservedDepthInGeneralLayout(const DepthState& depthState)
    {
        return getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL;
    }

    static VkImageLayout getInternalDepthReadOnlyLayout(VkFormat format)
    {
        return isStencilFormat(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }

    static VkImageLayout getDepthResolveReadOnlyLayout(const LogicalSwapchain* pLogicalSwapchain)
    {
        if (!pLogicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        return pLogicalSwapchain->depthResolveUsesShader
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat);
    }

    static VkImageView getOrCreateTrackedDepthSampleView(LogicalDevice* pLogicalDevice, const DepthState& depthState)
    {
        if (!pLogicalDevice || !hasDepthState(depthState))
            return VK_NULL_HANDLE;

        auto it = pLogicalDevice->depthViewStates.find(depthState.imageView);
        if (it != pLogicalDevice->depthViewStates.end() && it->second.imageView != VK_NULL_HANDLE)
            return it->second.imageView;

        auto imageIt = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), depthState.image);
        if (imageIt == pLogicalDevice->depthImages.end())
            return depthState.imageView;

        const size_t index = std::distance(pLogicalDevice->depthImages.begin(), imageIt);
        if (index >= pLogicalDevice->depthImageViews.size())
            pLogicalDevice->depthImageViews.resize(index + 1, VK_NULL_HANDLE);

        VkImageView& trackedView = pLogicalDevice->depthImageViews[index];
        if (trackedView == VK_NULL_HANDLE)
            trackedView = createImageViews(pLogicalDevice, depthState.format, {depthState.image}, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT)[0];

        return trackedView != VK_NULL_HANDLE ? trackedView : depthState.imageView;
    }

    static void recordDepthResolveSnapshotViaShader(LogicalDevice*    pLogicalDevice,
                                                    LogicalSwapchain* pLogicalSwapchain,
                                                    VkCommandBuffer   commandBuffer,
                                                    uint32_t          imageIndex,
                                                    const DepthState& depthState)
    {
        if (!pLogicalDevice || !pLogicalSwapchain || imageIndex >= pLogicalSwapchain->depthResolveDescriptorSets.size()
            || imageIndex >= pLogicalSwapchain->depthResolveFramebuffers.size())
            return;

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = pLogicalSwapchain->depthResolveSampler;
        imageInfo.imageView = getOrCreateTrackedDepthSampleView(pLogicalDevice, depthState);
        imageInfo.imageLayout = shouldKeepObservedDepthInGeneralLayout(depthState)
            ? VK_IMAGE_LAYOUT_GENERAL
            : getInternalDepthReadOnlyLayout(depthState.format);

        VkWriteDescriptorSet writeDescriptorSet = {};
        writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptorSet.dstSet = pLogicalSwapchain->depthResolveDescriptorSets[imageIndex];
        writeDescriptorSet.dstBinding = 0;
        writeDescriptorSet.descriptorCount = 1;
        writeDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeDescriptorSet.pImageInfo = &imageInfo;
        pLogicalDevice->vkd.UpdateDescriptorSets(pLogicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);

        VkImageMemoryBarrier sourceBarrier = {};
        sourceBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sourceBarrier.image = depthState.image;
        sourceBarrier.oldLayout = getObservedDepthAttachmentLayout(depthState);
        sourceBarrier.newLayout = shouldKeepObservedDepthInGeneralLayout(depthState)
            ? VK_IMAGE_LAYOUT_GENERAL
            : getInternalDepthReadOnlyLayout(depthState.format);
        sourceBarrier.srcAccessMask = getObservedDepthSourceAccess(depthState);
        sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
        sourceBarrier.subresourceRange.baseMipLevel = 0;
        sourceBarrier.subresourceRange.levelCount = 1;
        sourceBarrier.subresourceRange.baseArrayLayer = 0;
        sourceBarrier.subresourceRange.layerCount = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &sourceBarrier);

        VkImageMemoryBarrier resolveBarrier = {};
        resolveBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.image = pLogicalSwapchain->depthResolveImages[imageIndex];
        resolveBarrier.oldLayout = pLogicalSwapchain->depthResolveInitialized[imageIndex]
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolveBarrier.srcAccessMask = pLogicalSwapchain->depthResolveInitialized[imageIndex] ? VK_ACCESS_SHADER_READ_BIT : 0;
        resolveBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveBarrier.subresourceRange.baseMipLevel = 0;
        resolveBarrier.subresourceRange.levelCount = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               pLogicalSwapchain->depthResolveInitialized[imageIndex]
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkRenderPassBeginInfo renderPassBeginInfo = {};
        renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.renderPass = pLogicalSwapchain->depthResolveRenderPass;
        renderPassBeginInfo.framebuffer = pLogicalSwapchain->depthResolveFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = {pLogicalSwapchain->depthResolveExtent.width, pLogicalSwapchain->depthResolveExtent.height};
        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 1.0f;
        renderPassBeginInfo.clearValueCount = 1;
        renderPassBeginInfo.pClearValues = &clearValue;

        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        pLogicalDevice->vkd.CmdBindDescriptorSets(commandBuffer,
                                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  pLogicalSwapchain->depthResolvePipelineLayout,
                                                  0,
                                                  1,
                                                  &pLogicalSwapchain->depthResolveDescriptorSets[imageIndex],
                                                  0,
                                                  nullptr);
        pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pLogicalSwapchain->depthResolvePipeline);
        pLogicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);
        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        pLogicalSwapchain->depthResolveInitialized[imageIndex] = true;

        if (shouldKeepObservedDepthInGeneralLayout(depthState))
            return;

        sourceBarrier.oldLayout = getInternalDepthReadOnlyLayout(depthState.format);
        sourceBarrier.newLayout = getObservedDepthAttachmentLayout(depthState);
        sourceBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               getObservedDepthRestoreStages(depthState),
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &sourceBarrier);
    }

    std::vector<VkCommandBuffer> allocateCommandBuffer(LogicalDevice* pLogicalDevice, uint32_t count)
    {
        std::vector<VkCommandBuffer> commandBuffers(count);

        VkCommandBufferAllocateInfo allocInfo;
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.pNext              = nullptr;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = pLogicalDevice->commandPool;
        allocInfo.commandBufferCount = count;

        VkResult result = pLogicalDevice->vkd.AllocateCommandBuffers(pLogicalDevice->device, &allocInfo, commandBuffers.data());
        ASSERT_VULKAN(result);
        for (uint32_t i = 0; i < count; i++)
        {
            // initialize dispatch tables for commandBuffers since the are dispatchable objects
            initializeDispatchTable(commandBuffers[i], pLogicalDevice->device);
        }

        return commandBuffers;
    }
    void writeCommandBuffers(LogicalDevice*                                 pLogicalDevice,
                             LogicalSwapchain*                              pLogicalSwapchain,
                             std::vector<std::shared_ptr<vkShade::Effect>> effects,
                             std::vector<VkCommandBuffer>                   commandBuffers)
    {
        VkCommandBufferBeginInfo beginInfo = {};

        beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext            = nullptr;
        beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        const bool hasDepthResolveTarget = hasDepthResolveResources(pLogicalSwapchain, commandBuffers.size());

        for (uint32_t i = 0; i < commandBuffers.size(); i++)
        {
            const VkImageView  boundDepthImageView = hasDepthResolveTarget ? pLogicalSwapchain->depthResolveImageViews[i]
                                                                           : VK_NULL_HANDLE;
            const VkImageLayout boundDepthLayout   = hasDepthResolveTarget ? getDepthResolveReadOnlyLayout(pLogicalSwapchain)
                                                                           : VK_IMAGE_LAYOUT_UNDEFINED;
            for (auto& effect : effects)
            {
                effect->useDepthImage(i, boundDepthImageView, boundDepthLayout);
            }

            VkResult result = pLogicalDevice->vkd.BeginCommandBuffer(commandBuffers[i], &beginInfo);
            ASSERT_VULKAN(result);

            if (hasDepthResolveTarget && hasDepthState(pLogicalDevice->activeDepthState))
            {
                recordDepthResolveSnapshot(
                    pLogicalDevice, pLogicalSwapchain, commandBuffers[i], i, pLogicalDevice->activeDepthState);
            }

            for (uint32_t j = 0; j < effects.size(); j++)
            {
                effects[j]->applyEffect(i, commandBuffers[i]);
            }

            result = pLogicalDevice->vkd.EndCommandBuffer(commandBuffers[i]);
            ASSERT_VULKAN(result);
        }
    }

    void recordDepthResolveSnapshot(LogicalDevice*  pLogicalDevice,
                                    LogicalSwapchain* pLogicalSwapchain,
                                    VkCommandBuffer commandBuffer,
                                    uint32_t imageIndex,
                                    const DepthState& depthState)
    {
        if (!pLogicalDevice || !pLogicalSwapchain || !hasDepthState(depthState))
            return;
        if (pLogicalSwapchain->depthResolveImages.size() != pLogicalSwapchain->depthResolveImageViews.size()
            || pLogicalSwapchain->depthResolveImages.size() != pLogicalSwapchain->depthResolveMemories.size()
            || pLogicalSwapchain->depthResolveImages.size() != pLogicalSwapchain->depthResolveInitialized.size())
            return;
        if (imageIndex >= pLogicalSwapchain->depthResolveImageViews.size()
            || imageIndex >= pLogicalSwapchain->depthResolveImages.size()
            || imageIndex >= pLogicalSwapchain->depthResolveInitialized.size())
            return;

        auto metadataIt = pLogicalDevice->depthImageMetadata.find(depthState.image);
        if (metadataIt != pLogicalDevice->depthImageMetadata.end())
        {
            Logger::debug("depth snapshot source state: commandBuffer=" + convertToString(commandBuffer)
                          + " image=" + convertToString(depthState.image)
                          + " observedLayout=" + convertToString(depthState.observedLayout)
                          + " usage=" + convertToString(metadataIt->second.usage)
                          + " samples=" + convertToString(metadataIt->second.samples)
                          + " tiling=" + convertToString(metadataIt->second.tiling)
                          + " transient=" + std::string((metadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));
        }
        else
        {
            Logger::debug("depth snapshot source state: commandBuffer=" + convertToString(commandBuffer)
                          + " image=" + convertToString(depthState.image)
                          + " observedLayout=" + convertToString(depthState.observedLayout)
                          + " metadata=missing");
        }

        if (pLogicalSwapchain->depthResolveUsesShader)
        {
            recordDepthResolveSnapshotViaShader(pLogicalDevice, pLogicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.image               = depthState.image;
        const bool keepGeneralLayout = shouldKeepObservedDepthInGeneralLayout(depthState);
        memoryBarrier.oldLayout           = getObservedDepthAttachmentLayout(depthState);
        memoryBarrier.newLayout           = keepGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.srcAccessMask       = getObservedDepthSourceAccess(depthState);
        memoryBarrier.dstAccessMask       = keepGeneralLayout
                                                ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
                                                : VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &memoryBarrier);

        VkImageMemoryBarrier resolveBarrier = {};
        resolveBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.pNext               = nullptr;
        resolveBarrier.image               = pLogicalSwapchain->depthResolveImages[imageIndex];
        resolveBarrier.oldLayout = pLogicalSwapchain->depthResolveInitialized[imageIndex]
                                       ? getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat)
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveBarrier.srcAccessMask       = pLogicalSwapchain->depthResolveInitialized[imageIndex] ? VK_ACCESS_SHADER_READ_BIT : 0;
        resolveBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveBarrier.subresourceRange.baseMipLevel   = 0;
        resolveBarrier.subresourceRange.levelCount     = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               pLogicalSwapchain->depthResolveInitialized[imageIndex]
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        copyRegion.srcSubresource.mipLevel       = 0;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount     = 1;
        copyRegion.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        copyRegion.dstSubresource.mipLevel       = 0;
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount     = 1;
        copyRegion.extent.width                  = depthState.extent.width;
        copyRegion.extent.height                 = depthState.extent.height;
        copyRegion.extent.depth                  = 1;

        pLogicalDevice->vkd.CmdCopyImage(commandBuffer,
                                         depthState.image,
                                         keepGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         pLogicalSwapchain->depthResolveImages[imageIndex],
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1,
                                         &copyRegion);

        resolveBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveBarrier.newLayout     = getInternalDepthReadOnlyLayout(pLogicalSwapchain->depthResolveFormat);
        resolveBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        pLogicalSwapchain->depthResolveInitialized[imageIndex] = true;

        if (!keepGeneralLayout)
        {
            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            memoryBarrier.newLayout     = getObservedDepthAttachmentLayout(depthState);
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            memoryBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
            pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                   getObservedDepthRestoreStages(depthState),
                                                   0,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   1,
                                                   &memoryBarrier);
        }
    }

    std::vector<VkSemaphore> createSemaphores(LogicalDevice* pLogicalDevice, uint32_t count)
    {
        std::vector<VkSemaphore> semaphores(count);
        VkSemaphoreCreateInfo    info;
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = nullptr;
        info.flags = 0;

        for (uint32_t i = 0; i < count; i++)
        {
            pLogicalDevice->vkd.CreateSemaphore(pLogicalDevice->device, &info, nullptr, &semaphores[i]);
        }
        return semaphores;
    }

} // namespace vkShade
