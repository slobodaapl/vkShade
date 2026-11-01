#pragma once

#include "vulkan_include.hpp"

namespace vkShade
{
    struct DeviceFeatureSupport
    {
        VkPhysicalDeviceFeatures core{};
        bool                     deviceFault = false;
    };

    DeviceFeatureSupport queryDeviceFeatureSupport(VkPhysicalDevice                 physicalDevice,
                                                   PFN_vkGetPhysicalDeviceFeatures2 getFeatures2,
                                                   PFN_vkGetPhysicalDeviceFeatures  getFeatures,
                                                   bool                             queryDeviceFault);

    class DeviceFeatureConfiguration
    {
    public:
        DeviceFeatureConfiguration(const VkDeviceCreateInfo& createInfo, const DeviceFeatureSupport& support, bool enableDeviceFault);
        DeviceFeatureConfiguration(const DeviceFeatureConfiguration&)            = delete;
        DeviceFeatureConfiguration& operator=(const DeviceFeatureConfiguration&) = delete;

        const VkDeviceCreateInfo& createInfo() const
        {
            return createInfo_;
        }
        const VkPhysicalDeviceFeatures& enabledFeatures() const
        {
            return enabledFeatures_;
        }
        bool deviceFaultEnabled() const
        {
            return deviceFaultEnabled_;
        }

    private:
        VkDeviceCreateInfo               createInfo_{};
        VkPhysicalDeviceFeatures         enabledFeatures_{};
        VkPhysicalDeviceFaultFeaturesEXT faultFeature_{};
        bool                             deviceFaultEnabled_ = false;
    };
} // namespace vkShade
