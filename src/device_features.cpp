#include "device_features.hpp"

namespace vkShade
{
    DeviceFeatureSupport queryDeviceFeatureSupport(VkPhysicalDevice                 physicalDevice,
                                                   PFN_vkGetPhysicalDeviceFeatures2 getFeatures2,
                                                   PFN_vkGetPhysicalDeviceFeatures  getFeatures,
                                                   bool                             queryDeviceFault)
    {
        DeviceFeatureSupport support{};
        if (getFeatures2)
        {
            VkPhysicalDeviceFaultFeaturesEXT fault{};
            fault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = queryDeviceFault ? &fault : nullptr;
            getFeatures2(physicalDevice, &features);
            support.core        = features.features;
            support.deviceFault = queryDeviceFault && fault.deviceFault == VK_TRUE;
        }
        else if (getFeatures)
        {
            getFeatures(physicalDevice, &support.core);
        }
        return support;
    }

    DeviceFeatureConfiguration::DeviceFeatureConfiguration(const VkDeviceCreateInfo&   createInfo,
                                                           const DeviceFeatureSupport& support,
                                                           bool                        enableDeviceFault) : createInfo_(createInfo)
    {
        const VkPhysicalDeviceFeatures2*        features2 = nullptr;
        const VkPhysicalDeviceFaultFeaturesEXT* fault     = nullptr;
        for (const auto* next = reinterpret_cast<const VkBaseInStructure*>(createInfo.pNext); next; next = next->pNext)
        {
            if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
                features2 = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(next);
            else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT)
                fault = reinterpret_cast<const VkPhysicalDeviceFaultFeaturesEXT*>(next);
        }

        if (features2)
        {
            enabledFeatures_ = features2->features;
        }
        else
        {
            if (createInfo.pEnabledFeatures)
                enabledFeatures_ = *createInfo.pEnabledFeatures;
            enabledFeatures_.shaderImageGatherExtended |= support.core.shaderImageGatherExtended;
            enabledFeatures_.shaderStorageImageReadWithoutFormat |= support.core.shaderStorageImageReadWithoutFormat;
            enabledFeatures_.shaderStorageImageWriteWithoutFormat |= support.core.shaderStorageImageWriteWithoutFormat;
            createInfo_.pEnabledFeatures = &enabledFeatures_;
        }

        if (fault)
        {
            deviceFaultEnabled_ = fault->deviceFault == VK_TRUE;
        }
        else if (enableDeviceFault && support.deviceFault)
        {
            faultFeature_.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            faultFeature_.deviceFault = VK_TRUE;
            faultFeature_.pNext       = const_cast<void*>(createInfo_.pNext);
            createInfo_.pNext         = &faultFeature_;
            deviceFaultEnabled_       = true;
        }
    }
} // namespace vkShade
