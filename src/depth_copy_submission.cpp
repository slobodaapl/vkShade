#include "depth_copy_submission.hpp"

#include "command_buffer.hpp"
#include "depth_clear_capture.hpp"
#include "depth_resolve_resources.hpp"

#include <new>
#include <utility>

namespace vkShade
{
    void releaseDepthCopySubmission(LogicalDevice* device, LogicalSwapchain::DepthCopySubmission& submission)
    {
        if (submission.commandBuffer != VK_NULL_HANDLE)
            device->vkd.FreeCommandBuffers(device->device, device->commandPool, 1, &submission.commandBuffer);
        if (submission.fence != VK_NULL_HANDLE)
            device->vkd.DestroyFence(device->device, submission.fence, nullptr);
        submission = {};
    }

    VkResult depthResourcesIdle(LogicalDevice* device, const LogicalSwapchain* swapchain, bool& idle)
    {
        idle             = true;
        const auto check = [&](const auto& submissions) {
            for (const auto& submission : submissions)
            {
                if (!submission.pending || !submission.referencesDepth)
                    continue;
                const auto result = device->vkd.WaitForFences(device->device, 1, &submission.fence, VK_TRUE, 0);
                if (result == VK_TIMEOUT)
                    idle = false;
                else if (result != VK_SUCCESS)
                    return result;
            }
            return VK_SUCCESS;
        };
        const auto result = check(swapchain->depthCopySubmissions);
        return result == VK_SUCCESS ? check(swapchain->retiredDepthCopySubmissions) : result;
    }

    VkResult prepareDepthCopySubmission(LogicalDevice*                      device,
                                        LogicalSwapchain*                   swapchain,
                                        uint32_t                            imageIndex,
                                        const DepthState&                   fallback,
                                        const std::shared_ptr<DepthBackup>& backup,
                                        bool                                recordCopy,
                                        std::optional<uint32_t>             previousImage)
    {
        if (swapchain->depthCopySubmissions.size() != swapchain->imageCount)
            swapchain->depthCopySubmissions.resize(swapchain->imageCount);
        for (auto it = swapchain->retiredDepthCopySubmissions.begin(); it != swapchain->retiredDepthCopySubmissions.end();)
        {
            const auto result = device->vkd.WaitForFences(device->device, 1, &it->fence, VK_TRUE, 0);
            if (result == VK_SUCCESS)
            {
                releaseDepthCopySubmission(device, *it);
                it = swapchain->retiredDepthCopySubmissions.erase(it);
            }
            else if (result == VK_TIMEOUT)
                ++it;
            else
                return result;
        }
        auto& submission = swapchain->depthCopySubmissions[imageIndex];
        if (submission.pending)
        {
            const auto result = device->vkd.WaitForFences(device->device, 1, &submission.fence, VK_TRUE, 0);
            if (result == VK_TIMEOUT)
            {
                try
                {
                    swapchain->retiredDepthCopySubmissions.push_back(std::move(submission));
                }
                catch (const std::bad_alloc&)
                {
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                }
                submission = {};
            }
            else if (result != VK_SUCCESS)
                return result;
            submission.pending = false;
        }
        submission.backup.reset();
        submission.referencesDepth = false;
        if (recordCopy && submission.commandBuffer != VK_NULL_HANDLE)
        {
            const auto result = device->vkd.ResetCommandBuffer(submission.commandBuffer, 0);
            if (result != VK_SUCCESS)
                return result;
        }
        if (submission.fence == VK_NULL_HANDLE)
        {
            VkFenceCreateInfo info{};
            info.sType        = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            const auto result = device->vkd.CreateFence(device->device, &info, nullptr, &submission.fence);
            if (result != VK_SUCCESS)
                return result;
        }
        auto result = device->vkd.ResetFences(device->device, 1, &submission.fence);
        if (result != VK_SUCCESS)
            return result;
        if (!recordCopy)
            return VK_SUCCESS;
        if (submission.commandBuffer == VK_NULL_HANDLE)
        {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool        = device->commandPool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            result                      = device->vkd.AllocateCommandBuffers(device->device, &allocate, &submission.commandBuffer);
            if (result != VK_SUCCESS)
                return result;
            initializeDispatchTable(submission.commandBuffer, device->device);
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result      = device->vkd.BeginCommandBuffer(submission.commandBuffer, &begin);
        if (result != VK_SUCCESS)
            return result;
        if (previousImage)
            recordPreviousDepthResolve(device, swapchain, submission.commandBuffer, *previousImage, imageIndex);
        else if (backup && !swapchain->depthResolveUsesShader && backup->format == swapchain->depthResolveFormat
                 && backup->extent.width == swapchain->depthResolveExtent.width && backup->extent.height == swapchain->depthResolveExtent.height)
        {
            recordDepthBackupToResolve(device, swapchain, submission.commandBuffer, imageIndex, *backup);
            submission.backup = backup;
        }
        else
            recordDepthResolveSnapshot(device, swapchain, submission.commandBuffer, imageIndex, fallback);
        return device->vkd.EndCommandBuffer(submission.commandBuffer);
    }
} // namespace vkShade
