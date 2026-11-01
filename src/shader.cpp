#include "shader.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <spirv/unified1/spirv.hpp>

namespace vkShade
{
    const char* missingShaderFeature(const std::vector<uint32_t>& spirv, const VkPhysicalDeviceFeatures& enabled)
    {
        if (spirv.size() < 5 || spirv[0] != spv::MagicNumber)
            return nullptr;

        for (size_t offset = 5; offset < spirv.size();)
        {
            const uint32_t instruction = spirv[offset];
            const uint32_t wordCount   = instruction >> 16;
            if (wordCount == 0 || wordCount > spirv.size() - offset)
                break;

            if ((instruction & 0xffff) == spv::OpCapability && wordCount == 2)
            {
                switch (spirv[offset + 1])
                {
                    case spv::CapabilityImageGatherExtended:
                        if (!enabled.shaderImageGatherExtended)
                            return "shaderImageGatherExtended";
                        break;
                    case spv::CapabilityStorageImageReadWithoutFormat:
                        if (!enabled.shaderStorageImageReadWithoutFormat)
                            return "shaderStorageImageReadWithoutFormat";
                        break;
                    case spv::CapabilityStorageImageWriteWithoutFormat:
                        if (!enabled.shaderStorageImageWriteWithoutFormat)
                            return "shaderStorageImageWriteWithoutFormat";
                        break;
                    default: break;
                }
            }
            offset += wordCount;
        }
        return nullptr;
    }

    void createShaderModule(LogicalDevice* pLogicalDevice, const std::vector<char>& code, VkShaderModule* shaderModule)
    {
        if (code.empty() || code.size() % sizeof(uint32_t) != 0)
        {
            Logger::err("SPIR-V bytecode size must be a nonzero multiple of four");
            *shaderModule = VK_NULL_HANDLE;
            return;
        }

        std::vector<uint32_t> words(code.size() / sizeof(uint32_t));
        std::memcpy(words.data(), code.data(), code.size());
        createShaderModule(pLogicalDevice, words, shaderModule);
    }

    void createShaderModule(LogicalDevice* pLogicalDevice, const std::vector<uint32_t>& code, VkShaderModule* shaderModule)
    {
        if (const char* feature = missingShaderFeature(code, pLogicalDevice->enabledFeatures))
        {
            Logger::err(std::string("SPIR-V requires enabled device feature: ") + feature);
            *shaderModule = VK_NULL_HANDLE;
            return;
        }

        VkShaderModuleCreateInfo shaderCreateInfo;

        shaderCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderCreateInfo.pNext    = nullptr;
        shaderCreateInfo.flags    = 0;
        shaderCreateInfo.codeSize = code.size() * sizeof(uint32_t);
        shaderCreateInfo.pCode    = code.data();

        VkResult result = pLogicalDevice->vkd.CreateShaderModule(pLogicalDevice->device, &shaderCreateInfo, nullptr, shaderModule);
        ASSERT_VULKAN(result);
    }
} // namespace vkShade
