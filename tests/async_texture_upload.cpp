#include "overlay/async_texture_upload.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <type_traits>
#include <vector>

using namespace vkShade;

template<typename T>
static T handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}

struct Driver
{
    VkResult                     createResult = VK_SUCCESS;
    VkResult                     submitResult = VK_SUCCESS;
    VkResult                     waitResult   = VK_SUCCESS;
    uint64_t                     nextFence    = 1;
    std::map<VkFence, bool>      signaled;
    std::vector<VkFence>         destroyedFences;
    std::vector<VkCommandBuffer> freedCommands;
    std::vector<VkBuffer>        destroyedBuffers;
    std::vector<VkDeviceMemory>  freedMemory;
    bool                         nonblocking = true;
    bool                         validSubmit = true;
};

static Driver driver;

static VkResult VKAPI_CALL createFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence)
{
    if (driver.createResult != VK_SUCCESS)
        return driver.createResult;
    *fence                  = handle<VkFence>(driver.nextFence++);
    driver.signaled[*fence] = false;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL submit(VkQueue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
{
    driver.validSubmit &= count == 1 && info->commandBufferCount == 1 && info->pCommandBuffers && driver.signaled.contains(fence);
    return driver.submitResult;
}

static VkResult VKAPI_CALL waitForFences(VkDevice, uint32_t count, const VkFence* fence, VkBool32, uint64_t timeout)
{
    driver.nonblocking &= count == 1 && timeout == 0;
    if (driver.waitResult != VK_SUCCESS)
        return driver.waitResult;
    return driver.signaled.at(*fence) ? VK_SUCCESS : VK_TIMEOUT;
}

static void VKAPI_CALL destroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*)
{
    driver.destroyedFences.push_back(fence);
    driver.signaled.erase(fence);
}

static void VKAPI_CALL freeCommands(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer* buffers)
{
    driver.freedCommands.insert(driver.freedCommands.end(), buffers, buffers + count);
}

static void VKAPI_CALL destroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*)
{
    driver.destroyedBuffers.push_back(buffer);
}

static void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*)
{
    driver.freedMemory.push_back(memory);
}

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    DeviceDispatch dispatch;
    dispatch.CreateFence        = createFence;
    dispatch.WaitForFences      = waitForFences;
    dispatch.DestroyFence       = destroyFence;
    dispatch.FreeCommandBuffers = freeCommands;
    dispatch.DestroyBuffer      = destroyBuffer;
    dispatch.FreeMemory         = freeMemory;
    AsyncTextureUploads uploads(handle<VkDevice>(1), handle<VkQueue>(1), dispatch, submit);
    const auto          pool     = handle<VkCommandPool>(1);
    const auto          cmd1     = handle<VkCommandBuffer>(1);
    const auto          cmd2     = handle<VkCommandBuffer>(2);
    const auto          buf1     = handle<VkBuffer>(1);
    const auto          buf2     = handle<VkBuffer>(2);
    const auto          mem1     = handle<VkDeviceMemory>(1);
    const auto          mem2     = handle<VkDeviceMemory>(2);
    int                 texture1 = 0;
    int                 texture2 = 0;
    bool                ok       = true;

    ok &= check(uploads.submit(pool, cmd1, buf1, mem1, &texture1) == VK_SUCCESS && uploads.submit(pool, cmd2, buf2, mem2, &texture2) == VK_SUCCESS
                    && driver.validSubmit,
                "Uploads must be submitted with their own fences");
    ok &= check(uploads.collect() == VK_SUCCESS && uploads.pending(&texture1) && uploads.pending(&texture2) && driver.freedCommands.empty()
                    && driver.destroyedBuffers.empty() && driver.freedMemory.empty(),
                "Pending uploads must retain their resources");
    driver.signaled[handle<VkFence>(2)] = true;
    ok &= check(uploads.collect() == VK_SUCCESS && uploads.pending(&texture1) && !uploads.pending(&texture2)
                    && driver.freedCommands == std::vector<VkCommandBuffer>{cmd2} && driver.destroyedBuffers == std::vector<VkBuffer>{buf2}
                    && driver.freedMemory == std::vector<VkDeviceMemory>{mem2},
                "Only a completed upload may release its resources");
    driver.signaled[handle<VkFence>(1)] = true;
    ok &= check(uploads.collect() == VK_SUCCESS && !uploads.pending(&texture1) && driver.nonblocking,
                "Fence polling must never block and completed uploads must be reclaimed");

    ok &= check(uploads.submit(pool, cmd1, buf1, mem1, &texture1) == VK_SUCCESS && uploads.submit(pool, cmd2, buf2, mem2, &texture1) == VK_SUCCESS,
                "A texture may have more than one upload in flight");
    driver.signaled[handle<VkFence>(3)] = true;
    ok &= check(uploads.collect() == VK_SUCCESS && uploads.pending(&texture1), "A texture must remain live until every upload completes");
    driver.waitResult = VK_ERROR_DEVICE_LOST;
    ok &= check(uploads.collect() == VK_ERROR_DEVICE_LOST && uploads.pending(&texture1),
                "A failed fence query must report the error and retain the upload");
    driver.waitResult                   = VK_SUCCESS;
    driver.signaled[handle<VkFence>(4)] = true;
    ok &= check(uploads.collect() == VK_SUCCESS && !uploads.pending(&texture1), "The last completed upload must release the texture");

    const auto released = driver.freedCommands.size();
    driver.createResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    ok &= check(uploads.submit(pool, cmd1, buf1, mem1, &texture1) == VK_ERROR_OUT_OF_HOST_MEMORY && !uploads.pending(&texture1)
                    && driver.freedCommands.size() == released,
                "Fence creation failure must leave resources with the caller");
    driver.createResult            = VK_SUCCESS;
    driver.submitResult            = VK_ERROR_DEVICE_LOST;
    const auto fencesBeforeFailure = driver.destroyedFences.size();
    ok &= check(uploads.submit(pool, cmd1, buf1, mem1, &texture1) == VK_ERROR_DEVICE_LOST && !uploads.pending(&texture1)
                    && driver.freedCommands.size() == released && driver.destroyedFences.size() == fencesBeforeFailure + 1,
                "Submission failure must destroy its fence and leave resources with the caller");
    driver.submitResult = VK_SUCCESS;
    ok &= check(uploads.submit(pool, cmd1, buf1, mem1, &texture1) == VK_SUCCESS, "Final upload must submit");
    uploads.clearAfterIdle();
    ok &= check(!uploads.pending(&texture1) && driver.freedCommands.size() == released + 1 && driver.signaled.empty(),
                "Shutdown after queue idle must release all remaining resources");
    return ok ? 0 : 1;
}
