#include "fake_swapchain.hpp"
#include "overlay/imgui_overlay.hpp"

#include <cstdint>
#include <iostream>
#include <type_traits>
#include <vector>

namespace vkShade
{
    ImGuiOverlay::~ImGuiOverlay() = default;
}

namespace
{
    int                       createCalls, destroyed, freed, bound, failCreate, failBind;
    bool                      failAllocate;
    std::vector<VkDeviceSize> offsets;

    template<typename T>
    T handle(uintptr_t value)
    {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(value);
        else
            return static_cast<T>(value);
    }

    VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* image)
    {
        ++createCalls;
        if (createCalls == failCreate)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *image = handle<VkImage>(createCalls);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice, VkImage, const VkAllocationCallbacks*)
    {
        ++destroyed;
    }
    VKAPI_ATTR void VKAPI_CALL requirements(VkDevice, VkImage, VkMemoryRequirements* out)
    {
        *out = {65, 64, 1};
    }
    VKAPI_ATTR void VKAPI_CALL memoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out)
    {
        *out                              = {};
        out->memoryTypeCount              = 1;
        out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* memory)
    {
        if (failAllocate)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *memory = handle<VkDeviceMemory>(44);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*)
    {
        ++freed;
    }
    VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize offset)
    {
        ++bound;
        offsets.push_back(offset);
        return bound == failBind ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
    }
    void reset()
    {
        createCalls = destroyed = freed = bound = failCreate = failBind = 0;
        failAllocate                                                    = false;
        offsets.clear();
    }
} // namespace

int main()
{
    vkShade::LogicalDevice device{};
    device.vkd.CreateImage                       = createImage;
    device.vkd.DestroyImage                      = destroyImage;
    device.vkd.GetImageMemoryRequirements        = requirements;
    device.vki.GetPhysicalDeviceMemoryProperties = memoryProperties;
    device.vkd.AllocateMemory                    = allocate;
    device.vkd.FreeMemory                        = freeMemory;
    device.vkd.BindImageMemory                   = bind;
    VkSwapchainCreateInfoKHR info{};
    info.imageFormat      = VK_FORMAT_R8G8B8A8_UNORM;
    info.imageExtent      = {4, 4};
    info.imageArrayLayers = 1;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    reset();
    auto images = vkShade::createFakeSwapchainImages(&device, info, 2, memory);
    if (images.size() != 2 || memory == VK_NULL_HANDLE || offsets != std::vector<VkDeviceSize>({0, 128}) || destroyed || freed)
        return 1;

    reset();
    failCreate = 2;
    images     = vkShade::createFakeSwapchainImages(&device, info, 2, memory);
    if (!images.empty() || memory != VK_NULL_HANDLE || destroyed != 1 || freed)
        return 2;

    reset();
    failAllocate = true;
    images       = vkShade::createFakeSwapchainImages(&device, info, 2, memory);
    if (!images.empty() || memory != VK_NULL_HANDLE || destroyed != 2 || freed)
        return 3;

    reset();
    failBind = 2;
    images   = vkShade::createFakeSwapchainImages(&device, info, 2, memory);
    if (!images.empty() || memory != VK_NULL_HANDLE || destroyed != 2 || freed != 1)
        return 4;
    return 0;
}
