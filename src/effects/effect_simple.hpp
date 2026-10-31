#ifndef EFFECT_SIMPLE_HPP_INCLUDED
#define EFFECT_SIMPLE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>

#include "vulkan_include.hpp"

#include "effect.hpp"
#include "config.hpp"

#include "logical_device.hpp"

namespace vkShade
{
    class SimpleEffect : public Effect
    {
    public:
        SimpleEffect();
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        virtual ~SimpleEffect();

    protected:
        LogicalDevice*               pLogicalDevice = nullptr;
        std::vector<VkImage>         inputImages;
        std::vector<VkImage>         outputImages;
        std::vector<VkImageView>     inputImageViews;
        std::vector<VkImageView>     outputImageViews;
        std::vector<VkDescriptorSet> imageDescriptorSets;
        std::vector<VkFramebuffer>   framebuffers;
        VkDescriptorSetLayout        imageSamplerDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool             descriptorPool                  = VK_NULL_HANDLE;
        VkShaderModule               vertexModule                    = VK_NULL_HANDLE;
        VkShaderModule               fragmentModule                  = VK_NULL_HANDLE;
        VkRenderPass                 renderPass                      = VK_NULL_HANDLE;
        VkPipelineLayout             pipelineLayout                  = VK_NULL_HANDLE;
        VkPipeline                   graphicsPipeline                = VK_NULL_HANDLE;
        VkExtent2D                   imageExtent                     = {};
        VkFormat                     format                          = VK_FORMAT_UNDEFINED;
        VkSampler                    sampler                         = VK_NULL_HANDLE;
        Config*                      pConfig                         = nullptr;
        std::vector<uint32_t>        vertexCode;
        std::vector<uint32_t>        fragmentCode;

        // subclasses can put DescriptorSets in here, but the first one will be the input image descriptorSet
        std::vector<VkDescriptorSetLayout> descriptorSetLayouts;

        void init(LogicalDevice*              pLogicalDevice,
                  VkFormat                    format,
                  VkExtent2D                  imageExtent,
                  const std::vector<VkImage>& inputImages,
                  const std::vector<VkImage>& outputImages,
                  Config*                     pConfig,
                  const VkSpecializationInfo* fragmentSpecializationInfo);
    };
} // namespace vkShade

#endif // EFFECT_SIMPLE_HPP_INCLUDED
