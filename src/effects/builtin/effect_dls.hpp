#pragma once
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>

#include "vulkan_include.hpp"

#include "../effect_simple.hpp"
#include "config.hpp"

namespace vkShade
{
    class DlsEffect : public SimpleEffect
    {
    public:
        DlsEffect(LogicalDevice*              pLogicalDevice,
                  VkFormat                    format,
                  VkExtent2D                  imageExtent,
                  const std::vector<VkImage>& inputImages,
                  const std::vector<VkImage>& outputImages,
                  Config*                     pConfig);
        ~DlsEffect();
    };
} // namespace vkShade
