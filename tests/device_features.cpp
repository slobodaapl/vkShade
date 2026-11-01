#include "device_features.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    constexpr VkBool32 VkPhysicalDeviceFeatures::* featureMembers[] = {
        &VkPhysicalDeviceFeatures::robustBufferAccess,
        &VkPhysicalDeviceFeatures::fullDrawIndexUint32,
        &VkPhysicalDeviceFeatures::imageCubeArray,
        &VkPhysicalDeviceFeatures::independentBlend,
        &VkPhysicalDeviceFeatures::geometryShader,
        &VkPhysicalDeviceFeatures::tessellationShader,
        &VkPhysicalDeviceFeatures::sampleRateShading,
        &VkPhysicalDeviceFeatures::dualSrcBlend,
        &VkPhysicalDeviceFeatures::logicOp,
        &VkPhysicalDeviceFeatures::multiDrawIndirect,
        &VkPhysicalDeviceFeatures::drawIndirectFirstInstance,
        &VkPhysicalDeviceFeatures::depthClamp,
        &VkPhysicalDeviceFeatures::depthBiasClamp,
        &VkPhysicalDeviceFeatures::fillModeNonSolid,
        &VkPhysicalDeviceFeatures::depthBounds,
        &VkPhysicalDeviceFeatures::wideLines,
        &VkPhysicalDeviceFeatures::largePoints,
        &VkPhysicalDeviceFeatures::alphaToOne,
        &VkPhysicalDeviceFeatures::multiViewport,
        &VkPhysicalDeviceFeatures::samplerAnisotropy,
        &VkPhysicalDeviceFeatures::textureCompressionETC2,
        &VkPhysicalDeviceFeatures::textureCompressionASTC_LDR,
        &VkPhysicalDeviceFeatures::textureCompressionBC,
        &VkPhysicalDeviceFeatures::occlusionQueryPrecise,
        &VkPhysicalDeviceFeatures::pipelineStatisticsQuery,
        &VkPhysicalDeviceFeatures::vertexPipelineStoresAndAtomics,
        &VkPhysicalDeviceFeatures::fragmentStoresAndAtomics,
        &VkPhysicalDeviceFeatures::shaderTessellationAndGeometryPointSize,
        &VkPhysicalDeviceFeatures::shaderImageGatherExtended,
        &VkPhysicalDeviceFeatures::shaderStorageImageExtendedFormats,
        &VkPhysicalDeviceFeatures::shaderStorageImageMultisample,
        &VkPhysicalDeviceFeatures::shaderStorageImageReadWithoutFormat,
        &VkPhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat,
        &VkPhysicalDeviceFeatures::shaderUniformBufferArrayDynamicIndexing,
        &VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing,
        &VkPhysicalDeviceFeatures::shaderStorageBufferArrayDynamicIndexing,
        &VkPhysicalDeviceFeatures::shaderStorageImageArrayDynamicIndexing,
        &VkPhysicalDeviceFeatures::shaderClipDistance,
        &VkPhysicalDeviceFeatures::shaderCullDistance,
        &VkPhysicalDeviceFeatures::shaderFloat64,
        &VkPhysicalDeviceFeatures::shaderInt64,
        &VkPhysicalDeviceFeatures::shaderInt16,
        &VkPhysicalDeviceFeatures::shaderResourceResidency,
        &VkPhysicalDeviceFeatures::shaderResourceMinLod,
        &VkPhysicalDeviceFeatures::sparseBinding,
        &VkPhysicalDeviceFeatures::sparseResidencyBuffer,
        &VkPhysicalDeviceFeatures::sparseResidencyImage2D,
        &VkPhysicalDeviceFeatures::sparseResidencyImage3D,
        &VkPhysicalDeviceFeatures::sparseResidency2Samples,
        &VkPhysicalDeviceFeatures::sparseResidency4Samples,
        &VkPhysicalDeviceFeatures::sparseResidency8Samples,
        &VkPhysicalDeviceFeatures::sparseResidency16Samples,
        &VkPhysicalDeviceFeatures::sparseResidencyAliased,
        &VkPhysicalDeviceFeatures::variableMultisampleRate,
        &VkPhysicalDeviceFeatures::inheritedQueries,
    };

    bool sameFeatures(const VkPhysicalDeviceFeatures& left, const VkPhysicalDeviceFeatures& right)
    {
        for (const auto member : featureMembers)
        {
            if (left.*member != right.*member)
            {
                return false;
            }
        }
        return true;
    }

    VkPhysicalDeviceFeatures allFeatures(VkBool32 value)
    {
        VkPhysicalDeviceFeatures result{};
        for (const auto member : featureMembers)
        {
            result.*member = value;
        }
        return result;
    }

    struct SentinelChainNode
    {
        VkStructureType sType{};
        void*           pNext  = nullptr;
        uint64_t        marker = 0;
    };

    VkPhysicalDevice         expectedPhysicalDevice = VK_NULL_HANDLE;
    int                      features2Calls         = 0;
    int                      legacyCalls            = 0;
    bool                     expectFaultNode        = false;
    VkBool32                 queriedDeviceFault     = VK_FALSE;
    VkPhysicalDeviceFeatures queriedFeatures{};
    VkPhysicalDeviceFeatures legacyFeatures{};

    VKAPI_ATTR void VKAPI_CALL fakeGetFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* features)
    {
        ++features2Calls;
        require(physicalDevice == expectedPhysicalDevice, "Features2 query used a different physical device");
        require(features != nullptr, "Features2 query passed a null root");
        require(features->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "Features2 query used the wrong root type");

        if (expectFaultNode)
        {
            auto* fault = static_cast<VkPhysicalDeviceFaultFeaturesEXT*>(features->pNext);
            require(fault != nullptr, "fault query omitted its output node");
            require(fault->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, "fault query used the wrong node type");
            require(fault->pNext == nullptr, "fault query added an unexpected chain node");
            fault->deviceFault = queriedDeviceFault;
        }
        else
        {
            require(features->pNext == nullptr, "fault output node was chained when not requested");
        }

        features->features = queriedFeatures;
    }

    VKAPI_ATTR void VKAPI_CALL fakeGetFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* features)
    {
        ++legacyCalls;
        require(physicalDevice == expectedPhysicalDevice, "legacy query used a different physical device");
        require(features != nullptr, "legacy query passed a null output");
        *features = legacyFeatures;
    }

    void testQueries()
    {
        expectedPhysicalDevice         = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(0x1234));
        queriedFeatures                = allFeatures(VK_TRUE);
        queriedFeatures.geometryShader = VK_FALSE;
        queriedDeviceFault             = VK_TRUE;
        expectFaultNode                = true;
        features2Calls                 = 0;
        legacyCalls                    = 0;

        const auto viaFeatures2 = vkShade::queryDeviceFeatureSupport(expectedPhysicalDevice, fakeGetFeatures2, fakeGetFeatures, true);
        require(features2Calls == 1 && legacyCalls == 0, "Features2 query did not take exactly one preferred path");
        require(sameFeatures(viaFeatures2.core, queriedFeatures), "Features2 core output was not returned intact");
        require(viaFeatures2.deviceFault, "requested device-fault support was not returned");

        expectFaultNode                    = false;
        queriedFeatures                    = {};
        queriedFeatures.robustBufferAccess = VK_TRUE;
        queriedDeviceFault                 = VK_TRUE;
        features2Calls                     = 0;
        legacyCalls                        = 0;
        const auto withoutFault            = vkShade::queryDeviceFeatureSupport(expectedPhysicalDevice, fakeGetFeatures2, fakeGetFeatures, false);
        require(features2Calls == 1 && legacyCalls == 0, "Features2 query without fault request called the wrong path count");
        require(withoutFault.core.robustBufferAccess == VK_TRUE, "Features2 query lost core support");
        require(!withoutFault.deviceFault, "fault support was reported without querying it");

        legacyFeatures               = allFeatures(VK_FALSE);
        legacyFeatures.shaderFloat64 = VK_TRUE;
        features2Calls               = 0;
        legacyCalls                  = 0;
        const auto viaLegacy         = vkShade::queryDeviceFeatureSupport(expectedPhysicalDevice, nullptr, fakeGetFeatures, true);
        require(features2Calls == 0 && legacyCalls == 1, "legacy fallback did not call exactly once");
        require(sameFeatures(viaLegacy.core, legacyFeatures), "legacy core output was not returned intact");
        require(!viaLegacy.deviceFault, "legacy fallback claimed device-fault query support");

        features2Calls         = 0;
        legacyCalls            = 0;
        const auto unavailable = vkShade::queryDeviceFeatureSupport(expectedPhysicalDevice, nullptr, nullptr, true);
        require(features2Calls == 0 && legacyCalls == 0, "missing query functions were called");
        require(sameFeatures(unavailable.core, VkPhysicalDeviceFeatures{}), "missing query functions returned nonzero support");
        require(!unavailable.deviceFault, "missing query functions returned fault support");
    }

    void testLegacyConfigurationCopiesAndMerges()
    {
        auto              applicationFeatures       = allFeatures(VK_TRUE);
        const auto        applicationFeaturesBefore = applicationFeatures;
        SentinelChainNode originalChain{static_cast<VkStructureType>(0x7f000001), nullptr, 0xfeedbeef};

        VkDeviceQueueCreateInfo queue{};
        queue.sType              = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex   = 7;
        queue.queueCount         = 1;
        float priority           = 0.75f;
        queue.pQueuePriorities   = &priority;
        const char* layers[]     = {"VK_LAYER_TEST"};
        const char* extensions[] = {"VK_EXT_test_extension"};

        VkDeviceCreateInfo applicationInfo{};
        applicationInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        applicationInfo.pNext                   = &originalChain;
        applicationInfo.flags                   = 0x10;
        applicationInfo.queueCreateInfoCount    = 1;
        applicationInfo.pQueueCreateInfos       = &queue;
        applicationInfo.enabledLayerCount       = 1;
        applicationInfo.ppEnabledLayerNames     = layers;
        applicationInfo.enabledExtensionCount   = 1;
        applicationInfo.ppEnabledExtensionNames = extensions;
        applicationInfo.pEnabledFeatures        = &applicationFeatures;
        const auto applicationInfoBefore        = applicationInfo;

        vkShade::DeviceFeatureSupport       unsupported{};
        vkShade::DeviceFeatureConfiguration preserveAppRequests(applicationInfo, unsupported, false);
        const auto&                         preservedInfo = preserveAppRequests.createInfo();
        require(sameFeatures(preserveAppRequests.enabledFeatures(), applicationFeaturesBefore),
                "unsupported application-requested feature bits were cleared");
        require(preservedInfo.pEnabledFeatures == &preserveAppRequests.enabledFeatures(),
                "legacy feature pointer does not refer to owned configuration storage");
        require(preservedInfo.sType == applicationInfoBefore.sType && preservedInfo.pNext == applicationInfoBefore.pNext
                    && preservedInfo.flags == applicationInfoBefore.flags,
                "legacy configuration changed create-info header fields");
        require(preservedInfo.queueCreateInfoCount == applicationInfoBefore.queueCreateInfoCount
                    && preservedInfo.pQueueCreateInfos == applicationInfoBefore.pQueueCreateInfos,
                "legacy configuration changed queue requests");
        require(preservedInfo.enabledLayerCount == applicationInfoBefore.enabledLayerCount
                    && preservedInfo.ppEnabledLayerNames == applicationInfoBefore.ppEnabledLayerNames,
                "legacy configuration changed layer requests");
        require(preservedInfo.enabledExtensionCount == applicationInfoBefore.enabledExtensionCount
                    && preservedInfo.ppEnabledExtensionNames == applicationInfoBefore.ppEnabledExtensionNames,
                "legacy configuration changed extension requests");
        require(applicationInfo.pNext == &originalChain && applicationInfo.pEnabledFeatures == &applicationFeatures,
                "legacy configuration mutated the application create info");
        require(sameFeatures(applicationFeatures, applicationFeaturesBefore), "legacy configuration mutated application feature storage");
        require(originalChain.marker == 0xfeedbeef && originalChain.pNext == nullptr, "legacy configuration mutated an unknown caller chain node");

        auto mixedApplicationFeatures                                 = VkPhysicalDeviceFeatures{};
        mixedApplicationFeatures.robustBufferAccess                   = VK_TRUE;
        mixedApplicationFeatures.geometryShader                       = VK_FALSE;
        mixedApplicationFeatures.samplerAnisotropy                    = VK_TRUE;
        mixedApplicationFeatures.shaderImageGatherExtended            = VK_FALSE;
        mixedApplicationFeatures.shaderStorageImageReadWithoutFormat  = VK_FALSE;
        mixedApplicationFeatures.shaderStorageImageWriteWithoutFormat = VK_FALSE;
        const auto         mixedApplicationFeaturesBefore             = mixedApplicationFeatures;
        VkDeviceCreateInfo mixedApplicationInfo{};
        mixedApplicationInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        mixedApplicationInfo.pNext            = &originalChain;
        mixedApplicationInfo.pEnabledFeatures = &mixedApplicationFeatures;
        vkShade::DeviceFeatureSupport mixedSupport{};
        mixedSupport.core                                     = allFeatures(VK_TRUE);
        mixedSupport.core.shaderStorageImageReadWithoutFormat = VK_FALSE;
        vkShade::DeviceFeatureConfiguration mergeSupportedRequests(mixedApplicationInfo, mixedSupport, false);
        auto                                expectedMixedFeatures  = mixedApplicationFeaturesBefore;
        expectedMixedFeatures.shaderImageGatherExtended            = VK_TRUE;
        expectedMixedFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        require(sameFeatures(mergeSupportedRequests.enabledFeatures(), expectedMixedFeatures),
                "supported optional bits were not ORed into mixed application features, or unrelated app bits changed");
        require(mixedApplicationInfo.pEnabledFeatures == &mixedApplicationFeatures
                    && sameFeatures(mixedApplicationFeatures, mixedApplicationFeaturesBefore),
                "feature merge mutated caller-owned legacy feature data");
        require(mergeSupportedRequests.createInfo().pNext == &originalChain, "feature merge changed the caller chain");

        vkShade::DeviceFeatureSupport laptopSupport{};
        laptopSupport.core = allFeatures(VK_TRUE);
        VkDeviceCreateInfo noLegacyFeatures{};
        noLegacyFeatures.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        noLegacyFeatures.pNext = &originalChain;
        vkShade::DeviceFeatureConfiguration addOnlyLayerFeatures(noLegacyFeatures, laptopSupport, false);

        auto expected                                 = VkPhysicalDeviceFeatures{};
        expected.shaderImageGatherExtended            = VK_TRUE;
        expected.shaderStorageImageReadWithoutFormat  = VK_TRUE;
        expected.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        require(sameFeatures(addOnlyLayerFeatures.enabledFeatures(), expected),
                "physical support enabled features outside the three optional layer requirements");
        require(addOnlyLayerFeatures.createInfo().pEnabledFeatures == &addOnlyLayerFeatures.enabledFeatures(),
                "null legacy input did not produce stable owned feature storage");
        require(addOnlyLayerFeatures.createInfo().pNext == &originalChain, "legacy configuration replaced an unknown caller chain");
    }

    void testFeatures2AndExistingFaultNode()
    {
        SentinelChainNode         unknownBefore{static_cast<VkStructureType>(0x7f000002), nullptr, 0x11112222};
        SentinelChainNode         unknownAfter{static_cast<VkStructureType>(0x7f000003), nullptr, 0x33334444};
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType                                  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.features                               = {};
        features2.features.robustBufferAccess            = VK_TRUE;
        features2.features.shaderImageGatherExtended     = VK_FALSE;
        features2.features.geometryShader                = VK_FALSE;
        const auto                       features2Before = features2.features;
        VkPhysicalDeviceFaultFeaturesEXT fault{};
        fault.sType                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        fault.deviceFault             = VK_FALSE;
        fault.deviceFaultVendorBinary = VK_TRUE;
        fault.pNext                   = &unknownAfter;
        features2.pNext               = &fault;
        unknownBefore.pNext           = &features2;

        auto               conflictingLegacyFeatures = allFeatures(VK_TRUE);
        VkDeviceCreateInfo applicationInfo{};
        applicationInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        applicationInfo.pNext            = &unknownBefore;
        applicationInfo.pEnabledFeatures = &conflictingLegacyFeatures;
        const auto applicationInfoBefore = applicationInfo;

        vkShade::DeviceFeatureSupport support{};
        support.core        = allFeatures(VK_TRUE);
        support.deviceFault = VK_TRUE;
        vkShade::DeviceFeatureConfiguration existingFaultDisabled(applicationInfo, support, true);
        require(sameFeatures(existingFaultDisabled.enabledFeatures(), features2.features),
                "Features2 in the caller chain did not remain authoritative");
        require(existingFaultDisabled.createInfo().pEnabledFeatures == &conflictingLegacyFeatures,
                "existing Features2 input caused legacy pointer rewriting");
        require(existingFaultDisabled.createInfo().pNext == &unknownBefore, "existing Features2 chain head was replaced");
        require(unknownBefore.pNext == &features2 && features2.pNext == &fault && fault.pNext == &unknownAfter,
                "existing feature chain pointers were changed");
        require(features2.features.shaderImageGatherExtended == VK_FALSE && features2.features.geometryShader == VK_FALSE,
                "Features2 disabled bits were forced on from physical support");
        require(fault.deviceFault == VK_FALSE && fault.deviceFaultVendorBinary == VK_TRUE && !existingFaultDisabled.deviceFaultEnabled(),
                "existing fault request was overridden instead of reported exactly");
        require(sameFeatures(features2.features, features2Before), "Features2 input feature bits were mutated");
        require(unknownBefore.marker == 0x11112222 && unknownAfter.marker == 0x33334444, "unknown chain payload was changed");
        require(applicationInfo.pNext == applicationInfoBefore.pNext && applicationInfo.pEnabledFeatures == applicationInfoBefore.pEnabledFeatures,
                "Features2 configuration mutated caller create-info fields");

        VkPhysicalDeviceFeatures2 validFeatures2{};
        validFeatures2.sType                              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        validFeatures2.features                           = {};
        validFeatures2.features.geometryShader            = VK_TRUE;
        validFeatures2.features.shaderImageGatherExtended = VK_FALSE;
        const auto         validFeatures2Before           = validFeatures2.features;
        VkDeviceCreateInfo validFeatures2Info{};
        validFeatures2Info.sType            = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        validFeatures2Info.pNext            = &validFeatures2;
        validFeatures2Info.pEnabledFeatures = nullptr;
        vkShade::DeviceFeatureSupport validFeatures2Support{};
        validFeatures2Support.core = allFeatures(VK_TRUE);
        vkShade::DeviceFeatureConfiguration validFeatures2Configuration(validFeatures2Info, validFeatures2Support, false);
        require(validFeatures2Configuration.createInfo().pEnabledFeatures == nullptr, "valid Features2 create info gained a legacy feature pointer");
        require(validFeatures2Configuration.createInfo().pNext == &validFeatures2 && validFeatures2.pNext == nullptr
                    && validFeatures2.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                "valid Features2 chain was changed or extended");
        require(sameFeatures(validFeatures2Configuration.enabledFeatures(), validFeatures2Before)
                    && sameFeatures(validFeatures2.features, validFeatures2Before),
                "valid Features2 bits were forced or mutated");

        fault.deviceFault             = VK_TRUE;
        fault.deviceFaultVendorBinary = VK_FALSE;
        support.deviceFault           = VK_FALSE;
        vkShade::DeviceFeatureConfiguration existingFaultEnabled(applicationInfo, support, false);
        require(existingFaultEnabled.deviceFaultEnabled(), "true existing fault bit was not reported when support/request flags disagreed");
        require(existingFaultEnabled.createInfo().pNext == &unknownBefore && features2.pNext == &fault && fault.pNext == &unknownAfter,
                "existing fault node or feature chain was duplicated or replaced");
        require(fault.deviceFault == VK_TRUE && fault.deviceFaultVendorBinary == VK_FALSE, "existing fault fields were rewritten");
    }

    void consumeDeviceCreateInfo(const VkDeviceCreateInfo&       info,
                                 const VkPhysicalDeviceFeatures& expectedFeatures,
                                 const void*                     expectedOriginalChain,
                                 bool                            expectFault)
    {
        require(info.pEnabledFeatures != nullptr, "fake downstream call received no owned legacy features");
        require(sameFeatures(*info.pEnabledFeatures, expectedFeatures), "fake downstream call could not read owned feature data");
        const auto* fault = static_cast<const VkPhysicalDeviceFaultFeaturesEXT*>(info.pNext);
        if (expectFault)
        {
            require(fault != nullptr, "fake downstream call could not read owned fault data");
            require(fault->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT && fault->deviceFault == VK_TRUE,
                    "fake downstream call saw invalid owned fault data");
            require(fault->pNext != nullptr, "owned fault node lost the original chain");
        }
        else
        {
            require(info.pNext == expectedOriginalChain, "fake downstream call saw an unrequested fault node or changed caller chain");
        }
    }

    void testOwnedFaultNodeAndLifetime()
    {
        SentinelChainNode  originalChain{static_cast<VkStructureType>(0x7f000004), nullptr, 0xaabbccdd};
        VkDeviceCreateInfo applicationInfo{};
        applicationInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        applicationInfo.pNext            = &originalChain;
        const auto applicationInfoBefore = applicationInfo;

        vkShade::DeviceFeatureSupport support{};
        support.deviceFault                    = VK_TRUE;
        support.core.shaderImageGatherExtended = VK_TRUE;
        auto expected                          = VkPhysicalDeviceFeatures{};
        expected.shaderImageGatherExtended     = VK_TRUE;
        {
            vkShade::DeviceFeatureConfiguration configuration(applicationInfo, support, true);
            require(configuration.deviceFaultEnabled(), "supported and requested fault feature was not enabled");
            require(configuration.createInfo().pNext != applicationInfo.pNext, "owned fault node was not added when supported and requested");
            const auto* fault = static_cast<const VkPhysicalDeviceFaultFeaturesEXT*>(configuration.createInfo().pNext);
            require(fault->pNext == &originalChain, "owned fault node did not preserve the caller chain");
            consumeDeviceCreateInfo(configuration.createInfo(), expected, &originalChain, true);
        }

        require(applicationInfo.pNext == applicationInfoBefore.pNext && applicationInfo.pEnabledFeatures == applicationInfoBefore.pEnabledFeatures,
                "owned-node setup mutated caller create info");
        require(originalChain.marker == 0xaabbccdd && originalChain.pNext == nullptr, "owned-node setup mutated caller chain storage");

        support.deviceFault = VK_FALSE;
        vkShade::DeviceFeatureConfiguration unsupportedFault(applicationInfo, support, true);
        require(!unsupportedFault.deviceFaultEnabled(), "unsupported fault feature was enabled");
        require(unsupportedFault.createInfo().pNext == &originalChain, "unsupported fault feature added an owned node");
        consumeDeviceCreateInfo(unsupportedFault.createInfo(), expected, &originalChain, false);

        support.deviceFault = VK_TRUE;
        vkShade::DeviceFeatureConfiguration disabledFault(applicationInfo, support, false);
        require(!disabledFault.deviceFaultEnabled(), "unrequested fault feature was enabled");
        require(disabledFault.createInfo().pNext == &originalChain, "unrequested fault feature added an owned node");
        consumeDeviceCreateInfo(disabledFault.createInfo(), expected, &originalChain, false);
    }
} // namespace

int main()
{
    try
    {
        testQueries();
        testLegacyConfigurationCopiesAndMerges();
        testFeatures2AndExistingFaultNode();
        testOwnedFaultNodeAndLifetime();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
