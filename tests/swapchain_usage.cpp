#include "swapchain_usage.hpp"

#include <iostream>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType                    = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.presentMode              = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.imageUsage               = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    bool ok   = true;
    auto plan = planSwapchainUsage(createInfo);
    ok &= check(!plan.passthrough && !plan.useTransfer && plan.imageUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                "Color-only swapchains need a render copy");

    createInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    plan                  = planSwapchainUsage(createInfo);
    ok &= check(!plan.passthrough && plan.useTransfer && plan.imageUsage == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
                "Transfer destinations must retain their usage and support the overlay");

    createInfo.imageUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
    plan                  = planSwapchainUsage(createInfo);
    ok &= check(!plan.passthrough && !plan.useTransfer && plan.imageUsage == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
                "Adding color attachment usage must preserve other requested usages");

    createInfo.flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
    plan             = planSwapchainUsage(createInfo);
    ok &= check(plan.passthrough && plan.imageUsage == createInfo.imageUsage, "Protected swapchains must pass through unchanged");
    createInfo.flags       = 0;
    createInfo.presentMode = VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR;
    plan                   = planSwapchainUsage(createInfo);
    ok &= check(plan.passthrough && plan.imageUsage == createInfo.imageUsage, "Shared-present swapchains must pass through unchanged");
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;

#ifdef VK_KHR_extended_flags
    VkImageUsageFlags2CreateInfoKHR extended = {};
    extended.sType                           = VK_STRUCTURE_TYPE_IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR;
    extended.usage                           = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageFormatListCreateInfo formatList   = {};
    formatList.sType                         = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    formatList.pNext                         = &extended;
    createInfo.pNext                         = &formatList;
    createInfo.imageUsage                    = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    plan                                     = planSwapchainUsage(createInfo);
    ok &= check(!plan.passthrough && !plan.useTransfer && plan.imageUsage == createInfo.imageUsage,
                "Extended usage must override the legacy field without changing the chain");

    extended.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    plan = planSwapchainUsage(createInfo);
    ok &= check(!plan.passthrough && plan.useTransfer, "Extended transfer usage must select a transfer copy");

    extended.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    plan           = planSwapchainUsage(createInfo);
    ok &= check(plan.passthrough && plan.imageUsage == createInfo.imageUsage, "Extended usage without color attachment must pass through unchanged");
#endif

    return ok ? 0 : 1;
}
