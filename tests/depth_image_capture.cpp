#include "depth_image_capture.hpp"

#include <iostream>
#include <vector>

using namespace vkShade;

static VkResult                       queryResult = VK_SUCCESS;
static VkImageFormatProperties        properties{};
static int                            queryCount = 0;
static std::vector<VkResult>          createResults;
static std::vector<VkImageUsageFlags> usages;
static const void*                    seenNext = nullptr;

static VKAPI_ATTR VkResult VKAPI_CALL
query(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties* output)
{
    ++queryCount;
    *output = properties;
    return queryResult;
}

static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice, const VkImageCreateInfo* info, const VkAllocationCallbacks*, VkImage*)
{
    usages.push_back(info->usage);
    seenNext = info->pNext;
    return createResults.at(usages.size() - 1);
}

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    constexpr VkImageUsageFlags extra = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageCreateInfo           info{};
    info.sType                = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType            = VK_IMAGE_TYPE_2D;
    info.format               = VK_FORMAT_D32_SFLOAT;
    info.extent               = {1920, 1080, 1};
    info.mipLevels            = 1;
    info.arrayLayers          = 1;
    info.samples              = VK_SAMPLE_COUNT_1_BIT;
    info.tiling               = VK_IMAGE_TILING_OPTIMAL;
    info.usage                = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    properties.maxExtent      = {4096, 4096, 1};
    properties.maxMipLevels   = 12;
    properties.maxArrayLayers = 256;
    properties.sampleCounts   = VK_SAMPLE_COUNT_1_BIT;

    VkImage image = VK_NULL_HANDLE;
    auto    run   = [&](std::vector<VkResult> results) {
        queryCount = 0;
        usages.clear();
        createResults = std::move(results);
        seenNext      = nullptr;
        return createDepthCaptureImage(VK_NULL_HANDLE, VK_NULL_HANDLE, info, nullptr, &image, query, create);
    };
    bool ok = true;

    auto result = run({VK_SUCCESS});
    ok &= check(result.result == VK_SUCCESS && result.captured && queryCount == 1 && usages.size() == 1 && usages[0] == (info.usage | extra),
                "Supported depth images must receive capture usage");
    ok &= check(info.usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, "Application create info must not change");

    queryResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
    result      = run({VK_SUCCESS});
    ok &= check(result.result == VK_SUCCESS && !result.captured && usages.size() == 1 && usages[0] == info.usage,
                "Unsupported augmentation must preserve the application image");
    queryResult = VK_SUCCESS;

    properties.maxExtent.width = 1024;
    result                     = run({VK_SUCCESS});
    ok &= check(!result.captured && usages.size() == 1 && usages[0] == info.usage, "Augmented limits must cover the requested extent");
    properties.maxExtent.width = 4096;

    info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    result = run({VK_SUCCESS});
    ok &= check(!result.captured && queryCount == 0 && usages.size() == 1 && usages[0] == info.usage,
                "Transient depth images must retain their original usage");
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    result     = run({VK_SUCCESS});
    ok &= check(!result.captured && queryCount == 0 && usages.size() == 1 && usages[0] == info.usage,
                "Mutable-format images must not acquire new view-usage requirements");
    info.flags = 0;

    VkExternalMemoryImageCreateInfo external{};
    external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    info.pNext           = &external;
    result               = run({VK_SUCCESS});
    ok &= check(!result.captured && queryCount == 0 && seenNext == &external && usages[0] == info.usage,
                "External-memory images must retain their original create info");
    info.pNext = nullptr;

    result = run({VK_ERROR_FORMAT_NOT_SUPPORTED, VK_SUCCESS});
    ok &= check(result.result == VK_SUCCESS && !result.captured && usages.size() == 2 && usages[0] == (info.usage | extra) && usages[1] == info.usage,
                "A rejected augmentation must retry the original image");
    result = run({VK_ERROR_FORMAT_NOT_SUPPORTED, VK_ERROR_OUT_OF_DEVICE_MEMORY});
    ok &= check(result.result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !result.captured,
                "When fallback fails, the application's original create result must be returned");

    info.usage |= extra;
    result = run({VK_ERROR_OUT_OF_DEVICE_MEMORY});
    ok &= check(result.result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !result.captured && queryCount == 0 && usages.size() == 1,
                "An unchanged image request must not be retried");

    return ok ? 0 : 1;
}
