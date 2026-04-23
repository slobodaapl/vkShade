#ifndef LOGICAL_SWAPCHAIN_HPP_INCLUDED
#define LOGICAL_SWAPCHAIN_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "effects/effect.hpp"

#include "vulkan_include.hpp"

#include "logical_device.hpp"

namespace vkShade
{
    class Config;

    // for each swapchain, we have the Images and the other stuff we need to execute the compute shader
    struct LogicalSwapchain
    {
        LogicalDevice*                       pLogicalDevice;
        VkSwapchainCreateInfoKHR             swapchainCreateInfo;
        VkExtent2D                           imageExtent;
        VkFormat                             format;
        uint32_t                             imageCount;
        bool                                 useMutableFormat = false;
        std::vector<VkImage>                 images;
        std::vector<VkImageView>             imageViews;  // for overlay rendering
        std::vector<VkImage>                 fakeImages;
        size_t                               maxEffectSlots = 0;  // Max number of effects supported
        std::vector<VkCommandBuffer>         commandBuffersEffect;
        std::vector<VkCommandBuffer>         commandBuffersNoEffect;
        std::vector<VkSemaphore>             semaphores;
        std::vector<VkSemaphore>             overlaySemaphores;
        std::vector<std::shared_ptr<Effect>> effects;
        std::shared_ptr<Effect>              defaultTransfer;
        VkDeviceMemory                       fakeImageMemory;
        std::vector<VkImage>                 depthResolveImages;
        std::vector<VkImageView>             depthResolveImageViews;
        std::vector<VkDeviceMemory>          depthResolveMemories;
        std::vector<bool>                    depthResolveInitialized;
        VkFormat                             depthResolveFormat = VK_FORMAT_UNDEFINED;
        VkExtent3D                           depthResolveExtent = {0, 0, 1};
        bool                                 depthResolveUsesShader = false;
        VkSampler                            depthResolveSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout                depthResolveDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool                     depthResolveDescriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet>         depthResolveDescriptorSets;
        VkRenderPass                         depthResolveRenderPass = VK_NULL_HANDLE;
        VkPipelineLayout                     depthResolvePipelineLayout = VK_NULL_HANDLE;
        VkPipeline                           depthResolvePipeline = VK_NULL_HANDLE;
        std::vector<VkFramebuffer>           depthResolveFramebuffers;

        void destroy();
        void reloadEffects(Config* pConfig);
    };
} // namespace vkShade

#endif // LOGICAL_SWAPCHAIN_HPP_INCLUDED
