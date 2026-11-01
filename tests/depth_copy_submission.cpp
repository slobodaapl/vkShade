#include "depth_copy_submission.hpp"
#include "overlay/imgui_overlay.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>

using namespace vkShade;

namespace vkShade
{
    ImGuiOverlay::~ImGuiOverlay() = default;
    DepthBackup::~DepthBackup()   = default;
} // namespace vkShade

template<typename T>
static T handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}

struct FakeCommand
{
    void* dispatch = nullptr;
};

static std::vector<std::unique_ptr<FakeCommand>> commands;
static std::map<VkFence, bool>                   fences;
static uintptr_t                                 nextFence      = 1;
static int                                       freedCommands  = 0;
static int                                       resetCommands  = 0;
static int                                       fallbackCopies = 0;
static int                                       backupCopies   = 0;
static int                                       previousCopies = 0;
static bool                                      nonblocking    = true;
static VkResult                                  waitError      = VK_SUCCESS;

namespace vkShade
{
    void recordDepthResolveSnapshot(LogicalDevice*, LogicalSwapchain*, VkCommandBuffer, uint32_t, const DepthState&)
    {
        ++fallbackCopies;
    }
    void recordDepthBackupToResolve(LogicalDevice*, LogicalSwapchain*, VkCommandBuffer, uint32_t, const DepthBackup&)
    {
        ++backupCopies;
    }
    void recordPreviousDepthResolve(LogicalDevice*, LogicalSwapchain*, VkCommandBuffer, uint32_t, uint32_t)
    {
        ++previousCopies;
    }
} // namespace vkShade

static VkResult VKAPI_CALL waitForFences(VkDevice, uint32_t count, const VkFence* fence, VkBool32, uint64_t timeout)
{
    nonblocking &= count == 1 && timeout == 0;
    if (waitError != VK_SUCCESS)
        return waitError;
    return fences.at(*fence) ? VK_SUCCESS : VK_TIMEOUT;
}

static VkResult VKAPI_CALL createFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence)
{
    *fence         = handle<VkFence>(nextFence++);
    fences[*fence] = false;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*)
{
    fences.erase(fence);
}
static VkResult VKAPI_CALL resetFences(VkDevice, uint32_t, const VkFence* fence)
{
    fences[*fence] = false;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL allocateCommands(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* command)
{
    commands.push_back(std::make_unique<FakeCommand>());
    *command = reinterpret_cast<VkCommandBuffer>(commands.back().get());
    return VK_SUCCESS;
}
static void VKAPI_CALL freeCommands(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer*)
{
    freedCommands += count;
}
static VkResult VKAPI_CALL resetCommand(VkCommandBuffer, VkCommandBufferResetFlags)
{
    ++resetCommands;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL beginCommand(VkCommandBuffer, const VkCommandBufferBeginInfo*)
{
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL endCommand(VkCommandBuffer)
{
    return VK_SUCCESS;
}

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    void*         deviceDispatch = handle<void*>(123);
    LogicalDevice device{};
    device.device                     = reinterpret_cast<VkDevice>(&deviceDispatch);
    device.vkd.WaitForFences          = waitForFences;
    device.vkd.CreateFence            = createFence;
    device.vkd.DestroyFence           = destroyFence;
    device.vkd.ResetFences            = resetFences;
    device.vkd.AllocateCommandBuffers = allocateCommands;
    device.vkd.FreeCommandBuffers     = freeCommands;
    device.vkd.ResetCommandBuffer     = resetCommand;
    device.vkd.BeginCommandBuffer     = beginCommand;
    device.vkd.EndCommandBuffer       = endCommand;
    LogicalSwapchain swapchain{};
    swapchain.pLogicalDevice     = &device;
    swapchain.imageCount         = 2;
    swapchain.depthResolveFormat = VK_FORMAT_D32_SFLOAT;
    swapchain.depthResolveExtent = {128, 128, 1};
    DepthState depth{};
    auto       backup                          = std::make_shared<DepthBackup>();
    backup->format                             = VK_FORMAT_D32_SFLOAT;
    backup->extent                             = {128, 128, 1};
    std::weak_ptr<DepthBackup> backupReference = backup;
    bool                       ok              = true;

    ok &= check(prepareDepthCopySubmission(&device, &swapchain, 0, depth, backup) == VK_SUCCESS && backupCopies == 1,
                "A compatible backup must be recorded for the selected depth clear");
    auto&      first      = swapchain.depthCopySubmissions[0];
    const auto firstFence = first.fence;
    first.pending         = true;
    first.referencesDepth = true;
    bool idle             = true;
    ok &= check(depthResourcesIdle(&device, &swapchain, idle) == VK_SUCCESS && !idle,
                "Depth resources must remain in use while their submission is pending");
    backup.reset();
    ok &= check(prepareDepthCopySubmission(&device, &swapchain, 0, depth, {}) == VK_SUCCESS && fallbackCopies == 1 && nonblocking
                    && swapchain.retiredDepthCopySubmissions.size() == 1 && !backupReference.expired() && freedCommands == 0,
                "An unfinished copy must remain alive without blocking the next present");

    auto&      second      = swapchain.depthCopySubmissions[0];
    const auto secondFence = second.fence;
    second.pending         = true;
    second.referencesDepth = true;
    waitError              = VK_ERROR_DEVICE_LOST;
    ok &= check(prepareDepthCopySubmission(&device, &swapchain, 0, depth, {}) == VK_ERROR_DEVICE_LOST && !backupReference.expired()
                    && freedCommands == 0,
                "Fence errors must preserve resources and propagate to the caller");
    waitError           = VK_SUCCESS;
    fences[firstFence]  = true;
    fences[secondFence] = true;
    ok &= check(depthResourcesIdle(&device, &swapchain, idle) == VK_SUCCESS && idle,
                "Completed submissions must permit depth resources to be replaced without a queue-idle wait");
    ok &= check(prepareDepthCopySubmission(&device, &swapchain, 0, depth, {}) == VK_SUCCESS && backupReference.expired()
                    && swapchain.retiredDepthCopySubmissions.empty() && freedCommands == 1 && resetCommands == 1,
                "Completed copies must release retired resources and reuse the current command buffer");
    const int previousFallbackCopies = fallbackCopies;
    ok &= check(prepareDepthCopySubmission(&device, &swapchain, 0, depth, {}, false) == VK_SUCCESS && fallbackCopies == previousFallbackCopies
                    && resetCommands == 1,
                "A present without depth copying must still receive a fence without recording a copy");
    ok &= check(prepareDepthCopySubmission(&device, &swapchain, 0, depth, {}, true, 1) == VK_SUCCESS && previousCopies == 1
                    && fallbackCopies == previousFallbackCopies,
                "A generated frame must copy the latest depth output without reading a stale game image");
    releaseDepthCopySubmission(&device, swapchain.depthCopySubmissions[0]);
    ok &= check(freedCommands == 2 && fences.empty(), "Shutdown must release all remaining copy resources");
    return ok ? 0 : 1;
}
