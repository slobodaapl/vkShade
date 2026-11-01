#ifndef EFFECT_RESHADE_HPP_INCLUDED
#define EFFECT_RESHADE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <array>

#include "vulkan_include.hpp"

#include "effect.hpp"
#include "config.hpp"
#include "effect_config.hpp"
#include "effect_registry.hpp"
#include "reshade_uniforms.hpp"

#include "logical_device.hpp"

#include "reshade/effect_parser.hpp"
#include "reshade/effect_codegen.hpp"
#include "reshade/effect_preprocessor.hpp"

namespace vkShade
{
    class ReshadeEffect : public Effect
    {
    public:
        ReshadeEffect(LogicalDevice*                             pLogicalDevice,
                      VkFormat                                   format,
                      VkExtent2D                                 imageExtent,
                      const std::vector<VkImage>&                inputImages,
                      const std::vector<VkImage>&                outputImages,
                      EffectRegistry*                            pEffectRegistry,
                      std::string                                effectName,
                      const std::string&                         effectPath  = "", // Optional: explicit path to .fx file
                      const std::vector<PreprocessorDefinition>& customDefs  = {}, // Custom preprocessor definitions
                      const std::optional<std::string>&          technique   = std::nullopt,
                      std::shared_ptr<ReshadeEffect>             sharedOwner = {});
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        void virtual updateEffect() override;
        void virtual useDepthImage(uint32_t imageIndex, VkImageView depthImageView, VkImageLayout depthImageLayout) override;
        std::vector<std::unique_ptr<EffectParam>> getParameters() const override;
        int                                       getOutputWrites() const
        {
            return outputWrites;
        }
        virtual ~ReshadeEffect();

    private:
        LogicalDevice*           pLogicalDevice = nullptr;
        std::vector<VkImage>     inputImages;
        std::vector<VkImage>     outputImages;
        std::vector<VkImageView> inputImageViewsSRGB;
        std::vector<VkImageView> inputImageViewsUNORM;
        std::vector<VkImageView> outputImageViewsSRGB;
        std::vector<VkImageView> outputImageViewsUNORM;

        std::unordered_map<std::string, std::vector<VkImage>>     textureImages;
        std::unordered_map<std::string, std::vector<VkImageView>> textureImageViewsUNORM;
        std::unordered_map<std::string, std::vector<VkImageView>> textureImageViewsSRGB;
        std::unordered_map<std::string, std::vector<VkImageView>> renderImageViewsSRGB;
        std::unordered_map<std::string, std::vector<VkImageView>> renderImageViewsUNORM;
        std::unordered_set<std::string>                           borrowedTextureNames;

        std::unordered_map<std::string, VkFormat>   textureFormatsUNORM;
        std::unordered_map<std::string, VkFormat>   textureFormatsSRGB;
        std::unordered_map<std::string, uint32_t>   textureMipLevels;
        std::unordered_map<std::string, VkExtent3D> textureExtents;

        std::vector<VkDescriptorSet> inputDescriptorSets;
        std::vector<VkDescriptorSet> outputDescriptorSets;
        std::vector<VkDescriptorSet> backBufferDescriptorSets;

        struct PassRuntime
        {
            bool                        isCompute  = false;
            VkPipeline                  pipeline   = VK_NULL_HANDLE;
            VkRenderPass                renderPass = VK_NULL_HANDLE;
            std::vector<VkFramebuffer>  framebuffers;
            std::vector<std::string>    renderTargets;
            bool                        switchSamplers  = false;
            uint32_t                    vertexCount     = 3;
            uint32_t                    dispatchSizeX   = 1;
            uint32_t                    dispatchSizeY   = 1;
            uint32_t                    dispatchSizeZ   = 1;
            VkRect2D                    renderArea      = {};
            uint32_t                    clearValueCount = 0;
            std::array<VkClearValue, 9> clearValues     = {};
        };
        std::vector<PassRuntime> passRuntimes;

        VkDescriptorSetLayout               uniformDescriptorSetLayout      = VK_NULL_HANDLE;
        VkDescriptorSetLayout               imageSamplerDescriptorSetLayout = VK_NULL_HANDLE;
        VkShaderModule                      shaderModule                    = VK_NULL_HANDLE;
        VkDescriptorPool                    descriptorPool                  = VK_NULL_HANDLE;
        VkPipelineLayout                    pipelineLayout                  = VK_NULL_HANDLE;
        VkExtent2D                          imageExtent                     = {};
        std::vector<VkSampler>              samplers;
        EffectRegistry*                     pEffectRegistry = nullptr;
        std::string                         effectName;
        std::string                         effectPath;             // Path to .fx file (may differ from effectName)
        std::vector<PreprocessorDefinition> customPreprocessorDefs; // User-defined macros
        reshadefx::module                   module;
        std::vector<VkDeviceMemory>         textureMemory;
        std::shared_ptr<ReshadeEffect>      sharedOwner;

        VkFormat    inputOutputFormatUNORM = VK_FORMAT_UNDEFINED;
        VkFormat    inputOutputFormatSRGB  = VK_FORMAT_UNDEFINED;
        VkFormat    stencilFormat          = VK_FORMAT_UNDEFINED;
        VkImage     stencilImage           = VK_NULL_HANDLE;
        VkImageView stencilImageView       = VK_NULL_HANDLE;
        // how often the shader writes to the reshade back buffer
        // we need to flip the "backbuffer" after each write if there is a next one
        int                      outputWrites = 0;
        std::vector<VkImage>     backBufferImages;
        std::vector<VkImageView> backBufferImageViewsUNORM;
        std::vector<VkImageView> backBufferImageViewsSRGB;
        VkBuffer                 stagingBuffer                      = VK_NULL_HANDLE;
        VkDeviceMemory           stagingBufferMemory                = VK_NULL_HANDLE;
        uint32_t                 bufferSize                         = 0;
        void*                    stagingBufferMapped                = nullptr; // Persistent map (HOST_COHERENT)
        VkDescriptorSet          bufferDescriptorSet                = VK_NULL_HANDLE;
        bool                     disableComputePipelineOptimization = false;

        std::vector<std::shared_ptr<ReshadeUniform>> uniforms;

        void          createReshadeModule();
        void          destroyResources();
        VkFormat      convertReshadeFormat(reshadefx::texture_format texFormat);
        VkCompareOp   convertReshadeCompareOp(reshadefx::pass_stencil_func compareOp);
        VkStencilOp   convertReshadeStencilOp(reshadefx::pass_stencil_op stencilOp);
        VkBlendOp     convertReshadeBlendOp(reshadefx::pass_blend_op blendOp);
        VkBlendFactor convertReshadeBlendFactor(reshadefx::pass_blend_func blendFactor);
    };
} // namespace vkShade

#endif // EFFECT_RESHADE_HPP_INCLUDED
