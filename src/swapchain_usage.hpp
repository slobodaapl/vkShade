#pragma once

#include <cstdint>

#include "vulkan_include.hpp"

namespace vkShade
{
    struct SwapchainUsagePlan
    {
        VkImageUsageFlags imageUsage  = 0;
        bool              useTransfer = false;
        bool              passthrough = false;
    };

    inline bool canUseDirectEffectOutput(bool mutableFormat, bool transferDestination)
    {
        return mutableFormat && transferDestination;
    }

    inline SwapchainUsagePlan planSwapchainUsage(const VkSwapchainCreateInfoKHR& createInfo)
    {
        uint64_t requestedUsage   = createInfo.imageUsage;
        bool     hasExtendedUsage = false;
#ifdef VK_KHR_extended_flags
        for (const auto* next = reinterpret_cast<const VkBaseInStructure*>(createInfo.pNext); next; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR)
            {
                requestedUsage   = reinterpret_cast<const VkImageUsageFlags2CreateInfoKHR*>(next)->usage;
                hasExtendedUsage = true;
                break;
            }
        }
#endif
        const bool protectedSwapchain = (createInfo.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) != 0;
        const bool sharedPresent      = createInfo.presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR
                                        || createInfo.presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR;
        const bool passthrough = protectedSwapchain || sharedPresent || (hasExtendedUsage && !(requestedUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        const VkImageUsageFlags imageUsage =
            hasExtendedUsage || passthrough ? createInfo.imageUsage : createInfo.imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        return {imageUsage, (requestedUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0, passthrough};
    }
} // namespace vkShade
