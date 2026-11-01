#include "depth_resolve_resources.hpp"
#include "logical_swapchain.hpp"
#include "overlay/imgui_overlay.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <vector>

using namespace vkShade;

namespace vkShade
{
    ImGuiOverlay::~ImGuiOverlay() = default;
}

template<typename T>
static T handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}

static std::vector<std::array<VkImageMemoryBarrier, 2>> barriers;
static VkImage                                          sourceImage;
static VkImage                                          targetImage;
static VkImageLayout                                    sourceLayout;
static VkImageLayout                                    targetLayout;
static VkImageCopy                                      imageCopy;
static bool                                             validCalls = true;

static void VKAPI_CALL pipelineBarrier(VkCommandBuffer,
                                       VkPipelineStageFlags,
                                       VkPipelineStageFlags,
                                       VkDependencyFlags,
                                       uint32_t,
                                       const VkMemoryBarrier*,
                                       uint32_t,
                                       const VkBufferMemoryBarrier*,
                                       uint32_t                    count,
                                       const VkImageMemoryBarrier* images)
{
    validCalls &= count == 2;
    if (count == 2)
        barriers.push_back({images[0], images[1]});
}

static void VKAPI_CALL copyImage(VkCommandBuffer,
                                 VkImage            source,
                                 VkImageLayout      sourceImageLayout,
                                 VkImage            target,
                                 VkImageLayout      targetImageLayout,
                                 uint32_t           count,
                                 const VkImageCopy* copies)
{
    validCalls &= count == 1;
    if (count == 1)
    {
        sourceImage  = source;
        targetImage  = target;
        sourceLayout = sourceImageLayout;
        targetLayout = targetImageLayout;
        imageCopy    = copies[0];
    }
}

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    LogicalDevice device{};
    device.vkd.CmdPipelineBarrier = pipelineBarrier;
    device.vkd.CmdCopyImage       = copyImage;
    LogicalSwapchain swapchain{};
    swapchain.depthResolveImages = {handle<VkImage>(1), handle<VkImage>(2)};
    swapchain.depthResolveExtent = {128, 64, 1};
    bool ok                      = true;
    for (const auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_R32_SFLOAT})
    {
        swapchain.depthResolveFormat     = format;
        swapchain.depthResolveUsesShader = format == VK_FORMAT_R32_SFLOAT;
        barriers.clear();
        recordPreviousDepthResolve(&device, &swapchain, handle<VkCommandBuffer>(1), 0, 1);
        const VkImageAspectFlags imageAspect = format == VK_FORMAT_R32_SFLOAT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        const VkImageAspectFlags barrierAspect =
            format == VK_FORMAT_D24_UNORM_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : imageAspect;
        ok &= check(validCalls && barriers.size() == 2 && sourceImage == swapchain.depthResolveImages[0]
                        && targetImage == swapchain.depthResolveImages[1] && sourceLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                        && targetLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && imageCopy.srcSubresource.aspectMask == imageAspect
                        && imageCopy.dstSubresource.aspectMask == imageAspect && imageCopy.extent.width == 128 && imageCopy.extent.height == 64,
                    "Repeated presents must copy the previous depth output with the correct image aspect and extent");
        if (barriers.size() == 2)
            ok &= check(barriers[0][0].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            && barriers[0][0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                            && barriers[0][1].newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                            && barriers[0][0].subresourceRange.aspectMask == barrierAspect
                            && barriers[1][0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            && barriers[1][1].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        "Both images must return to the shader-readable layout after the copy");
    }
    swapchain.depthResolveVersions = {4, 3};
    swapchain.depthResolveLatest   = 0;
    swapchain.depthResolveVersion  = 4;
    invalidateDepthResolveOutput(&swapchain);
    ok &=
        check(swapchain.depthResolveVersions == std::vector<uint64_t>({0, 0}) && !swapchain.depthResolveLatest && swapchain.depthResolveVersion == 0,
              "Changing the selected source must invalidate every previous depth output");
    return ok ? 0 : 1;
}
