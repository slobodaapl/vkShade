#ifndef SHADER_HPP_INCLUDED
#define SHADER_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <memory>

#include "vulkan_include.hpp"

#include "logical_device.hpp"

namespace vkShade
{
    const char* missingShaderFeature(const std::vector<uint32_t>& spirv, const VkPhysicalDeviceFeatures& enabled);
    void        createShaderModule(LogicalDevice* pLogicalDevice, const std::vector<char>& code, VkShaderModule* shaderModule);
    void        createShaderModule(LogicalDevice* pLogicalDevice, const std::vector<uint32_t>& code, VkShaderModule* shaderModule);
} // namespace vkShade

#endif // SHADER_HPP_INCLUDED
