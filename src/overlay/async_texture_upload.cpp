#include "async_texture_upload.hpp"

#include <algorithm>
#include <new>

namespace vkShade
{
    AsyncTextureUploads::AsyncTextureUploads(VkDevice device, VkQueue queue, DeviceDispatch& dispatch, PFN_vkQueueSubmit submit) :
        device(device), queue(queue), dispatch(dispatch), queueSubmit(submit)
    {
    }

    VkResult AsyncTextureUploads::submit(
        VkCommandPool pool, VkCommandBuffer commandBuffer, VkBuffer stagingBuffer, VkDeviceMemory stagingMemory, const void* texture)
    {
        try
        {
            uploads.push_back({pool, commandBuffer, stagingBuffer, stagingMemory, texture, VK_NULL_HANDLE});
        }
        catch (const std::bad_alloc&)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkResult result             = dispatch.CreateFence(device, &fenceInfo, nullptr, &uploads.back().fence);
        if (result == VK_SUCCESS)
        {
            VkSubmitInfo submitInfo       = {};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &commandBuffer;
            result                        = queueSubmit(queue, 1, &submitInfo, uploads.back().fence);
            if (result != VK_SUCCESS)
                dispatch.DestroyFence(device, uploads.back().fence, nullptr);
        }
        if (result != VK_SUCCESS)
            uploads.pop_back();
        return result;
    }

    VkResult AsyncTextureUploads::collect()
    {
        VkResult failure = VK_SUCCESS;
        for (auto it = uploads.begin(); it != uploads.end();)
        {
            const VkResult result = dispatch.WaitForFences(device, 1, &it->fence, VK_TRUE, 0);
            if (result == VK_SUCCESS)
            {
                release(*it);
                it = uploads.erase(it);
            }
            else
            {
                if (result != VK_TIMEOUT)
                    failure = result;
                ++it;
            }
        }
        return failure;
    }

    bool AsyncTextureUploads::pending(const void* texture) const
    {
        return std::any_of(uploads.begin(), uploads.end(), [texture](const Upload& upload) { return upload.texture == texture; });
    }

    void AsyncTextureUploads::release(const Upload& upload)
    {
        dispatch.DestroyFence(device, upload.fence, nullptr);
        dispatch.FreeCommandBuffers(device, upload.pool, 1, &upload.commandBuffer);
        dispatch.DestroyBuffer(device, upload.stagingBuffer, nullptr);
        dispatch.FreeMemory(device, upload.stagingMemory, nullptr);
    }

    void AsyncTextureUploads::clearAfterIdle()
    {
        for (const auto& upload : uploads)
            release(upload);
        uploads.clear();
    }
} // namespace vkShade
