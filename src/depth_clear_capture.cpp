#include "depth_clear_capture.hpp"

#include "format.hpp"
#include "logical_swapchain.hpp"
#include "memory.hpp"

namespace vkShade
{
    DepthBackup::~DepthBackup()
    {
        if (image != VK_NULL_HANDLE)
            device->vkd.DestroyImage(device->device, image, nullptr);
        if (memory != VK_NULL_HANDLE)
            device->vkd.FreeMemory(device->device, memory, nullptr);
    }

    bool canCopyDepthBeforeClear(const DepthState& depth)
    {
        if (!depth.capturable || depth.image == VK_NULL_HANDLE || depth.imageView == VK_NULL_HANDLE || depth.extent.width == 0
            || depth.extent.height == 0)
            return false;
        switch (depth.observedLayout)
        {
            case VK_IMAGE_LAYOUT_GENERAL:
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return true;
            default: return false;
        }
    }

    std::shared_ptr<DepthBackup> createDepthBackup(LogicalDevice* device, const DepthState& depth)
    {
        if (!device || !canCopyDepthBeforeClear(depth))
            return {};
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageFormatProperties     properties{};
        if (device->vki.GetPhysicalDeviceImageFormatProperties(
                device->physicalDevice, depth.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &properties)
                != VK_SUCCESS
            || depth.extent.width > properties.maxExtent.width || depth.extent.height > properties.maxExtent.height)
            return {};

        auto backup          = std::make_shared<DepthBackup>();
        backup->device       = device;
        backup->format       = depth.format;
        backup->extent       = depth.extent;
        backup->extent.depth = 1;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = depth.format;
        imageInfo.extent        = backup->extent;
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = usage;
        imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (device->vkd.CreateImage(device->device, &imageInfo, nullptr, &backup->image) != VK_SUCCESS)
        {
            backup->image = VK_NULL_HANDLE;
            return {};
        }

        VkMemoryRequirements requirements{};
        device->vkd.GetImageMemoryRequirements(device->device, backup->image, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize  = requirements.size;
        allocateInfo.memoryTypeIndex = findMemoryTypeIndex(device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocateInfo.memoryTypeIndex >= VK_MAX_MEMORY_TYPES
            || device->vkd.AllocateMemory(device->device, &allocateInfo, nullptr, &backup->memory) != VK_SUCCESS)
        {
            backup->memory = VK_NULL_HANDLE;
            return {};
        }
        if (device->vkd.BindImageMemory(device->device, backup->image, backup->memory, 0) != VK_SUCCESS)
            return {};
        return backup;
    }

    void recordDepthBackup(LogicalDevice* device, VkCommandBuffer commandBuffer, const DepthState& depth, const DepthBackup& backup)
    {
        const VkImageAspectFlags aspect =
            isStencilFormat(depth.format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkMemoryBarrier priorReads{};
        priorReads.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        priorReads.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        priorReads.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        VkImageMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers)
        {
            barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange    = {aspect, 0, 1, 0, 1};
        }
        barriers[0].image         = depth.image;
        barriers[0].oldLayout     = depth.observedLayout;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image         = backup.image;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        device->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &priorReads, 0, nullptr, 2, barriers);

        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent         = backup.extent;
        device->vkd.CmdCopyImage(
            commandBuffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, backup.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout     = depth.observedLayout;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        device->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    }

    void recordDepthBackupToResolve(
        LogicalDevice* device, LogicalSwapchain* swapchain, VkCommandBuffer commandBuffer, uint32_t imageIndex, const DepthBackup& backup)
    {
        const VkImageAspectFlags aspect =
            isStencilFormat(backup.format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkImageMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers)
        {
            barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange    = {aspect, 0, 1, 0, 1};
        }
        barriers[0].image         = backup.image;
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image         = swapchain->depthResolveImages[imageIndex];
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        device->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent         = backup.extent;
        device->vkd.CmdCopyImage(commandBuffer,
                                 backup.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 swapchain->depthResolveImages[imageIndex],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1,
                                 &copy);
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        device->vkd.CmdPipelineBarrier(commandBuffer,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       0,
                                       0,
                                       nullptr,
                                       0,
                                       nullptr,
                                       1,
                                       &barriers[1]);
        swapchain->depthResolveInitialized[imageIndex] = true;
    }
} // namespace vkShade
