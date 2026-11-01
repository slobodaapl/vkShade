#pragma once

#include "logical_device.hpp"

namespace vkShade
{
    bool                         canCopyDepthBeforeClear(const DepthState& depth);
    std::shared_ptr<DepthBackup> createDepthBackup(LogicalDevice* device, const DepthState& depth);
    void recordDepthBackup(LogicalDevice* device, VkCommandBuffer commandBuffer, const DepthState& depth, const DepthBackup& backup);
    void recordDepthBackupToResolve(
        LogicalDevice* device, LogicalSwapchain* swapchain, VkCommandBuffer commandBuffer, uint32_t imageIndex, const DepthBackup& backup);
} // namespace vkShade
