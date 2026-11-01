#include "overlay/imgui_overlay.hpp"
#include "descriptor_set.hpp"
#include "image_view.hpp"
#include "shader.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <type_traits>
#include <vector>

namespace vkShade
{
    // The test device never owns an overlay.
    ImGuiOverlay::~ImGuiOverlay() = default;
} // namespace vkShade

namespace
{
    unsigned int          calls   = 0;
    bool                  aligned = false;
    std::vector<uint32_t> captured;
    unsigned int          allocations       = 0;
    bool                  hasViewUsage      = false;
    VkImageUsageFlags     capturedViewUsage = 0;

    struct DescriptorWrite
    {
        VkDescriptorSet       set;
        uint32_t              binding = 0;
        VkDescriptorType      type;
        VkDescriptorImageInfo image;
    };
    std::vector<std::vector<DescriptorWrite>> updates;

    template<typename T>
    T handle(uintptr_t value)
    {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(value);
        else
            return static_cast<T>(value);
    }

    VKAPI_ATTR VkResult VKAPI_CALL captureShader(VkDevice, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks*, VkShaderModule* module)
    {
        ++calls;
        aligned = reinterpret_cast<uintptr_t>(info->pCode) % alignof(uint32_t) == 0;
        captured.resize(info->codeSize / sizeof(uint32_t));
        std::memcpy(captured.data(), info->pCode, info->codeSize);
        *module = VK_NULL_HANDLE;
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL allocateSets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets)
    {
        ++allocations;
        for (uint32_t i = 0; i < info->descriptorSetCount; ++i)
            sets[i] = handle<VkDescriptorSet>(i + 1);
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL captureImageView(VkDevice, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*, VkImageView* view)
    {
        const auto* usage = static_cast<const VkImageViewUsageCreateInfo*>(info->pNext);
        hasViewUsage      = usage && usage->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
        capturedViewUsage = hasViewUsage ? usage->usage : 0;
        *view             = handle<VkImageView>(31);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL captureWrites(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes, uint32_t, const VkCopyDescriptorSet*)
    {
        updates.emplace_back();
        for (uint32_t i = 0; i < count; ++i)
            updates.back().push_back({writes[i].dstSet, writes[i].dstBinding, writes[i].descriptorType, *writes[i].pImageInfo});
    }
} // namespace

int main()
{
    vkShade::LogicalDevice device{};
    device.vkd.CreateShaderModule = captureShader;
    VkShaderModule module         = VK_NULL_HANDLE;

    const std::vector<uint32_t> expected = {0x07230203, 0x00010000, 0x01020304};
    std::vector<char>           bytes(expected.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), expected.data(), bytes.size());
    vkShade::createShaderModule(&device, bytes, &module);
    if (calls != 1 || !aligned || captured != expected)
    {
        std::cerr << "SPIR-V bytes were not passed to Vulkan as aligned words\n";
        return 1;
    }

    bytes.pop_back();
    module = handle<VkShaderModule>(99);
    vkShade::createShaderModule(&device, bytes, &module);
    if (calls != 1 || module != VK_NULL_HANDLE)
    {
        std::cerr << "Misaligned SPIR-V byte count reached Vulkan\n";
        return 1;
    }
    module = handle<VkShaderModule>(99);
    vkShade::createShaderModule(&device, std::vector<char>{}, &module);
    if (calls != 1 || module != VK_NULL_HANDLE)
    {
        std::cerr << "Invalid SPIR-V byte counts reached Vulkan\n";
        return 1;
    }

    device.vkd.AllocateDescriptorSets              = allocateSets;
    device.vkd.UpdateDescriptorSets                = captureWrites;
    const std::vector<VkSampler>          samplers = {handle<VkSampler>(11), handle<VkSampler>(12)};
    const std::vector<VkDescriptorType>   types    = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    const std::vector<VkImageLayout>      layouts  = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL};
    std::vector<std::vector<VkImageView>> views    = {{handle<VkImageView>(21), handle<VkImageView>(22)}, {handle<VkImageView>(23)}};
    auto sets = vkShade::allocateAndWriteImageSamplerDescriptorSets(&device, VK_NULL_HANDLE, VK_NULL_HANDLE, samplers, views, types, layouts);
    if (!sets.empty() || allocations != 0)
    {
        std::cerr << "Mismatched image counts must not allocate descriptor sets\n";
        return 1;
    }

    views[1].push_back(handle<VkImageView>(24));
    sets = vkShade::allocateAndWriteImageSamplerDescriptorSets(&device, VK_NULL_HANDLE, VK_NULL_HANDLE, samplers, views, types, layouts);
    if (allocations != 1 || sets.size() != 2 || updates.size() != 2)
    {
        std::cerr << "Expected one descriptor set and update per image\n";
        return 1;
    }
    for (size_t i = 0; i < updates.size(); ++i)
    {
        if (updates[i].size() != 2 || updates[i][0].set != sets[i] || updates[i][1].set != sets[i] || updates[i][0].binding != 0
            || updates[i][1].binding != 1 || updates[i][0].type != types[0] || updates[i][1].type != types[1]
            || updates[i][0].image.imageView != views[0][i] || updates[i][1].image.imageView != views[1][i]
            || updates[i][0].image.sampler != samplers[0] || updates[i][1].image.sampler != VK_NULL_HANDLE
            || updates[i][0].image.imageLayout != layouts[0] || updates[i][1].image.imageLayout != layouts[1])
        {
            std::cerr << "Descriptor writes did not preserve the image, binding, sampler, or layout\n";
            return 1;
        }
    }
    device.vkd.CreateImageView        = captureImageView;
    const std::vector<VkImage> images = {handle<VkImage>(41)};
    vkShade::createImageViews(&device, VK_FORMAT_R8G8B8A8_UNORM, images);
    if (hasViewUsage)
    {
        std::cerr << "Default image views must retain the image's usage\n";
        return 1;
    }
    vkShade::createImageViews(&device,
                              VK_FORMAT_R8G8B8A8_SRGB,
                              images,
                              VK_IMAGE_VIEW_TYPE_2D,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              1,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!hasViewUsage || capturedViewUsage != (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    {
        std::cerr << "Image views must pass their requested usage to Vulkan\n";
        return 1;
    }
    return 0;
}
