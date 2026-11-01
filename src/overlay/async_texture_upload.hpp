#pragma once

#include "vkdispatch.hpp"

#include <vector>

namespace vkShade
{
    class AsyncTextureUploads
    {
    public:
        AsyncTextureUploads(VkDevice device, VkQueue queue, DeviceDispatch& dispatch, PFN_vkQueueSubmit submit);

        VkResult submit(VkCommandPool pool, VkCommandBuffer commandBuffer, VkBuffer stagingBuffer, VkDeviceMemory stagingMemory, const void* texture);
        VkResult collect();
        bool     pending(const void* texture) const;
        void     clearAfterIdle();

    private:
        struct Upload
        {
            VkCommandPool   pool;
            VkCommandBuffer commandBuffer;
            VkBuffer        stagingBuffer;
            VkDeviceMemory  stagingMemory;
            const void*     texture = nullptr;
            VkFence         fence;
        };

        void release(const Upload& upload);

        VkDevice            device;
        VkQueue             queue;
        DeviceDispatch&     dispatch;
        PFN_vkQueueSubmit   queueSubmit;
        std::vector<Upload> uploads;
    };
} // namespace vkShade
