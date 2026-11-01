#include "depth_image_capture.hpp"

#include "format.hpp"

namespace vkShade
{
    static bool canAddCaptureUsage(VkPhysicalDevice                             physicalDevice,
                                   const VkImageCreateInfo&                     createInfo,
                                   PFN_vkGetPhysicalDeviceImageFormatProperties getImageFormatProperties)
    {
        if (!isDepthFormat(createInfo.format) || createInfo.samples != VK_SAMPLE_COUNT_1_BIT
            || !(createInfo.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || (createInfo.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
            || (createInfo.flags & (VK_IMAGE_CREATE_PROTECTED_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT))
            || (createInfo.tiling != VK_IMAGE_TILING_OPTIMAL && createInfo.tiling != VK_IMAGE_TILING_LINEAR))
            return false;

        for (auto* node = static_cast<const VkBaseInStructure*>(createInfo.pNext); node; node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO
                && reinterpret_cast<const VkExternalMemoryImageCreateInfo*>(node)->handleTypes != 0)
                return false;
#ifdef VK_NV_external_memory
            if (node->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_NV
                && reinterpret_cast<const VkExternalMemoryImageCreateInfoNV*>(node)->handleTypes != 0)
                return false;
#endif
#ifdef VK_KHR_maintenance11
            if (node->sType == VK_STRUCTURE_TYPE_IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR
                || node->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_FLAGS_2_CREATE_INFO_KHR)
                return false;
#endif
        }

        constexpr VkImageUsageFlags captureUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ((createInfo.usage & captureUsage) == captureUsage)
            return true;
        if (createInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
            return false;

        VkImageFormatProperties properties{};
        if (getImageFormatProperties(physicalDevice,
                                     createInfo.format,
                                     createInfo.imageType,
                                     createInfo.tiling,
                                     createInfo.usage | captureUsage,
                                     createInfo.flags,
                                     &properties)
            != VK_SUCCESS)
            return false;

        return createInfo.extent.width <= properties.maxExtent.width && createInfo.extent.height <= properties.maxExtent.height
               && createInfo.extent.depth <= properties.maxExtent.depth && createInfo.mipLevels <= properties.maxMipLevels
               && createInfo.arrayLayers <= properties.maxArrayLayers && (properties.sampleCounts & createInfo.samples) != 0;
    }

    DepthImageCreateResult createDepthCaptureImage(VkPhysicalDevice                             physicalDevice,
                                                   VkDevice                                     device,
                                                   const VkImageCreateInfo&                     createInfo,
                                                   const VkAllocationCallbacks*                 allocator,
                                                   VkImage*                                     image,
                                                   PFN_vkGetPhysicalDeviceImageFormatProperties getImageFormatProperties,
                                                   PFN_vkCreateImage                            createImage)
    {
        if (!canAddCaptureUsage(physicalDevice, createInfo, getImageFormatProperties))
            return {createImage(device, &createInfo, allocator, image), false};

        VkImageCreateInfo augmented = createInfo;
        augmented.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkResult result = createImage(device, &augmented, allocator, image);
        if (result != VK_SUCCESS && augmented.usage != createInfo.usage)
            return {createImage(device, &createInfo, allocator, image), false};
        return {result, result == VK_SUCCESS};
    }
} // namespace vkShade
