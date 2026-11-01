#include "overlay/imgui_overlay.hpp"
#include "shader.hpp"

#include <spirv/unified1/spirv.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace vkShade
{
    ImGuiOverlay::~ImGuiOverlay() = default;
}

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct FeatureRequirement
    {
        SpvCapability capability{};
        VkBool32 VkPhysicalDeviceFeatures::* enabledBit  = nullptr;
        const char*                          featureName = nullptr;
    };

    constexpr FeatureRequirement requirements[] = {
        {SpvCapabilityImageGatherExtended, &VkPhysicalDeviceFeatures::shaderImageGatherExtended, "shaderImageGatherExtended"},
        {SpvCapabilityStorageImageReadWithoutFormat,
         &VkPhysicalDeviceFeatures::shaderStorageImageReadWithoutFormat,
         "shaderStorageImageReadWithoutFormat"},
        {SpvCapabilityStorageImageWriteWithoutFormat,
         &VkPhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat,
         "shaderStorageImageWriteWithoutFormat"},
    };

    constexpr uint32_t instruction(uint16_t wordCount, SpvOp opcode)
    {
        return (static_cast<uint32_t>(wordCount) << 16) | static_cast<uint32_t>(opcode);
    }

    std::vector<uint32_t> makeComputeShader(std::initializer_list<SpvCapability> capabilities)
    {
        std::vector<uint32_t> words = {SpvMagicNumber, 0x00010000, 0, 5, 0};
        for (const auto capability : capabilities)
        {
            words.push_back(instruction(2, SpvOpCapability));
            words.push_back(static_cast<uint32_t>(capability));
        }

        words.insert(words.end(),
                     {
                         instruction(3, SpvOpMemoryModel),
                         SpvAddressingModelLogical,
                         SpvMemoryModelGLSL450,
                         instruction(5, SpvOpEntryPoint),
                         SpvExecutionModelGLCompute,
                         1,
                         0x6e69616d, // "main"
                         0,
                         instruction(6, SpvOpExecutionMode),
                         1,
                         SpvExecutionModeLocalSize,
                         1,
                         1,
                         1,
                         instruction(2, SpvOpTypeVoid),
                         2,
                         instruction(3, SpvOpTypeFunction),
                         3,
                         2,
                         instruction(5, SpvOpFunction),
                         2,
                         1,
                         SpvFunctionControlMaskNone,
                         3,
                         instruction(2, SpvOpLabel),
                         4,
                         instruction(1, SpvOpReturn),
                         instruction(1, SpvOpFunctionEnd),
                     });
        return words;
    }

    void testCapabilityMappingsAndOrdering()
    {
        VkPhysicalDeviceFeatures enabled{};
        auto words = makeComputeShader({SpvCapabilityShader, SpvCapabilityStorageImageReadWithoutFormat, SpvCapabilityImageGatherExtended});

        const char* missing = vkShade::missingShaderFeature(words, enabled);
        require(missing != nullptr && std::string(missing) == "shaderStorageImageReadWithoutFormat",
                "first declared disabled capability did not determine the missing feature");

        enabled.shaderStorageImageReadWithoutFormat = VK_TRUE;
        missing                                     = vkShade::missingShaderFeature(words, enabled);
        require(missing != nullptr && std::string(missing) == "shaderImageGatherExtended",
                "later disabled capability was not reported after the first was enabled");

        enabled.shaderImageGatherExtended = VK_TRUE;
        require(vkShade::missingShaderFeature(words, enabled) == nullptr, "enabled declared capabilities were reported missing");

        for (const auto& requirement : requirements)
        {
            const auto               singleCapability = makeComputeShader({SpvCapabilityShader, requirement.capability});
            VkPhysicalDeviceFeatures noFeatures{};
            const char*              actual = vkShade::missingShaderFeature(singleCapability, noFeatures);
            require(actual != nullptr && std::string(actual) == requirement.featureName,
                    "SPIR-V optional capability mapped to the wrong Vulkan feature name");
            noFeatures.*(requirement.enabledBit) = VK_TRUE;
            require(vkShade::missingShaderFeature(singleCapability, noFeatures) == nullptr, "enabled Vulkan feature was still reported missing");
        }

        VkPhysicalDeviceFeatures noOptionalFeatures{};
        const auto               otherCapability = makeComputeShader({SpvCapabilityShader, SpvCapabilityFloat64});
        require(vkShade::missingShaderFeature(otherCapability, noOptionalFeatures) == nullptr,
                "unmapped SPIR-V capability was treated as one of the layer optional features");
    }

    void testMalformedWordStreamsReturn()
    {
        VkPhysicalDeviceFeatures                 enabled{};
        const std::vector<std::vector<uint32_t>> malformed = {
            {},
            {SpvMagicNumber},
            {SpvMagicNumber, 0x00010000},
            {SpvMagicNumber, 0x00010000, 0},
            {SpvMagicNumber, 0x00010000, 0, 5},
            {SpvMagicNumber, 0x00010000, 0, 5, 0, instruction(0, SpvOpCapability), SpvCapabilityImageGatherExtended},
            {SpvMagicNumber, 0x00010000, 0, 5, 0, instruction(2, SpvOpCapability)},
            {SpvMagicNumber, 0x00010000, 0, 5, 0, instruction(0xffff, SpvOpCapability), SpvCapabilityImageGatherExtended},
        };
        for (const auto& words : malformed)
        {
            // Malformed modules have no promised validation result. Reaching the next case
            // establishes that these short/zero/oversized instructions do not hang the scan.
            (void) vkShade::missingShaderFeature(words, enabled);
        }
    }

    int                  createCalls    = 0;
    VkDevice             expectedDevice = VK_NULL_HANDLE;
    std::vector<char>    expectedCodeBytes;
    size_t               expectedCodeSize = 0;
    const VkShaderModule fakeShaderModule = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(0x4321));

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateShaderModule(VkDevice                        device,
                                                          const VkShaderModuleCreateInfo* createInfo,
                                                          const VkAllocationCallbacks*    allocator,
                                                          VkShaderModule*                 shaderModule)
    {
        ++createCalls;
        require(device == expectedDevice, "shader module call used a different device");
        require(allocator == nullptr, "shader module call changed allocator behavior");
        require(createInfo != nullptr && createInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                "shader module call passed an invalid create-info root");
        require(createInfo->codeSize == expectedCodeSize, "shader module call changed code byte size");
        require(createInfo->pCode != nullptr && expectedCodeBytes.size() == expectedCodeSize,
                "shader module call passed null or unexpected shader bytes");
        require(std::memcmp(createInfo->pCode, expectedCodeBytes.data(), expectedCodeSize) == 0, "shader module call changed shader bytes");
        require(shaderModule != nullptr, "shader module call passed a null output");
        *shaderModule = fakeShaderModule;
        return VK_SUCCESS;
    }

    void setExpectedCode(const void* codeBytes, size_t codeSize)
    {
        expectedCodeBytes.resize(codeSize);
        std::memcpy(expectedCodeBytes.data(), codeBytes, codeSize);
        expectedCodeSize = codeSize;
    }

    std::vector<char> asBytes(const std::vector<uint32_t>& words)
    {
        std::vector<char> bytes(words.size() * sizeof(uint32_t));
        std::memcpy(bytes.data(), words.data(), bytes.size());
        return bytes;
    }

    void testCreateShaderModuleChecksEnabledFeatures()
    {
        expectedDevice = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x9876));

        for (const auto& requirement : requirements)
        {
            const auto words = makeComputeShader({SpvCapabilityShader, requirement.capability});
            setExpectedCode(words.data(), words.size() * sizeof(uint32_t));
            createCalls = 0;

            vkShade::LogicalDevice device{};
            device.device                                    = expectedDevice;
            device.vkd.CreateShaderModule                    = fakeCreateShaderModule;
            device.enabledFeatures.*(requirement.enabledBit) = VK_FALSE;
            VkShaderModule output                            = fakeShaderModule;
            vkShade::createShaderModule(&device, words, &output);
            require(createCalls == 0, "disabled optional shader feature reached the Vulkan driver");
            require(output == VK_NULL_HANDLE, "rejected shader module left a stale output handle");

            device.enabledFeatures.*(requirement.enabledBit) = VK_TRUE;
            output                                           = VK_NULL_HANDLE;
            vkShade::createShaderModule(&device, words, &output);
            require(createCalls == 1, "enabled optional shader feature did not reach the Vulkan driver exactly once");
            require(output == fakeShaderModule, "enabled optional shader module did not preserve the driver output");
        }
    }

    void testByteOverloadAndOrdinaryShaderForwarding()
    {
        expectedDevice   = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x9876));
        const auto words = makeComputeShader({SpvCapabilityShader, SpvCapabilityStorageImageWriteWithoutFormat});
        auto       bytes = asBytes(words);
        setExpectedCode(bytes.data(), bytes.size());

        vkShade::LogicalDevice device{};
        device.device                 = expectedDevice;
        device.vkd.CreateShaderModule = fakeCreateShaderModule;
        VkShaderModule output         = fakeShaderModule;
        createCalls                   = 0;
        vkShade::createShaderModule(&device, bytes, &output);
        require(createCalls == 0 && output == VK_NULL_HANDLE, "byte overload did not reject a disabled required feature safely");

        device.enabledFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        output                                                      = VK_NULL_HANDLE;
        vkShade::createShaderModule(&device, bytes, &output);
        require(createCalls == 1 && output == fakeShaderModule, "byte overload did not forward enabled shader bytes");

        const auto ordinaryWords = makeComputeShader({SpvCapabilityShader});
        setExpectedCode(ordinaryWords.data(), ordinaryWords.size() * sizeof(uint32_t));
        device.enabledFeatures = {};
        output                 = VK_NULL_HANDLE;
        createCalls            = 0;
        vkShade::createShaderModule(&device, ordinaryWords, &output);
        require(createCalls == 1 && output == fakeShaderModule, "shader without mapped optional capabilities did not preserve driver behavior");
    }
} // namespace

int main()
{
    try
    {
        testCapabilityMappingsAndOrdering();
        testMalformedWordStreamsReturn();
        testCreateShaderModuleChecksEnabledFeatures();
        testByteOverloadAndOrdinaryShaderForwarding();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
