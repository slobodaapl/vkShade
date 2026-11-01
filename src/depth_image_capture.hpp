#pragma once

#include <vulkan/vulkan.h>

namespace vkShade
{
    struct DepthImageCreateResult
    {
        VkResult result   = VK_SUCCESS;
        bool     captured = false;
    };

    DepthImageCreateResult createDepthCaptureImage(VkPhysicalDevice                             physicalDevice,
                                                   VkDevice                                     device,
                                                   const VkImageCreateInfo&                     createInfo,
                                                   const VkAllocationCallbacks*                 allocator,
                                                   VkImage*                                     image,
                                                   PFN_vkGetPhysicalDeviceImageFormatProperties getImageFormatProperties,
                                                   PFN_vkCreateImage                            createImage);
} // namespace vkShade
