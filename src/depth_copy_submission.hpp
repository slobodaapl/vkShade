#pragma once

#include "logical_swapchain.hpp"

namespace vkShade
{
    VkResult prepareDepthCopySubmission(LogicalDevice*                      device,
                                        LogicalSwapchain*                   swapchain,
                                        uint32_t                            imageIndex,
                                        const DepthState&                   fallback,
                                        const std::shared_ptr<DepthBackup>& backup,
                                        bool                                recordCopy    = true,
                                        std::optional<uint32_t>             previousImage = {});
    VkResult depthResourcesIdle(LogicalDevice* device, const LogicalSwapchain* swapchain, bool& idle);
    void     releaseDepthCopySubmission(LogicalDevice* device, LogicalSwapchain::DepthCopySubmission& submission);
} // namespace vkShade
