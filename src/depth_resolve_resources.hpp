#pragma once

#include "vulkan_include.hpp"

namespace vkShade
{
    struct LogicalDevice;
    struct LogicalSwapchain;
    void invalidateDepthResolveOutput(LogicalSwapchain* swapchain);
    void
    recordPreviousDepthResolve(LogicalDevice* device, LogicalSwapchain* swapchain, VkCommandBuffer commandBuffer, uint32_t source, uint32_t target);
    void destroyDepthResolveResources(LogicalSwapchain* swapchain);
} // namespace vkShade
