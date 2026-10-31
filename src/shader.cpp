#include "shader.hpp"

#include <array>
#include <cstring>
#include <filesystem>

namespace vkShade
{
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
