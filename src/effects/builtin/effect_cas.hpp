#ifndef EFFECT_CAS_HPP_INCLUDED
#define EFFECT_CAS_HPP_INCLUDED
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
    class CasEffect : public SimpleEffect
    {
    public:
        CasEffect(LogicalDevice*              pLogicalDevice,
                  VkFormat                    format,
                  VkExtent2D                  imageExtent,
                  const std::vector<VkImage>& inputImages,
                  const std::vector<VkImage>& outputImages,
                  Config*                     pConfig);
        ~CasEffect();
    };
} // namespace vkShade

#endif // EFFECT_CAS_HPP_INCLUDED
