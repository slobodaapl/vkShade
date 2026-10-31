#include "command_buffer.hpp"

#include <algorithm>

#include "format.hpp"
#include "image_view.hpp"
#include "logger.hpp"
#include "logical_swapchain.hpp"
#include "util.hpp"

#include <cstdlib>
#include <string>

namespace vkShade
{
    static bool hasDepthResolveResources(const LogicalSwapchain* pLogicalSwapchain, size_t commandBufferCount)
    {
        return pLogicalSwapchain != nullptr && pLogicalSwapchain->depthResolveImages.size() == commandBufferCount
               && pLogicalSwapchain->depthResolveImageViews.size() == commandBufferCount
               && pLogicalSwapchain->depthResolveMemories.size() == commandBufferCount
               && pLogicalSwapchain->depthResolveInitialized.size() == commandBufferCount;
    }

    // [DEBUG-TAG: depth-resolve-trace-20260423]
    static bool isDepthResolveTraceEnabled()
    {
        const char* value = std::getenv("VKSHADE_DEBUG_TRACE_DEPTH_RESOLVE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }

    // [DEBUG-TAG: depth-resolve-constant-20260423]
    static bool isDepthResolveConstantTestEnabled()
    {
        const char* value = std::getenv("VKSHADE_DEBUG_DEPTH_RESOLVE_CONSTANT");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }

    static bool hasDepthState(const DepthState& state)
    {
        return state.image != VK_NULL_HANDLE && state.imageView != VK_NULL_HANDLE && state.format != VK_FORMAT_UNDEFINED;
    }

    static VkImageLayout getObservedDepthAttachmentLayout(const DepthState& depthState)
    {
        return depthState.observedLayout != VK_IMAGE_LAYOUT_UNDEFINED ? depthState.observedLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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

    static VkImageLayout getDepthResolveReadOnlyLayout(const LogicalSwapchain* pLogicalSwapchain)
    {
        if (!pLogicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    static void recordDepthResolveSnapshotViaShader(LogicalDevice*    pLogicalDevice,
                                                    LogicalSwapchain* pLogicalSwapchain,
                                                    VkCommandBuffer   commandBuffer,
                                                    uint32_t          imageIndex,
                                                    const DepthState& depthState)
    {
        if (!pLogicalDevice || !pLogicalSwapchain)
            return;
        if (imageIndex >= pLogicalSwapchain->depthResolveDescriptorSets.size() || imageIndex >= pLogicalSwapchain->depthResolveFramebuffers.size())
        {
            if (isDepthResolveTraceEnabled())
                Logger::debug("depth resolve trace shader skip: imageIndex=" + std::to_string(imageIndex)
                              + " descriptorSets=" + std::to_string(pLogicalSwapchain->depthResolveDescriptorSets.size())
                              + " framebuffers=" + std::to_string(pLogicalSwapchain->depthResolveFramebuffers.size()));
            return;
        }

        if (isDepthResolveTraceEnabled())
            Logger::debug("depth resolve trace shader begin: cb=" + convertToString(commandBuffer) + " imageIndex=" + std::to_string(imageIndex)
                          + " srcImage=" + convertToString(depthState.image) + " srcView=" + convertToString(depthState.imageView)
                          + " srcLayout=" + convertToString(getObservedDepthAttachmentLayout(depthState))
                          + " dstImage=" + convertToString(pLogicalSwapchain->depthResolveImages[imageIndex])
                          + " dstInitialized=" + std::string(pLogicalSwapchain->depthResolveInitialized[imageIndex] ? "true" : "false"));

        VkImageMemoryBarrier sourceBarrier = {};
        sourceBarrier.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sourceBarrier.image                = depthState.image;
        sourceBarrier.oldLayout            = getObservedDepthAttachmentLayout(depthState);
        sourceBarrier.newLayout =
            shouldKeepObservedDepthInGeneralLayout(depthState) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sourceBarrier.srcAccessMask       = getObservedDepthSourceAccess(depthState);
        sourceBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
        sourceBarrier.subresourceRange.baseMipLevel   = 0;
        sourceBarrier.subresourceRange.levelCount     = 1;
        sourceBarrier.subresourceRange.baseArrayLayer = 0;
        sourceBarrier.subresourceRange.layerCount     = 1;

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

        VkImageMemoryBarrier resolveBarrier            = {};
        resolveBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.image                           = pLogicalSwapchain->depthResolveImages[imageIndex];
        resolveBarrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolveBarrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        resolveBarrier.dstAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveBarrier.subresourceRange.baseMipLevel   = 0;
        resolveBarrier.subresourceRange.levelCount     = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                   | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkRenderPassBeginInfo renderPassBeginInfo = {};
        renderPassBeginInfo.sType                 = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.renderPass            = pLogicalSwapchain->depthResolveRenderPass;
        renderPassBeginInfo.framebuffer           = pLogicalSwapchain->depthResolveFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset     = {0, 0};
        renderPassBeginInfo.renderArea.extent     = {pLogicalSwapchain->depthResolveExtent.width, pLogicalSwapchain->depthResolveExtent.height};
        VkClearValue clearValue                   = {};
        clearValue.color.float32[0]               = 1.0f;
        renderPassBeginInfo.clearValueCount       = 1;
        renderPassBeginInfo.pClearValues          = &clearValue;

        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        if (!isDepthResolveConstantTestEnabled())
        {
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
        }
        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        pLogicalSwapchain->depthResolveInitialized[imageIndex] = true;

        if (isDepthResolveTraceEnabled())
            Logger::debug("depth resolve trace shader end: cb=" + convertToString(commandBuffer) + " imageIndex=" + std::to_string(imageIndex)
                          + " dstImage=" + convertToString(pLogicalSwapchain->depthResolveImages[imageIndex]));

        if (shouldKeepObservedDepthInGeneralLayout(depthState))
            return;

        sourceBarrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sourceBarrier.newLayout     = getObservedDepthAttachmentLayout(depthState);
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
    void writeCommandBuffers(LogicalDevice*                                pLogicalDevice,
                             LogicalSwapchain*                             pLogicalSwapchain,
                             std::vector<std::shared_ptr<vkShade::Effect>> effects,
                             std::vector<VkCommandBuffer>                  commandBuffers)
    {
        VkCommandBufferBeginInfo beginInfo = {};

        beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext            = nullptr;
        beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        const bool hasDepthResolveTarget = hasDepthResolveResources(pLogicalSwapchain, commandBuffers.size());

        for (uint32_t i = 0; i < commandBuffers.size(); i++)
        {
            const VkImageView   boundDepthImageView = hasDepthResolveTarget ? pLogicalSwapchain->depthResolveImageViews[i] : VK_NULL_HANDLE;
            const VkImageLayout boundDepthLayout =
                hasDepthResolveTarget ? getDepthResolveReadOnlyLayout(pLogicalSwapchain) : VK_IMAGE_LAYOUT_UNDEFINED;
            for (auto& effect : effects)
            {
                effect->useDepthImage(i, boundDepthImageView, boundDepthLayout);
            }

            VkResult result = pLogicalDevice->vkd.BeginCommandBuffer(commandBuffers[i], &beginInfo);
            ASSERT_VULKAN(result);

            if (pLogicalDevice->activeDepthStatePresentTimeFallback && hasDepthResolveTarget)
            {
                // [DEBUG-TAG: present-depth-fallback-20260424]
                if (isDepthResolveTraceEnabled())
                    Logger::debug("depth resolve trace present fallback record: cb=" + convertToString(commandBuffers[i])
                                  + " imageIndex=" + std::to_string(i) + " depthImage=" + convertToString(pLogicalDevice->activeDepthState.image)
                                  + " depthView=" + convertToString(pLogicalDevice->activeDepthState.imageView));
                recordDepthResolveSnapshot(pLogicalDevice, pLogicalSwapchain, commandBuffers[i], i, pLogicalDevice->activeDepthState);
            }

            for (uint32_t j = 0; j < effects.size(); j++)
            {
                effects[j]->applyEffect(i, commandBuffers[i]);
            }

            result = pLogicalDevice->vkd.EndCommandBuffer(commandBuffers[i]);
            ASSERT_VULKAN(result);
        }
    }

    void recordDepthResolveSnapshot(LogicalDevice*    pLogicalDevice,
                                    LogicalSwapchain* pLogicalSwapchain,
                                    VkCommandBuffer   commandBuffer,
                                    uint32_t          imageIndex,
                                    const DepthState& depthState)
    {
        if (!pLogicalDevice || !pLogicalSwapchain || !hasDepthState(depthState))
        {
            if (isDepthResolveTraceEnabled())
                Logger::debug("depth resolve trace snapshot skip: missing device/swapchain/depth");
            return;
        }
        if (pLogicalSwapchain->depthResolveImages.size() != pLogicalSwapchain->depthResolveImageViews.size()
            || pLogicalSwapchain->depthResolveImages.size() != pLogicalSwapchain->depthResolveMemories.size()
            || pLogicalSwapchain->depthResolveImages.size() != pLogicalSwapchain->depthResolveInitialized.size())
        {
            if (isDepthResolveTraceEnabled())
                Logger::debug("depth resolve trace snapshot skip: resource size mismatch images="
                              + std::to_string(pLogicalSwapchain->depthResolveImages.size())
                              + " views=" + std::to_string(pLogicalSwapchain->depthResolveImageViews.size())
                              + " memories=" + std::to_string(pLogicalSwapchain->depthResolveMemories.size())
                              + " initialized=" + std::to_string(pLogicalSwapchain->depthResolveInitialized.size()));
            return;
        }
        if (imageIndex >= pLogicalSwapchain->depthResolveImageViews.size() || imageIndex >= pLogicalSwapchain->depthResolveImages.size()
            || imageIndex >= pLogicalSwapchain->depthResolveInitialized.size())
        {
            if (isDepthResolveTraceEnabled())
                Logger::debug("depth resolve trace snapshot skip: imageIndex out of range imageIndex=" + std::to_string(imageIndex)
                              + " images=" + std::to_string(pLogicalSwapchain->depthResolveImages.size())
                              + " initialized=" + std::to_string(pLogicalSwapchain->depthResolveInitialized.size()));
            return;
        }

        if (isDepthResolveTraceEnabled())
            Logger::debug("depth resolve trace snapshot begin: cb=" + convertToString(commandBuffer) + " imageIndex=" + std::to_string(imageIndex)
                          + " usesShader=" + std::string(pLogicalSwapchain->depthResolveUsesShader ? "true" : "false")
                          + " srcImage=" + convertToString(depthState.image) + " srcView=" + convertToString(depthState.imageView)
                          + " srcLayout=" + convertToString(depthState.observedLayout)
                          + " dstImage=" + convertToString(pLogicalSwapchain->depthResolveImages[imageIndex]));

        if (pLogicalSwapchain->depthResolveUsesShader)
        {
            recordDepthResolveSnapshotViaShader(pLogicalDevice, pLogicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType          = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext          = nullptr;
        memoryBarrier.image          = depthState.image;
        const bool keepGeneralLayout = shouldKeepObservedDepthInGeneralLayout(depthState);
        memoryBarrier.oldLayout      = getObservedDepthAttachmentLayout(depthState);
        memoryBarrier.newLayout      = keepGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.srcAccessMask  = getObservedDepthSourceAccess(depthState);
        memoryBarrier.dstAccessMask =
            keepGeneralLayout ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) : VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, getObservedDepthSourceStages(depthState), VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        VkImageMemoryBarrier resolveBarrier            = {};
        resolveBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.pNext                           = nullptr;
        resolveBarrier.image                           = pLogicalSwapchain->depthResolveImages[imageIndex];
        resolveBarrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveBarrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask     = isStencilFormat(pLogicalSwapchain->depthResolveFormat)
                                                             ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                             : VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveBarrier.subresourceRange.baseMipLevel   = 0;
        resolveBarrier.subresourceRange.levelCount     = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                   | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkImageCopy copyRegion                   = {};
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
        resolveBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
