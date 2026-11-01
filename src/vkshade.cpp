#include "vulkan_include.hpp"
#include "proton/host_bridge.hpp"

#include <mutex>
#include <map>
#include <set>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <string>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <dlfcn.h>
#include <sys/stat.h>
#include <signal.h>
#include <setjmp.h>
#include <execinfo.h>
#include <limits>

#include "util.hpp"
#include "keyboard_input.hpp"
#include "keyboard_input_wayland.hpp"
#include "mouse_input_wayland.hpp"
#include "wayland_input_common.hpp"
#include "input_blocker.hpp"
#include "wayland_display.hpp"

#include "logical_device.hpp"
#include "device_features.hpp"
#include "depth_clear_capture.hpp"
#include "depth_copy_submission.hpp"
#include "depth_image_capture.hpp"
#include "depth_render_pass.hpp"
#include "depth_selection.hpp"
#include "logical_swapchain.hpp"

#include "image_view.hpp"
#include "image.hpp"
#include "sampler.hpp"
#include "framebuffer.hpp"
#include "descriptor_set.hpp"
#include "shader.hpp"
#include "graphics_pipeline.hpp"
#include "command_buffer.hpp"
#include "buffer.hpp"
#include "memory.hpp"
#include "config.hpp"
#include "config_serializer.hpp"
#include "settings_manager.hpp"
#include "technique_plan.hpp"
#include "fake_swapchain.hpp"
#include "renderpass.hpp"
#include "format.hpp"
#include "logger.hpp"
#include "shader_sources.hpp"
#include "swapchain_usage.hpp"

#include "effects/effect.hpp"
#include "effects/effect_reshade.hpp"
#include "effects/effect_transfer.hpp"
#include "imgui_overlay.hpp"
#include "effects/effect_registry.hpp"

// Vulkan platform surface interception signatures used in this translation unit.
// Keep these after project headers to avoid X11 macro collisions in shared headers.
#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <wayland-client.h>
#include "vulkan/vulkan_wayland.h"
#include "vulkan/vulkan_xlib.h"
#include "vulkan/vulkan_xcb.h"

#define VKSHADE_LAYER_NAME "VK_LAYER_VKSHADE_post_processing"

#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_SHADE_EXPORT __attribute__((visibility("default")))
#else
#error "Unsupported platform!"
#endif

namespace vkShade
{
    std::shared_ptr<Config> pBaseConfig = nullptr; // Always vkShade.conf
    std::shared_ptr<Config> pConfig     = nullptr; // Current config (base + overlay)
    EffectRegistry          effectRegistry;        // Single source of truth for effect configs

    // layer book-keeping information, to store dispatch tables by key
    std::unordered_map<void*, InstanceDispatch>                           instanceDispatchMap;
    std::unordered_map<void*, VkInstance>                                 instanceMap;
    std::unordered_map<void*, uint32_t>                                   instanceVersionMap;
    std::unordered_map<void*, std::shared_ptr<LogicalDevice>>             deviceMap;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchainMap;

    std::mutex globalLock;
#ifdef _GCC_
    using scoped_lock __attribute__((unused)) = std::lock_guard<std::mutex>;
#else
    using scoped_lock = std::lock_guard<std::mutex>;
#endif

    template<typename DispatchableType>
    void* GetKey(DispatchableType inst)
    {
        return *(void**) inst;
    }

    // Cached available effects data (to avoid re-parsing config every frame)
    struct CachedEffectsData
    {
        std::vector<std::string>           currentConfigEffects;
        std::vector<std::string>           defaultConfigEffects;
        std::vector<std::string>           effectFiles;
        std::vector<std::string>           shaderIncludePaths;
        std::map<std::string, std::string> effectPaths;
        std::map<std::string, std::string> configuredEffectPaths;
        std::string                        configPath;
        bool                               initialized = false;
    };
    CachedEffectsData cachedEffects;

    // Cached parameters (to avoid re-parsing config every frame)
    struct CachedParametersData
    {
        std::vector<std::unique_ptr<EffectParam>> parameters;
        std::vector<std::string>                  effectNames; // Effects when params were collected
        std::string                               configPath;
        bool                                      dirty = true; // Set to true to force recollection
    };
    CachedParametersData cachedParams;

    static bool pNextChainContainsSType(const void* pNext, VkStructureType sType)
    {
        const VkBaseInStructure* base = reinterpret_cast<const VkBaseInStructure*>(pNext);
        while (base != nullptr)
        {
            if (base->sType == sType)
                return true;
            base = base->pNext;
        }
        return false;
    }

    static std::string formatHexU64(uint64_t value)
    {
        std::ostringstream oss;
        oss << std::hex << value;
        return oss.str();
    }

    // Debounce for resize - delays effect reload until resize stops
    struct ResizeDebounceState
    {
        std::chrono::steady_clock::time_point lastResizeTime;
        bool                                  pending = false;
    };
    ResizeDebounceState resizeDebounce;
    constexpr int64_t   RESIZE_DEBOUNCE_MS = 200;

    static bool parseBoolEnvValue(const std::string& value, bool defaultValue)
    {
        std::string s(value);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "1" || s == "true" || s == "yes" || s == "on")
            return true;
        if (s == "0" || s == "false" || s == "no" || s == "off")
            return false;
        return defaultValue;
    }

    static bool getMutableSwapchainEnvOverride(bool& hasOverride, bool defaultValue)
    {
        const char* value = std::getenv("VKSHADE_ENABLE_MUTABLE_SWAPCHAIN");
        if (value == nullptr)
        {
            hasOverride = false;
            return defaultValue;
        }

        hasOverride = true;
        return parseBoolEnvValue(value, defaultValue);
    }

    static bool isSwapchainDiagEnabled()
    {
        const char* value = std::getenv("VKSHADE_DIAG_SWAPCHAIN");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    static bool isGpuCrashDiagEnabled()
    {
        const char* value = std::getenv("VKSHADE_GPU_CRASH_DIAGNOSTICS");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    static bool isDepthCopyDumpEnabled()
    {
        const char* value = std::getenv("VKSHADE_DEBUG_DUMP_DEPTH_COPY");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    // [DEBUG-TAG: depth-resolve-trace-20260423]
    static bool isDepthResolveTraceEnabled()
    {
        const char* value = std::getenv("VKSHADE_DEBUG_TRACE_DEPTH_RESOLVE");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    // [DEBUG-TAG: depth-source-override-20260424]
    static int32_t getDepthSourceOverrideIndex(LogicalDevice* pLogicalDevice, bool* fromEnvironment = nullptr)
    {
        if (fromEnvironment)
            *fromEnvironment = false;
        const char* envValue = std::getenv("VKSHADE_DEPTH_SOURCE_INDEX");
        if (envValue != nullptr && envValue[0] != '\0')
        {
            try
            {
                int value = std::stoi(envValue);
                if (fromEnvironment)
                    *fromEnvironment = true;
                return value;
            }
            catch (...)
            {
                Logger::warn("invalid VKSHADE_DEPTH_SOURCE_INDEX value");
            }
        }

        const auto selection       = pLogicalDevice ? pLogicalDevice->depthSourceOverrideIndex : std::nullopt;
        const int  configSelection = !selection && pConfig ? pConfig->getOption<int32_t>("depthSourceIndex", DEPTH_SOURCE_AUTO) : DEPTH_SOURCE_AUTO;
        return resolveDepthSourcePreference(selection, configSelection);
    }

    static bool hasDepthSourceOverrideCandidate(LogicalDevice* pLogicalDevice)
    {
        if (!pLogicalDevice)
            return false;
        bool          fromEnvironment = false;
        const int32_t overrideIndex   = getDepthSourceOverrideIndex(pLogicalDevice, &fromEnvironment);
        const bool    usePinned       = !fromEnvironment && pLogicalDevice->depthSourcePinnedIndex == overrideIndex;
        return resolveDepthSource(pLogicalDevice->depthSourceCandidates,
                                  overrideIndex,
                                  usePinned ? pLogicalDevice->depthSourceOverrideImage : DepthState{},
                                  usePinned && pLogicalDevice->depthSourceOverrideImageDestroyed)
            .has_value();
    }

    static VkImageLayout getDepthResolveReadOnlyLayoutForDebug(const LogicalSwapchain* pLogicalSwapchain)
    {
        if (!pLogicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    static VkImageAspectFlags getDepthResolveAspectMaskForDebug(const LogicalSwapchain* pLogicalSwapchain)
    {
        if (!pLogicalSwapchain)
            return VK_IMAGE_ASPECT_COLOR_BIT;

        return pLogicalSwapchain->depthResolveUsesShader ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    static void maybeDumpDepthResolveImage(LogicalDevice* pLogicalDevice, LogicalSwapchain* pLogicalSwapchain, uint32_t imageIndex, VkQueue queue)
    {
        static bool dumped = false;
        if (dumped || !isDepthCopyDumpEnabled() || !pLogicalDevice || !pLogicalSwapchain)
            return;
        if (imageIndex >= pLogicalSwapchain->depthResolveImages.size() || imageIndex >= pLogicalSwapchain->depthResolveMemories.size()
            || imageIndex >= pLogicalSwapchain->depthResolveImageViews.size())
            return;
        if (pLogicalSwapchain->depthResolveFormat != VK_FORMAT_D32_SFLOAT && pLogicalSwapchain->depthResolveFormat != VK_FORMAT_R32_SFLOAT)
        {
            Logger::warn("depth copy dump only supports VK_FORMAT_D32_SFLOAT or VK_FORMAT_R32_SFLOAT right now, got format="
                         + convertToString(pLogicalSwapchain->depthResolveFormat));
            dumped = true;
            return;
        }

        VkExtent3D extent = pLogicalSwapchain->depthResolveExtent;
        if (extent.width == 0 || extent.height == 0)
            return;

        const bool initialized =
            imageIndex < pLogicalSwapchain->depthResolveInitialized.size() && pLogicalSwapchain->depthResolveInitialized[imageIndex];
        // [DEBUG-TAG: depth-dump-source-20260423]
        Logger::warn(
            "depth dump source: imageIndex=" + std::to_string(imageIndex) + " initialized=" + std::string(initialized ? "true" : "false")
            + " resolveImage=" + convertToString(pLogicalSwapchain->depthResolveImages[imageIndex])
            + " resolveView=" + convertToString(pLogicalSwapchain->depthResolveImageViews[imageIndex])
            + " resolveFormat=" + convertToString(pLogicalSwapchain->depthResolveFormat)
            + " resolveUsesShader=" + std::string(pLogicalSwapchain->depthResolveUsesShader ? "true" : "false")
            + " readLayout=" + convertToString(getDepthResolveReadOnlyLayoutForDebug(pLogicalSwapchain)) + " activeImage="
            + convertToString(pLogicalDevice->activeDepthState.image) + " activeView=" + convertToString(pLogicalDevice->activeDepthState.imageView)
            + " activeFormat=" + convertToString(pLogicalDevice->activeDepthState.format) + " activeExtent="
            + std::to_string(pLogicalDevice->activeDepthState.extent.width) + "x" + std::to_string(pLogicalDevice->activeDepthState.extent.height)
            + " activeLayout=" + convertToString(pLogicalDevice->activeDepthState.observedLayout)
            + " resolveSourceImage=" + convertToString(pLogicalSwapchain->depthResolveSourceImage)
            + " resolveSourceView=" + convertToString(pLogicalSwapchain->depthResolveSourceImageView));

        auto metadataIt = pLogicalDevice->depthImageMetadata.find(pLogicalDevice->activeDepthState.image);
        if (metadataIt != pLogicalDevice->depthImageMetadata.end())
        {
            Logger::warn("depth dump source metadata: image=" + convertToString(pLogicalDevice->activeDepthState.image) + " usage=0x"
                         + formatHexU64(static_cast<uint64_t>(metadataIt->second.usage)) + " samples=" + convertToString(metadataIt->second.samples)
                         + " tiling=" + convertToString(metadataIt->second.tiling)
                         + " transient=" + std::string((metadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));
        }

        uint32_t candidateIndex = 0;
        for (const auto& candidate : pLogicalDevice->frameDepthCandidates)
        {
            Logger::warn(
                "depth dump frame candidate[" + std::to_string(candidateIndex++) + "]: image=" + convertToString(candidate.depthState.image)
                + " view=" + convertToString(candidate.depthState.imageView) + " extent=" + std::to_string(candidate.depthState.extent.width) + "x"
                + std::to_string(candidate.depthState.extent.height) + " layout=" + convertToString(candidate.depthState.observedLayout)
                + " vertices=" + std::to_string(candidate.stats.vertices) + " drawcalls=" + std::to_string(candidate.stats.drawcalls) + " indirect="
                + std::to_string(candidate.stats.drawcallsIndirect) + " snapshot.swapchain=" + convertToString(candidate.snapshotTarget.swapchain)
                + " snapshot.index=" + std::to_string(candidate.snapshotTarget.imageIndex));
        }

        if (isDepthResolveTraceEnabled())
            Logger::debug("depth resolve trace dump: imageIndex=" + std::to_string(imageIndex)
                          + " initialized=" + std::string(initialized ? "true" : "false")
                          + " resolveImage=" + convertToString(pLogicalSwapchain->depthResolveImages[imageIndex])
                          + " resolveView=" + convertToString(pLogicalSwapchain->depthResolveImageViews[imageIndex])
                          + " readLayout=" + convertToString(getDepthResolveReadOnlyLayoutForDebug(pLogicalSwapchain)));

        dumped = true;

        // One-shot debug path: wait for prior work so the sampled backup image is stable.
        pLogicalDevice->vkd.QueueWaitIdle(queue);

        const VkDeviceSize dumpSize      = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * sizeof(float);
        VkBuffer           stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory     stagingMemory = VK_NULL_HANDLE;
        createBuffer(pLogicalDevice,
                     dumpSize,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer,
                     stagingMemory);

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool                 = pLogicalDevice->commandPool;
        allocInfo.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount          = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkResult        vr            = pLogicalDevice->vkd.AllocateCommandBuffers(pLogicalDevice->device, &allocInfo, &commandBuffer);
        ASSERT_VULKAN(vr);
        initializeDispatchTable(commandBuffer, pLogicalDevice->device);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vr                                 = pLogicalDevice->vkd.BeginCommandBuffer(commandBuffer, &beginInfo);
        ASSERT_VULKAN(vr);

        VkImageMemoryBarrier barrier            = {};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image                           = pLogicalSwapchain->depthResolveImages[imageIndex];
        barrier.oldLayout                       = getDepthResolveReadOnlyLayoutForDebug(pLogicalSwapchain);
        barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask     = getDepthResolveAspectMaskForDebug(pLogicalSwapchain);
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &barrier);

        VkBufferImageCopy region               = {};
        region.bufferOffset                    = 0;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = getDepthResolveAspectMaskForDebug(pLogicalSwapchain);
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageExtent.width               = extent.width;
        region.imageExtent.height              = extent.height;
        region.imageExtent.depth               = 1;

        pLogicalDevice->vkd.CmdCopyImageToBuffer(
            commandBuffer, pLogicalSwapchain->depthResolveImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = getDepthResolveReadOnlyLayoutForDebug(pLogicalSwapchain);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &barrier);

        vr = pLogicalDevice->vkd.EndCommandBuffer(commandBuffer);
        ASSERT_VULKAN(vr);

        VkSubmitInfo submitInfo       = {};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &commandBuffer;
        vr                            = pLogicalDevice->vkd.QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        ASSERT_VULKAN(vr);
        pLogicalDevice->vkd.QueueWaitIdle(queue);

        void* mapped = nullptr;
        vr           = pLogicalDevice->vkd.MapMemory(pLogicalDevice->device, stagingMemory, 0, dumpSize, 0, &mapped);
        ASSERT_VULKAN(vr);

        const float* values       = static_cast<const float*>(mapped);
        const size_t pixelCount   = static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height);
        float        minValue     = std::numeric_limits<float>::infinity();
        float        maxValue     = -std::numeric_limits<float>::infinity();
        size_t       finiteCount  = 0;
        size_t       zeroishCount = 0;
        size_t       oneishCount  = 0;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const float v = values[i];
            if (!std::isfinite(v))
                continue;
            finiteCount++;
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
            if (std::abs(v) < 1e-6f)
                zeroishCount++;
            if (std::abs(v - 1.0f) < 1e-6f)
                oneishCount++;
        }

        auto sampleAt = [&](uint32_t x, uint32_t y) -> float { return values[static_cast<size_t>(y) * static_cast<size_t>(extent.width) + x]; };

        const uint32_t centerX = extent.width / 2;
        const uint32_t centerY = extent.height / 2;
        Logger::warn("depth copy dump: imageIndex=" + std::to_string(imageIndex) + " extent=" + std::to_string(extent.width) + "x"
                     + std::to_string(extent.height) + " finite=" + std::to_string(finiteCount) + " min=" + std::to_string(minValue)
                     + " max=" + std::to_string(maxValue) + " zeroish=" + std::to_string(zeroishCount) + " oneish=" + std::to_string(oneishCount)
                     + " sample(0,0)=" + std::to_string(sampleAt(0, 0)) + " sample(center)=" + std::to_string(sampleAt(centerX, centerY))
                     + " sample(last,last)=" + std::to_string(sampleAt(extent.width - 1, extent.height - 1)));

        const std::string dumpPath = "/tmp/vkshade-depth-copy-" + std::to_string(imageIndex) + ".f32";
        {
            std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
            dump.write(reinterpret_cast<const char*>(values), static_cast<std::streamsize>(dumpSize));
        }
        Logger::warn("depth copy dump written to " + dumpPath);

        pLogicalDevice->vkd.UnmapMemory(pLogicalDevice->device, stagingMemory);
        pLogicalDevice->vkd.FreeCommandBuffers(pLogicalDevice->device, pLogicalDevice->commandPool, 1, &commandBuffer);
        pLogicalDevice->vkd.DestroyBuffer(pLogicalDevice->device, stagingBuffer, nullptr);
        pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, stagingMemory, nullptr);
    }

    static void logNvQueueCheckpointData(LogicalDevice* pLogicalDevice, VkQueue queue, const char* context)
    {
        if (!pLogicalDevice || !pLogicalDevice->supportsNvDiagnosticCheckpoints)
            return;

        if (pLogicalDevice->vkd.GetQueueCheckpointData2NV)
        {
            uint32_t checkpointCount = 0;
            pLogicalDevice->vkd.GetQueueCheckpointData2NV(queue, &checkpointCount, nullptr);
            if (checkpointCount == 0)
            {
                Logger::warn(std::string(context) + ": no VK_NV checkpoint data available");
                return;
            }

            std::vector<VkCheckpointData2NV> checkpoints(checkpointCount);
            for (auto& checkpoint : checkpoints)
            {
                checkpoint       = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV;
            }
            pLogicalDevice->vkd.GetQueueCheckpointData2NV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(std::string(context) + ": checkpoint[" + std::to_string(i) + "] stage=0x"
                             + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) + " marker=0x"
                             + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
            }
            return;
        }

        if (pLogicalDevice->vkd.GetQueueCheckpointDataNV)
        {
            uint32_t checkpointCount = 0;
            pLogicalDevice->vkd.GetQueueCheckpointDataNV(queue, &checkpointCount, nullptr);
            if (checkpointCount == 0)
            {
                Logger::warn(std::string(context) + ": no VK_NV checkpoint data available");
                return;
            }

            std::vector<VkCheckpointDataNV> checkpoints(checkpointCount);
            for (auto& checkpoint : checkpoints)
            {
                checkpoint       = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
            }
            pLogicalDevice->vkd.GetQueueCheckpointDataNV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(std::string(context) + ": checkpoint[" + std::to_string(i) + "] stage=0x"
                             + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) + " marker=0x"
                             + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
            }
        }
    }

    static void logDeviceFaultInfo(LogicalDevice* pLogicalDevice, const char* context)
    {
        if (!pLogicalDevice || !pLogicalDevice->supportsDeviceFaultExt || !pLogicalDevice->vkd.GetDeviceFaultInfoEXT)
            return;

        VkDeviceFaultCountsEXT faultCounts = {};
        faultCounts.sType                  = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        VkResult countsResult              = pLogicalDevice->vkd.GetDeviceFaultInfoEXT(pLogicalDevice->device, &faultCounts, nullptr);
        if (countsResult != VK_SUCCESS && countsResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(counts) failed: " + std::to_string(countsResult));
            return;
        }

        std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(faultCounts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT>  vendorInfos(faultCounts.vendorInfoCount);
        std::vector<uint8_t>                     vendorBinary(faultCounts.vendorBinarySize);

        VkDeviceFaultInfoEXT faultInfo = {};
        faultInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        faultInfo.pAddressInfos        = addressInfos.empty() ? nullptr : addressInfos.data();
        faultInfo.pVendorInfos         = vendorInfos.empty() ? nullptr : vendorInfos.data();
        faultInfo.pVendorBinaryData    = vendorBinary.empty() ? nullptr : vendorBinary.data();

        VkResult infoResult = pLogicalDevice->vkd.GetDeviceFaultInfoEXT(pLogicalDevice->device, &faultCounts, &faultInfo);
        if (infoResult != VK_SUCCESS && infoResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(info) failed: " + std::to_string(infoResult));
            return;
        }

        Logger::err(std::string(context) + ": device fault description=\"" + faultInfo.description
                    + "\", addresses=" + std::to_string(faultCounts.addressInfoCount) + ", vendorInfos=" + std::to_string(faultCounts.vendorInfoCount)
                    + ", vendorBinaryBytes=" + std::to_string(faultCounts.vendorBinarySize));

        for (uint32_t i = 0; i < faultCounts.addressInfoCount; ++i)
        {
            const auto& address = addressInfos[i];
            Logger::warn(std::string(context) + ": faultAddress[" + std::to_string(i) + "] type=" + std::to_string(address.addressType)
                         + " reported=0x" + formatHexU64(address.reportedAddress) + " precision=0x" + formatHexU64(address.addressPrecision));
        }
    }

    static void reportDeviceLostDiagnostics(LogicalDevice* pLogicalDevice, VkQueue queue, const char* context, VkResult result)
    {
        if (result != VK_ERROR_DEVICE_LOST || !pLogicalDevice || !pLogicalDevice->gpuCrashDiagnosticsEnabled)
            return;

        Logger::err(std::string(context) + ": VK_ERROR_DEVICE_LOST (gathering diagnostics)");
        logNvQueueCheckpointData(pLogicalDevice, queue, context);
        logDeviceFaultInfo(pLogicalDevice, context);
    }

    // Signal-safe crash recovery for SIGFPE/SIGABRT from embedded reshadefx compiler.
    // These signals cannot be caught by C++ try-catch — they require signal handlers.
    static thread_local sigjmp_buf            signalJmpBuf;
    static thread_local volatile sig_atomic_t signalJmpActive = 0;
    static thread_local volatile sig_atomic_t caughtSignal    = 0;

    static void crashSignalHandler(int sig)
    {
        if (signalJmpActive)
        {
            caughtSignal = sig;
            siglongjmp(signalJmpBuf, 1);
        }
        // Print backtrace before crashing so we can find the source
        const char* sigName = (sig == SIGFPE) ? "SIGFPE" : (sig == SIGABRT) ? "SIGABRT" : "SIGNAL";
        fprintf(stderr, "\nvkShade: caught %s — backtrace:\n", sigName);
        void* frames[64];
        int   count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, 2); // fd 2 = stderr
        fprintf(stderr, "\n");
        // Restore default handler and re-raise
        signal(sig, SIG_DFL);
        raise(sig);
    }

    static void installCrashHandlers()
    {
        static bool installed = false;
        if (installed)
            return;
        struct sigaction sa = {};
        sa.sa_handler       = crashSignalHandler;
        sa.sa_flags         = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGFPE, &sa, nullptr);
        sigaction(SIGABRT, &sa, nullptr);
        installed = true;
    }

    // Helper for key press with debounce - returns true on key-down edge
    bool handleKeyPress(uint32_t keySymbol, bool& wasPressed)
    {
        if (isKeyPressed(keySymbol))
        {
            if (!wasPressed)
            {
                wasPressed = true;
                return true;
            }
        }
        else
        {
            wasPressed = false;
        }
        return false;
    }

    bool hasDepthState(const DepthState& state)
    {
        return state.image != VK_NULL_HANDLE && state.imageView != VK_NULL_HANDLE && state.format != VK_FORMAT_UNDEFINED;
    }

    static bool isDepthStencilAttachmentFormat(VkFormat format)
    {
        return isDepthFormat(format) || isStencilFormat(format);
    }

    static bool needsStoredAttachmentPreservation(VkAttachmentStoreOp storeOp)
    {
        return storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE
#ifdef VK_ATTACHMENT_STORE_OP_NONE
               || storeOp == VK_ATTACHMENT_STORE_OP_NONE
#endif
            ;
    }

    static bool forceDepthAttachmentStoreOp(VkAttachmentDescription& attachment)
    {
        if (!isDepthStencilAttachmentFormat(attachment.format))
            return false;

        bool changed = false;
        if (needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed            = true;
        }
        if (isStencilFormat(attachment.format) && needsStoredAttachmentPreservation(attachment.stencilStoreOp))
        {
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed                   = true;
        }
        return changed;
    }

    static bool forceDepthAttachmentStoreOp(VkAttachmentDescription2& attachment)
    {
        if (!isDepthStencilAttachmentFormat(attachment.format))
            return false;

        bool changed = false;
        if (needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed            = true;
        }
        if (isStencilFormat(attachment.format) && needsStoredAttachmentPreservation(attachment.stencilStoreOp))
        {
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed                   = true;
        }
        return changed;
    }

    static bool forceDepthAttachmentStoreOp(VkRenderingAttachmentInfo& attachment, bool hasStencilAspect)
    {
        bool changed = false;
        if (attachment.imageView != VK_NULL_HANDLE && needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed            = true;
        }
        if (hasStencilAspect && attachment.imageView != VK_NULL_HANDLE && needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed            = true;
        }
        return changed;
    }

    bool sameDepthState(const DepthState& a, const DepthState& b)
    {
        return a.image == b.image && a.imageView == b.imageView && a.sourceId == b.sourceId && a.format == b.format
               && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.extent.depth == b.extent.depth
               && a.observedLayout == b.observedLayout;
    }

    static bool hasPresentableSnapshotTarget(const DepthSnapshotTarget& target)
    {
        return target.swapchain != VK_NULL_HANDLE && target.pLogicalSwapchain != nullptr && target.imageIndex < target.pLogicalSwapchain->imageCount;
    }

    static bool matchesPresentableSnapshotTargetExtent(const DepthState& depth, const DepthSnapshotTarget& target)
    {
        return hasPresentableSnapshotTarget(target) && depth.extent.width == target.pLogicalSwapchain->imageExtent.width
               && depth.extent.height == target.pLogicalSwapchain->imageExtent.height;
    }

    static bool matchesAnySwapchainExtent(LogicalDevice* pLogicalDevice, const DepthState& depth)
    {
        if (!pLogicalDevice || !hasDepthState(depth))
            return false;

        for (auto& [_, pLogicalSwapchain] : swapchainMap)
        {
            if (!pLogicalSwapchain || pLogicalSwapchain->pLogicalDevice != pLogicalDevice)
                continue;
            const auto extent = pLogicalSwapchain->imageExtent;
            if (!extent.width || !extent.height || !depth.extent.width || !depth.extent.height)
                continue;
            const float widthRatio  = static_cast<float>(depth.extent.width) / extent.width;
            const float heightRatio = static_cast<float>(depth.extent.height) / extent.height;
            const float aspectDelta =
                std::abs(static_cast<float>(depth.extent.width) / depth.extent.height - static_cast<float>(extent.width) / extent.height);
            if (aspectDelta <= 0.1f && widthRatio >= 0.5f && widthRatio <= 1.85f && heightRatio >= 0.5f && heightRatio <= 1.85f)
                return true;
        }

        return false;
    }

    static void resetDepthSelectionForSwapchainRebuildLocked(LogicalDevice* pLogicalDevice)
    {
        if (!pLogicalDevice)
            return;

        pLogicalDevice->frameDepthCandidates.clear();
        pLogicalDevice->depthSourceCandidates.clear();
        pLogicalDevice->automaticDepthSelection             = {};
        pLogicalDevice->automaticClearSelection             = {};
        pLogicalDevice->depthFrameCadence                   = {};
        pLogicalDevice->bestDepthCandidate                  = {};
        pLogicalDevice->activeDepthState                    = {};
        pLogicalDevice->activeDepthStatePresentTimeFallback = false;
    }

    // Returns true if `lhs` is a better depth-scope candidate than `rhs`,
    // using ReShade's `prefer_vertices` heuristic: compare vertex count
    // first (scene meshes have far more vertices than alpha-tested quads),
    // fall back to drawcalls only when vertex totals are equal (apps using
    // mostly indirect draws report 0 vertices at record time).
    static bool depthScopeBeatsCandidate(const LogicalDevice::DrawStats& lhs, const LogicalDevice::DrawStats& rhs)
    {
        if (lhs.vertices != rhs.vertices)
            return lhs.vertices > rhs.vertices;
        if (lhs.drawcalls != rhs.drawcalls)
            return lhs.drawcalls > rhs.drawcalls;
        return lhs.drawcallsIndirect > rhs.drawcallsIndirect;
    }

    static const LogicalDevice::DepthCandidateTrackingState* selectDepthSourceOverrideCandidate(
        LogicalDevice* pLogicalDevice, int32_t overrideIndex, LogicalDevice::DepthCandidateTrackingState& selectedCandidate, bool fromEnvironment)
    {
        if (!pLogicalDevice)
            return nullptr;
        const bool usePinned = !fromEnvironment && pLogicalDevice->depthSourcePinnedIndex == overrideIndex;
        const auto index     = resolveActiveDepthSource(pLogicalDevice->depthSourceCandidates,
                                                        overrideIndex,
                                                        usePinned ? pLogicalDevice->depthSourceOverrideImage : DepthState{},
                                                        usePinned && pLogicalDevice->depthSourceOverrideImageDestroyed);
        if (!index)
            return nullptr;
        const auto& sourceCandidate = pLogicalDevice->depthSourceCandidates[*index];
        if (!sourceCandidate.depthState.capturable)
            return nullptr;
        selectedCandidate            = {};
        selectedCandidate.valid      = true;
        selectedCandidate.depthState = sourceCandidate.depthState;
        selectedCandidate.stats      = sourceCandidate.stats;
        return &selectedCandidate;
    }

    static bool publishDepthSourceCandidatesLocked(LogicalDevice* pLogicalDevice)
    {
        if (!pLogicalDevice)
            return false;

        auto submitted = pLogicalDevice->depthFrameTracker.finishFrame();
        if (!acceptDepthFrame(submitted, pLogicalDevice->depthSourceCandidates, pLogicalDevice->depthFrameCadence, std::chrono::steady_clock::now()))
            return false;
        const bool hasWork                    = !submitted.empty();
        pLogicalDevice->depthSourceCandidates = retainDepthSourceHistory(std::move(submitted), std::move(pLogicalDevice->depthSourceCandidates));
        return hasWork;
    }

    static bool isAutomaticDepthCandidate(LogicalDevice* device, const LogicalDevice::DepthSourceCandidate& candidate)
    {
        const auto& stats = candidate.stats;
        return !candidate.stale && hasDepthState(candidate.depthState) && candidate.depthState.capturable
               && (stats.drawcalls != 0 || stats.drawcallsIndirect != 0) && (stats.vertices > 3 || stats.drawcallsIndirect != 0)
               && matchesAnySwapchainExtent(device, candidate.depthState);
    }

    static const LogicalDevice::DepthSourceCandidate* selectAutomaticDepthCandidate(LogicalDevice* device)
    {
        AutomaticDepthSelection ranking;
        const auto              index = selectStableAutomaticDepthSource(
            device->depthSourceCandidates, ranking, {}, [device](const auto& candidate) { return isAutomaticDepthCandidate(device, candidate); });
        return index ? &device->depthSourceCandidates[*index] : nullptr;
    }

    static bool isQualifiedDepthCandidate(LogicalDevice* pLogicalDevice, const LogicalDevice::DepthScopeTrackingState& scopeState)
    {
        if (!hasDepthState(scopeState.depthState))
            return false;
        if (scopeState.stats.vertices == 0 && scopeState.stats.drawcalls == 0 && scopeState.stats.drawcallsIndirect == 0)
            return false;
        if (hasPresentableSnapshotTarget(scopeState.snapshotTarget))
            return true;
        return false;
    }

    template<typename Predicate>
    void clearTrackedDepthScopesLocked(LogicalDevice* pLogicalDevice, Predicate predicate, bool preserveImage = false)
    {
        if (pLogicalDevice->depthSourceOverrideImage.image != VK_NULL_HANDLE && predicate(pLogicalDevice->depthSourceOverrideImage))
        {
            if (preserveImage)
                pLogicalDevice->depthSourceOverrideImage.imageView = VK_NULL_HANDLE;
            else
            {
                pLogicalDevice->depthSourceOverrideImage          = {};
                pLogicalDevice->depthSourceOverrideImageDestroyed = true;
            }
        }
        for (auto it = pLogicalDevice->commandBufferDepthStates.begin(); it != pLogicalDevice->commandBufferDepthStates.end();)
        {
            if (predicate(it->second.depthState))
                it = pLogicalDevice->commandBufferDepthStates.erase(it);
            else
                ++it;
        }

        for (auto it = pLogicalDevice->pendingTransferLinkedDepthScopes.begin(); it != pLogicalDevice->pendingTransferLinkedDepthScopes.end();)
        {
            if (predicate(it->second.depthState))
                it = pLogicalDevice->pendingTransferLinkedDepthScopes.erase(it);
            else
                ++it;
        }

        for (auto it = pLogicalDevice->frameDepthCandidates.begin(); it != pLogicalDevice->frameDepthCandidates.end();)
        {
            if (predicate(it->depthState))
                it = pLogicalDevice->frameDepthCandidates.erase(it);
            else
                ++it;
        }

        for (auto& candidate : pLogicalDevice->depthSourceCandidates)
            if (candidate.depthState.image != VK_NULL_HANDLE && predicate(candidate.depthState))
            {
                if (preserveImage)
                {
                    candidate.depthState.imageView = VK_NULL_HANDLE;
                    markDepthSourceStale(candidate);
                }
                else
                    candidate = {};
            }
        while (!pLogicalDevice->depthSourceCandidates.empty() && pLogicalDevice->depthSourceCandidates.back().depthState.image == VK_NULL_HANDLE)
            pLogicalDevice->depthSourceCandidates.pop_back();

        if (pLogicalDevice->bestDepthCandidate.valid && predicate(pLogicalDevice->bestDepthCandidate.depthState))
            pLogicalDevice->bestDepthCandidate = {};
    }

    static void rememberColorOnlySnapshotScope(LogicalDevice*             pLogicalDevice,
                                               VkCommandBuffer            commandBuffer,
                                               const DepthState&          depthState,
                                               const DepthSnapshotTarget& snapshotTarget)
    {
        if (!pLogicalDevice)
            return;

        pLogicalDevice->commandBufferColorOnlySnapshotTargets.erase(commandBuffer);
        if (!hasDepthState(depthState) && hasPresentableSnapshotTarget(snapshotTarget))
            pLogicalDevice->commandBufferColorOnlySnapshotTargets[commandBuffer] = snapshotTarget;
    }

    static DepthSnapshotTarget takeColorOnlySnapshotScope(LogicalDevice* pLogicalDevice, VkCommandBuffer commandBuffer)
    {
        DepthSnapshotTarget target;
        if (!pLogicalDevice)
            return target;

        auto it = pLogicalDevice->commandBufferColorOnlySnapshotTargets.find(commandBuffer);
        if (it == pLogicalDevice->commandBufferColorOnlySnapshotTargets.end())
            return target;

        target = it->second;
        pLogicalDevice->commandBufferColorOnlySnapshotTargets.erase(it);
        return target;
    }

    template<typename CreateInfo>
    static void trackRenderPassMetadata(LogicalDevice* pLogicalDevice, VkRenderPass renderPass, const CreateInfo* pCreateInfo)
    {
        if (!pLogicalDevice || renderPass == VK_NULL_HANDLE || !pCreateInfo)
            return;
        pLogicalDevice->renderPassMetadata[renderPass] = depthRenderPassMetadata(*pCreateInfo);
    }

    static const RenderPassMetadata* findRenderPassMetadata(LogicalDevice* pLogicalDevice, VkRenderPass renderPass)
    {
        if (!pLogicalDevice || renderPass == VK_NULL_HANDLE)
            return nullptr;

        auto it = pLogicalDevice->renderPassMetadata.find(renderPass);
        if (it == pLogicalDevice->renderPassMetadata.end())
            return nullptr;

        return &it->second;
    }

    static bool selectDepthStateFromAttachments(
        LogicalDevice* pLogicalDevice, VkRenderPass renderPass, const VkImageView* attachments, uint32_t attachmentCount, DepthState& depth)
    {
        if (!pLogicalDevice || !attachments || attachmentCount == 0)
            return false;

        const auto* renderPassMetadata = findRenderPassMetadata(pLogicalDevice, renderPass);
        if (renderPassMetadata != nullptr && renderPassMetadata->depthAttachmentIndex != 0xFFFFFFFFu
            && renderPassMetadata->depthAttachmentIndex < attachmentCount)
        {
            auto viewIt = pLogicalDevice->depthViewStates.find(attachments[renderPassMetadata->depthAttachmentIndex]);
            if (viewIt != pLogicalDevice->depthViewStates.end())
            {
                depth = viewIt->second;
                if (renderPassMetadata->depthAttachmentIndex < renderPassMetadata->attachmentFinalLayouts.size())
                    depth.observedLayout = renderPassMetadata->attachmentFinalLayouts[renderPassMetadata->depthAttachmentIndex];
                return true;
            }

            return false;
        }

        if (renderPassMetadata != nullptr)
            return false;

        for (uint32_t i = 0; i < attachmentCount; ++i)
        {
            auto viewIt = pLogicalDevice->depthViewStates.find(attachments[i]);
            if (viewIt != pLogicalDevice->depthViewStates.end())
            {
                depth = viewIt->second;
                return true;
            }
        }

        return false;
    }

    DepthState selectDepthStateFromRenderPassBegin(LogicalDevice* pLogicalDevice, const VkRenderPassBeginInfo* pRenderPassBegin)
    {
        DepthState depth;
        if (!pRenderPassBegin)
            return depth;

        bool       found                 = false;
        const bool hasRenderPassMetadata = findRenderPassMetadata(pLogicalDevice, pRenderPassBegin->renderPass) != nullptr;
        for (const VkBaseInStructure* next = reinterpret_cast<const VkBaseInStructure*>(pRenderPassBegin->pNext); next; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                continue;

            const auto* attachmentBeginInfo = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);
            if (selectDepthStateFromAttachments(
                    pLogicalDevice, pRenderPassBegin->renderPass, attachmentBeginInfo->pAttachments, attachmentBeginInfo->attachmentCount, depth))
            {
                found = true;
                break;
            }

            if (!hasRenderPassMetadata)
            {
                for (uint32_t i = 0; i < attachmentBeginInfo->attachmentCount; i++)
                {
                    auto viewIt = pLogicalDevice->depthViewStates.find(attachmentBeginInfo->pAttachments[i]);
                    if (viewIt != pLogicalDevice->depthViewStates.end())
                    {
                        depth = viewIt->second;
                        found = true;
                        break;
                    }
                }
            }

            if (found)
                break;
        }

        if (!found)
        {
            auto fbIt = pLogicalDevice->framebufferDepthStates.find(pRenderPassBegin->framebuffer);
            if (fbIt != pLogicalDevice->framebufferDepthStates.end())
                depth = fbIt->second;
        }

        return depth;
    }

    DepthState selectDepthStateFromRenderingInfo(LogicalDevice* pLogicalDevice, const VkRenderingInfo* pRenderingInfo)
    {
        DepthState depth;
        if (!pRenderingInfo || !pRenderingInfo->pDepthAttachment || pRenderingInfo->pDepthAttachment->imageView == VK_NULL_HANDLE)
            return depth;

        auto it = pLogicalDevice->depthViewStates.find(pRenderingInfo->pDepthAttachment->imageView);
        if (it != pLogicalDevice->depthViewStates.end())
            depth = it->second;

        depth.observedLayout = pRenderingInfo->pDepthAttachment->imageLayout;

        return depth;
    }

    static bool findDepthSnapshotTargetForImageView(LogicalDevice* pLogicalDevice, VkImageView imageView, DepthSnapshotTarget& target)
    {
        if (imageView == VK_NULL_HANDLE)
            return false;

        auto trackedViewIt = pLogicalDevice->snapshotTargetViewStates.find(imageView);
        if (trackedViewIt != pLogicalDevice->snapshotTargetViewStates.end())
        {
            target = trackedViewIt->second;
            return target.swapchain != VK_NULL_HANDLE;
        }

        for (auto& [swapchainHandle, pLogicalSwapchain] : swapchainMap)
        {
            if (!pLogicalSwapchain || pLogicalSwapchain->pLogicalDevice != pLogicalDevice)
                continue;

            auto imageIt = std::find(pLogicalSwapchain->imageViews.begin(), pLogicalSwapchain->imageViews.end(), imageView);
            if (imageIt == pLogicalSwapchain->imageViews.end())
                continue;

            target.swapchain         = swapchainHandle;
            target.pLogicalSwapchain = pLogicalSwapchain.get();
            target.imageIndex        = static_cast<uint32_t>(imageIt - pLogicalSwapchain->imageViews.begin());
            return true;
        }

        return false;
    }

    static DepthSnapshotTarget selectDepthSnapshotTargetFromImage(LogicalDevice* pLogicalDevice, VkImage image)
    {
        DepthSnapshotTarget target;
        if (image == VK_NULL_HANDLE)
            return target;

        for (auto& [swapchainHandle, pLogicalSwapchain] : swapchainMap)
        {
            if (!pLogicalSwapchain || pLogicalSwapchain->pLogicalDevice != pLogicalDevice)
                continue;

            auto realImageIt = std::find(pLogicalSwapchain->images.begin(), pLogicalSwapchain->images.end(), image);
            if (realImageIt != pLogicalSwapchain->images.end())
            {
                target.swapchain         = swapchainHandle;
                target.pLogicalSwapchain = pLogicalSwapchain.get();
                target.imageIndex        = static_cast<uint32_t>(realImageIt - pLogicalSwapchain->images.begin());
                return target;
            }

            const size_t presentableFakeCount = std::min<size_t>(pLogicalSwapchain->fakeImages.size(), pLogicalSwapchain->imageCount);
            auto fakeImageIt = std::find(pLogicalSwapchain->fakeImages.begin(), pLogicalSwapchain->fakeImages.begin() + presentableFakeCount, image);
            if (fakeImageIt != pLogicalSwapchain->fakeImages.begin() + presentableFakeCount)
            {
                target.swapchain         = swapchainHandle;
                target.pLogicalSwapchain = pLogicalSwapchain.get();
                target.imageIndex        = static_cast<uint32_t>(fakeImageIt - pLogicalSwapchain->fakeImages.begin());
                return target;
            }
        }

        return target;
    }

    static DepthSnapshotTarget
    selectDepthSnapshotTargetFromImageViews(LogicalDevice* pLogicalDevice, const VkImageView* imageViews, uint32_t imageViewCount)
    {
        DepthSnapshotTarget target;
        if (!imageViews)
            return target;

        for (uint32_t i = 0; i < imageViewCount; ++i)
        {
            if (findDepthSnapshotTargetForImageView(pLogicalDevice, imageViews[i], target))
                return target;
        }

        return target;
    }

    static DepthSnapshotTarget selectDepthSnapshotTargetFromRenderPassBegin(LogicalDevice*               pLogicalDevice,
                                                                            const VkRenderPassBeginInfo* pRenderPassBegin)
    {
        DepthSnapshotTarget target;
        if (!pRenderPassBegin)
            return target;

        for (const VkBaseInStructure* next = reinterpret_cast<const VkBaseInStructure*>(pRenderPassBegin->pNext); next; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                continue;

            const auto* attachmentBeginInfo = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);
            target = selectDepthSnapshotTargetFromImageViews(pLogicalDevice, attachmentBeginInfo->pAttachments, attachmentBeginInfo->attachmentCount);
            if (target.swapchain != VK_NULL_HANDLE)
                return target;
        }

        auto fbTargetIt = pLogicalDevice->framebufferSnapshotTargets.find(pRenderPassBegin->framebuffer);
        if (fbTargetIt != pLogicalDevice->framebufferSnapshotTargets.end())
            target = fbTargetIt->second;

        return target;
    }

    static DepthSnapshotTarget selectDepthSnapshotTargetFromRenderingInfo(LogicalDevice* pLogicalDevice, const VkRenderingInfo* pRenderingInfo)
    {
        DepthSnapshotTarget target;
        if (!pRenderingInfo)
            return target;

        if (pRenderingInfo->pColorAttachments)
        {
            for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; ++i)
            {
                if (findDepthSnapshotTargetForImageView(pLogicalDevice, pRenderingInfo->pColorAttachments[i].imageView, target))
                    return target;
            }
        }

        if (pRenderingInfo->pDepthAttachment
            && findDepthSnapshotTargetForImageView(pLogicalDevice, pRenderingInfo->pDepthAttachment->imageView, target))
            return target;

        if (pRenderingInfo->pStencilAttachment
            && findDepthSnapshotTargetForImageView(pLogicalDevice, pRenderingInfo->pStencilAttachment->imageView, target))
            return target;

        return target;
    }

    void beginTrackedDepthScope(LogicalDevice*      pLogicalDevice,
                                VkCommandBuffer     commandBuffer,
                                const DepthState&   depthState,
                                DepthSnapshotTarget snapshotTarget)
    {
        rememberColorOnlySnapshotScope(pLogicalDevice, commandBuffer, depthState, snapshotTarget);
        if (!hasDepthState(depthState))
            return;

        pLogicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);
        auto& scopeState          = pLogicalDevice->commandBufferDepthStates[commandBuffer];
        scopeState.inRenderScope  = true;
        scopeState.depthState     = depthState;
        scopeState.snapshotTarget = snapshotTarget;
        scopeState.stats          = {};
    }

    // Record a direct draw that contributes `vertices * instances` vertices
    // and one drawcall. Called from CmdDraw / CmdDrawIndexed hooks.
    void countTrackedDepthDraw(LogicalDevice* pLogicalDevice, VkCommandBuffer commandBuffer, uint64_t vertices, uint32_t instances)
    {
        const uint64_t verts   = vertices * (instances == 0 ? 0 : static_cast<uint64_t>(instances));
        auto&          cbStats = pLogicalDevice->commandBufferRecordedDraws[commandBuffer];
        cbStats.vertices += verts;
        cbStats.drawcalls += 1;

        auto scopeIt = pLogicalDevice->commandBufferDepthStates.find(commandBuffer);
        if (scopeIt == pLogicalDevice->commandBufferDepthStates.end() || !scopeIt->second.inRenderScope)
        {
            auto level = pLogicalDevice->commandBufferLevels.find(commandBuffer);
            if (level != pLogicalDevice->commandBufferLevels.end() && level->second == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
                pLogicalDevice->depthFrameTracker.draw(commandBuffer, {}, {verts, 1, 0});
            return;
        }
        scopeIt->second.stats.vertices += verts;
        scopeIt->second.stats.drawcalls += 1;
        pLogicalDevice->depthFrameTracker.draw(commandBuffer, scopeIt->second.depthState, {verts, 1, 0});
    }

    // Record indirect draws whose vertex count is unknown at record time
    // (CmdDrawIndirect / CmdDrawIndexedIndirect / their *Count variants).
    // We count drawcalls_indirect; promotion falls back to this only when
    // vertex counts don't separate candidates.
    void countTrackedDepthDrawIndirect(LogicalDevice* pLogicalDevice, VkCommandBuffer commandBuffer, uint32_t drawCount)
    {
        if (drawCount == 0)
            return;
        auto& cbStats = pLogicalDevice->commandBufferRecordedDraws[commandBuffer];
        cbStats.drawcallsIndirect += drawCount;

        auto scopeIt = pLogicalDevice->commandBufferDepthStates.find(commandBuffer);
        if (scopeIt == pLogicalDevice->commandBufferDepthStates.end() || !scopeIt->second.inRenderScope)
        {
            auto level = pLogicalDevice->commandBufferLevels.find(commandBuffer);
            if (level != pLogicalDevice->commandBufferLevels.end() && level->second == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
                pLogicalDevice->depthFrameTracker.draw(commandBuffer, {}, {0, 0, drawCount});
            return;
        }
        scopeIt->second.stats.drawcallsIndirect += drawCount;
        pLogicalDevice->depthFrameTracker.draw(commandBuffer, scopeIt->second.depthState, {0, 0, drawCount});
    }

    void accumulateExecutedCommandBufferDraws(LogicalDevice*         pLogicalDevice,
                                              VkCommandBuffer        primaryCommandBuffer,
                                              const VkCommandBuffer* pCommandBuffers,
                                              uint32_t               commandBufferCount)
    {
        auto scopeIt = pLogicalDevice->commandBufferDepthStates.find(primaryCommandBuffer);
        if (scopeIt == pLogicalDevice->commandBufferDepthStates.end() || !scopeIt->second.inRenderScope || !pCommandBuffers)
            return;

        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            auto countIt = pLogicalDevice->commandBufferRecordedDraws.find(pCommandBuffers[i]);
            if (countIt == pLogicalDevice->commandBufferRecordedDraws.end())
                continue;
            scopeIt->second.stats.vertices += countIt->second.vertices;
            scopeIt->second.stats.drawcalls += countIt->second.drawcalls;
            scopeIt->second.stats.drawcallsIndirect += countIt->second.drawcallsIndirect;
        }
    }

    static LogicalDevice::DepthCandidateTrackingState& mergeFrameDepthCandidate(LogicalDevice*                                pLogicalDevice,
                                                                                const LogicalDevice::DepthScopeTrackingState& scopeState)
    {
        for (auto& candidate : pLogicalDevice->frameDepthCandidates)
        {
            if (!sameDepthState(candidate.depthState, scopeState.depthState))
                continue;

            candidate.stats.vertices += scopeState.stats.vertices;
            candidate.stats.drawcalls += scopeState.stats.drawcalls;
            candidate.stats.drawcallsIndirect += scopeState.stats.drawcallsIndirect;

            const bool scopeHasPresentableSnapshotTarget   = hasPresentableSnapshotTarget(scopeState.snapshotTarget);
            const bool scopeExtentMatchesPresentableTarget = matchesPresentableSnapshotTargetExtent(scopeState.depthState, scopeState.snapshotTarget);
            if (scopeHasPresentableSnapshotTarget
                && (!candidate.hasPresentableSnapshotTarget || (scopeExtentMatchesPresentableTarget && !candidate.extentMatchesPresentableTarget)))
            {
                candidate.snapshotTarget                 = scopeState.snapshotTarget;
                candidate.hasPresentableSnapshotTarget   = true;
                candidate.extentMatchesPresentableTarget = scopeExtentMatchesPresentableTarget;
            }

            return candidate;
        }

        LogicalDevice::DepthCandidateTrackingState candidate;
        candidate.valid                          = true;
        candidate.depthState                     = scopeState.depthState;
        candidate.snapshotTarget                 = scopeState.snapshotTarget;
        candidate.hasPresentableSnapshotTarget   = hasPresentableSnapshotTarget(scopeState.snapshotTarget);
        candidate.extentMatchesPresentableTarget = matchesPresentableSnapshotTargetExtent(scopeState.depthState, scopeState.snapshotTarget);
        candidate.stats                          = scopeState.stats;
        pLogicalDevice->frameDepthCandidates.push_back(candidate);
        return pLogicalDevice->frameDepthCandidates.back();
    }

    void updateDeviceDepthStateLocked(LogicalDevice* pLogicalDevice, const DepthState& depth, const char* reason, bool presentTimeFallback = false);
    bool endTrackedDepthScope(LogicalDevice* pLogicalDevice, VkCommandBuffer commandBuffer, const char* reason)
    {
        auto scopeIt = pLogicalDevice->commandBufferDepthStates.find(commandBuffer);
        if (scopeIt == pLogicalDevice->commandBufferDepthStates.end())
            return false;

        LogicalDevice::DepthScopeTrackingState scopeState = scopeIt->second;
        pLogicalDevice->commandBufferDepthStates.erase(scopeIt);

        if (!scopeState.inRenderScope || !hasDepthState(scopeState.depthState) || !scopeState.depthState.capturable)
            return false;

        if (isDepthResolveTraceEnabled())
        {
            auto                       levelIt = pLogicalDevice->commandBufferLevels.find(commandBuffer);
            const VkCommandBufferLevel level =
                levelIt != pLogicalDevice->commandBufferLevels.end() ? levelIt->second : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            Logger::debug(std::string("depth resolve trace scope end: reason=") + reason + " cb=" + convertToString(commandBuffer)
                          + " level=" + std::to_string(static_cast<int>(level)) + " image=" + convertToString(scopeState.depthState.image) + " view="
                          + convertToString(scopeState.depthState.imageView) + " extent=" + std::to_string(scopeState.depthState.extent.width) + "x"
                          + std::to_string(scopeState.depthState.extent.height) + " stats.vertices=" + std::to_string(scopeState.stats.vertices)
                          + " stats.drawcalls=" + std::to_string(scopeState.stats.drawcalls)
                          + " stats.indirect=" + std::to_string(scopeState.stats.drawcallsIndirect)
                          + " snapshot.swapchain=" + convertToString(scopeState.snapshotTarget.swapchain)
                          + " snapshot.index=" + std::to_string(scopeState.snapshotTarget.imageIndex) + " activeMatch="
                          + std::string(sameDepthState(pLogicalDevice->activeDepthState, scopeState.depthState) ? "true" : "false"));
        }

        const auto& frameCandidate = mergeFrameDepthCandidate(pLogicalDevice, scopeState);

        LogicalDevice::DepthScopeTrackingState frameScopeState = scopeState;
        frameScopeState.snapshotTarget                         = frameCandidate.snapshotTarget;
        frameScopeState.stats                                  = frameCandidate.stats;

        // ReShade-style prefer_vertices heuristic: pick the scope with the
        // most accumulated vertices for the depth resource (scene meshes
        // dominate over alpha-tested UI/foliage quads), tiebreaking on
        // drawcalls and then indirect drawcalls.
        // Presentable snapshot targets still preempt non-presentable ones.
        bool shouldPromote = !pLogicalDevice->bestDepthCandidate.valid;
        if (!shouldPromote)
        {
            const bool bestHasPresentableSnapshotTarget = pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
            if (frameCandidate.hasPresentableSnapshotTarget != bestHasPresentableSnapshotTarget)
            {
                shouldPromote = frameCandidate.hasPresentableSnapshotTarget;
            }
            else if (frameCandidate.hasPresentableSnapshotTarget
                     && frameCandidate.extentMatchesPresentableTarget != pLogicalDevice->bestDepthCandidate.extentMatchesPresentableTarget)
            {
                shouldPromote = frameCandidate.extentMatchesPresentableTarget;
            }
            else
            {
                shouldPromote = depthScopeBeatsCandidate(frameCandidate.stats, pLogicalDevice->bestDepthCandidate.stats);
            }
        }

        if (!shouldPromote)
            return false;

        pLogicalDevice->bestDepthCandidate.valid                          = true;
        pLogicalDevice->bestDepthCandidate.depthState                     = frameCandidate.depthState;
        pLogicalDevice->bestDepthCandidate.snapshotTarget                 = frameCandidate.snapshotTarget;
        pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget   = frameCandidate.hasPresentableSnapshotTarget;
        pLogicalDevice->bestDepthCandidate.extentMatchesPresentableTarget = frameCandidate.extentMatchesPresentableTarget;
        pLogicalDevice->bestDepthCandidate.stats                          = frameCandidate.stats;

        if (isDepthResolveTraceEnabled())
            Logger::debug(
                std::string("depth resolve trace candidate promote: reason=") + reason + " image=" + convertToString(frameCandidate.depthState.image)
                + " view=" + convertToString(frameCandidate.depthState.imageView) + " frame.vertices=" + std::to_string(frameCandidate.stats.vertices)
                + " frame.drawcalls=" + std::to_string(frameCandidate.stats.drawcalls)
                + " frame.indirect=" + std::to_string(frameCandidate.stats.drawcallsIndirect)
                + " scope.vertices=" + std::to_string(scopeState.stats.vertices) + " scope.drawcalls=" + std::to_string(scopeState.stats.drawcalls)
                + " snapshot.swapchain=" + convertToString(frameCandidate.snapshotTarget.swapchain)
                + " snapshot.index=" + std::to_string(frameCandidate.snapshotTarget.imageIndex));

        if (pLogicalDevice->depthSourceCandidates.empty() && !pLogicalDevice->depthCopyBeforeClears
            && isQualifiedDepthCandidate(pLogicalDevice, frameScopeState) && !hasDepthSourceOverrideCandidate(pLogicalDevice))
        {
            updateDeviceDepthStateLocked(pLogicalDevice, frameCandidate.depthState, reason);
        }
        else if (frameCandidate.stats.vertices > 0 || frameCandidate.stats.drawcalls > 0 || frameCandidate.stats.drawcallsIndirect > 0)
        {
            auto       pendingIt      = pLogicalDevice->pendingTransferLinkedDepthScopes.find(commandBuffer);
            const bool replacePending = pendingIt == pLogicalDevice->pendingTransferLinkedDepthScopes.end()
                                        || depthScopeBeatsCandidate(frameScopeState.stats, pendingIt->second.stats);
            if (replacePending)
            {
                pLogicalDevice->pendingTransferLinkedDepthScopes[commandBuffer] = frameScopeState;
                // [DEBUG-TAG: depth-transfer-probe-20260424]
                static uint32_t pendingProbeCount = 0;
                if (pendingProbeCount++ < 8)
                    Logger::debug(std::string("depth transfer probe pending: reason=") + reason + " cb=" + convertToString(commandBuffer)
                                  + " image=" + convertToString(frameScopeState.depthState.image)
                                  + " view=" + convertToString(frameScopeState.depthState.imageView)
                                  + " extent=" + std::to_string(frameScopeState.depthState.extent.width) + "x"
                                  + std::to_string(frameScopeState.depthState.extent.height)
                                  + " layout=" + convertToString(frameScopeState.depthState.observedLayout) + " vertices="
                                  + std::to_string(frameScopeState.stats.vertices) + " drawcalls=" + std::to_string(frameScopeState.stats.drawcalls)
                                  + " indirect=" + std::to_string(frameScopeState.stats.drawcallsIndirect)
                                  + " snapshot.swapchain=" + convertToString(frameScopeState.snapshotTarget.swapchain)
                                  + " snapshot.index=" + std::to_string(frameScopeState.snapshotTarget.imageIndex));
                if (isDepthResolveTraceEnabled())
                    Logger::debug(std::string("depth resolve trace pending candidate: reason=") + reason + " cb=" + convertToString(commandBuffer)
                                  + " image=" + convertToString(frameScopeState.depthState.image)
                                  + " view=" + convertToString(frameScopeState.depthState.imageView)
                                  + " frame.vertices=" + std::to_string(frameScopeState.stats.vertices)
                                  + " frame.drawcalls=" + std::to_string(frameScopeState.stats.drawcalls)
                                  + " frame.indirect=" + std::to_string(frameScopeState.stats.drawcallsIndirect));
            }
            else if (isDepthResolveTraceEnabled())
            {
                Logger::debug(std::string("depth resolve trace pending candidate kept: reason=") + reason + " cb=" + convertToString(commandBuffer)
                              + " keptImage=" + convertToString(pendingIt->second.depthState.image)
                              + " kept.vertices=" + std::to_string(pendingIt->second.stats.vertices)
                              + " candidateImage=" + convertToString(frameScopeState.depthState.image)
                              + " candidate.vertices=" + std::to_string(frameScopeState.stats.vertices));
            }
        }
        return true;
    }

    void tryActivatePendingTransferLinkedDepthScope(
        LogicalDevice* pLogicalDevice, VkCommandBuffer commandBuffer, VkImage sourceImage, VkImage destinationImage, const char* reason)
    {
        if (!pLogicalDevice || destinationImage == VK_NULL_HANDLE)
            return;

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImage(pLogicalDevice, destinationImage);
        if (!hasPresentableSnapshotTarget(snapshotTarget))
            return;

        auto pendingIt = pLogicalDevice->pendingTransferLinkedDepthScopes.find(commandBuffer);
        if (pendingIt == pLogicalDevice->pendingTransferLinkedDepthScopes.end())
            return;

        auto scopeState           = pendingIt->second;
        scopeState.snapshotTarget = snapshotTarget;
        pLogicalDevice->pendingTransferLinkedDepthScopes.erase(pendingIt);

        pLogicalDevice->bestDepthCandidate.valid                        = true;
        pLogicalDevice->bestDepthCandidate.depthState                   = scopeState.depthState;
        pLogicalDevice->bestDepthCandidate.snapshotTarget               = snapshotTarget;
        pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget = true;
        pLogicalDevice->bestDepthCandidate.extentMatchesPresentableTarget =
            matchesPresentableSnapshotTargetExtent(scopeState.depthState, snapshotTarget);
        pLogicalDevice->bestDepthCandidate.stats = scopeState.stats;

        if (isDepthResolveTraceEnabled())
            Logger::debug(std::string("depth resolve trace transfer activate: reason=") + reason + " cb=" + convertToString(commandBuffer)
                          + " srcImage=" + convertToString(sourceImage) + " dstImage=" + convertToString(destinationImage) + " depthImage="
                          + convertToString(scopeState.depthState.image) + " depthView=" + convertToString(scopeState.depthState.imageView)
                          + " vertices=" + std::to_string(scopeState.stats.vertices) + " drawcalls=" + std::to_string(scopeState.stats.drawcalls)
                          + " snapshot.swapchain=" + convertToString(snapshotTarget.swapchain)
                          + " snapshot.index=" + std::to_string(snapshotTarget.imageIndex));
        // [DEBUG-TAG: depth-transfer-probe-20260424]
        static uint32_t activationProbeCount = 0;
        if (activationProbeCount++ < 8)
            Logger::debug(std::string("depth transfer probe activate: reason=") + reason + " cb=" + convertToString(commandBuffer)
                          + " srcImage=" + convertToString(sourceImage) + " dstImage=" + convertToString(destinationImage) + " depthImage="
                          + convertToString(scopeState.depthState.image) + " depthView=" + convertToString(scopeState.depthState.imageView)
                          + " extent=" + std::to_string(scopeState.depthState.extent.width) + "x"
                          + std::to_string(scopeState.depthState.extent.height) + " layout=" + convertToString(scopeState.depthState.observedLayout)
                          + " vertices=" + std::to_string(scopeState.stats.vertices) + " drawcalls=" + std::to_string(scopeState.stats.drawcalls)
                          + " indirect=" + std::to_string(scopeState.stats.drawcallsIndirect) + " snapshot.swapchain="
                          + convertToString(snapshotTarget.swapchain) + " snapshot.index=" + std::to_string(snapshotTarget.imageIndex));

        if (pLogicalDevice->depthSourceCandidates.empty() && !pLogicalDevice->depthCopyBeforeClears
            && !hasDepthSourceOverrideCandidate(pLogicalDevice))
            updateDeviceDepthStateLocked(pLogicalDevice, scopeState.depthState, reason);
    }

    void tryActivatePendingDepthScopeForSnapshotTarget(LogicalDevice*             pLogicalDevice,
                                                       VkCommandBuffer            commandBuffer,
                                                       const DepthSnapshotTarget& snapshotTarget,
                                                       const char*                reason)
    {
        if (!pLogicalDevice || snapshotTarget.swapchain == VK_NULL_HANDLE)
            return;

        scoped_lock l(globalLock);
        const auto  swapchain = swapchainMap.find(snapshotTarget.swapchain);
        if (swapchain == swapchainMap.end() || !swapchain->second || swapchain->second.get() != snapshotTarget.pLogicalSwapchain
            || swapchain->second->pLogicalDevice != pLogicalDevice || snapshotTarget.imageIndex >= swapchain->second->imageCount)
            return;

        auto pendingIt = pLogicalDevice->pendingTransferLinkedDepthScopes.find(commandBuffer);
        if (pendingIt == pLogicalDevice->pendingTransferLinkedDepthScopes.end())
            return;

        auto scopeState           = pendingIt->second;
        scopeState.snapshotTarget = snapshotTarget;
        pLogicalDevice->pendingTransferLinkedDepthScopes.erase(pendingIt);

        pLogicalDevice->bestDepthCandidate.valid                        = true;
        pLogicalDevice->bestDepthCandidate.depthState                   = scopeState.depthState;
        pLogicalDevice->bestDepthCandidate.snapshotTarget               = snapshotTarget;
        pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget = true;
        pLogicalDevice->bestDepthCandidate.extentMatchesPresentableTarget =
            matchesPresentableSnapshotTargetExtent(scopeState.depthState, snapshotTarget);
        pLogicalDevice->bestDepthCandidate.stats = scopeState.stats;

        // [DEBUG-TAG: depth-transfer-probe-20260424]
        static uint32_t colorScopeActivationProbeCount = 0;
        if (colorScopeActivationProbeCount++ < 8)
            Logger::debug(std::string("depth transfer probe color-scope activate: reason=") + reason + " cb=" + convertToString(commandBuffer)
                          + " depthImage=" + convertToString(scopeState.depthState.image) + " depthView="
                          + convertToString(scopeState.depthState.imageView) + " extent=" + std::to_string(scopeState.depthState.extent.width) + "x"
                          + std::to_string(scopeState.depthState.extent.height) + " layout=" + convertToString(scopeState.depthState.observedLayout)
                          + " vertices=" + std::to_string(scopeState.stats.vertices) + " drawcalls=" + std::to_string(scopeState.stats.drawcalls)
                          + " indirect=" + std::to_string(scopeState.stats.drawcallsIndirect) + " snapshot.swapchain="
                          + convertToString(snapshotTarget.swapchain) + " snapshot.index=" + std::to_string(snapshotTarget.imageIndex));

        if (pLogicalDevice->depthSourceCandidates.empty() && !pLogicalDevice->depthCopyBeforeClears
            && !hasDepthSourceOverrideCandidate(pLogicalDevice))
            updateDeviceDepthStateLocked(pLogicalDevice, scopeState.depthState, reason);
    }

    VkImageView getOrCreateTrackedDepthSampleViewLocked(LogicalDevice* pLogicalDevice, VkImage image, VkFormat format)
    {
        auto imageIt = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), image);
        if (imageIt == pLogicalDevice->depthImages.end())
            return VK_NULL_HANDLE;

        const size_t index = std::distance(pLogicalDevice->depthImages.begin(), imageIt);
        if (index >= pLogicalDevice->depthImageViews.size())
            pLogicalDevice->depthImageViews.resize(index + 1, VK_NULL_HANDLE);

        VkImageView& trackedView = pLogicalDevice->depthImageViews[index];
        if (trackedView == VK_NULL_HANDLE)
            trackedView = createImageViews(pLogicalDevice, format, {image}, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT)[0];

        return trackedView;
    }

    void updateDeviceDepthStateLocked(LogicalDevice* pLogicalDevice, const DepthState& depth, const char* reason, bool presentTimeFallback);
    void reallocateCommandBuffers(LogicalDevice* pLogicalDevice, LogicalSwapchain* pLogicalSwapchain, const DepthState& depth);

    void initializeDepthResolveLayout(LogicalSwapchain* pLogicalSwapchain, const DepthState& depth)
    {
        LogicalDevice* pLogicalDevice = pLogicalSwapchain->pLogicalDevice;
        auto           metadataIt     = pLogicalDevice->depthImageMetadata.find(depth.image);
        const bool     sourceSupportsTransfer =
            metadataIt != pLogicalDevice->depthImageMetadata.end() && (metadataIt->second.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        const bool useShaderResolve                    = depth.observedLayout == VK_IMAGE_LAYOUT_GENERAL && !sourceSupportsTransfer;
        pLogicalSwapchain->depthResolveUsesShader      = useShaderResolve;
        pLogicalSwapchain->depthResolveSourceImage     = depth.image;
        pLogicalSwapchain->depthResolveSourceImageView = depth.imageView;
        pLogicalSwapchain->depthResolveSourceId        = depth.sourceId;
        pLogicalSwapchain->depthResolveFormat          = useShaderResolve ? VK_FORMAT_R32_SFLOAT : depth.format;
        pLogicalSwapchain->depthResolveExtent          = {depth.extent.width, depth.extent.height, 1};

        pLogicalSwapchain->depthResolveImages.clear();
        pLogicalSwapchain->depthResolveImageViews.clear();
        pLogicalSwapchain->depthResolveMemories.clear();
        pLogicalSwapchain->depthResolveInitialized.assign(pLogicalSwapchain->imageCount, false);
        pLogicalSwapchain->depthResolveVersions.assign(pLogicalSwapchain->imageCount, 0);
        pLogicalSwapchain->depthResolveLatest.reset();
        pLogicalSwapchain->depthResolveVersion = 0;
        pLogicalSwapchain->depthResolveImages.reserve(pLogicalSwapchain->imageCount);
        pLogicalSwapchain->depthResolveMemories.reserve(pLogicalSwapchain->imageCount);
        for (uint32_t i = 0; i < pLogicalSwapchain->imageCount; ++i)
        {
            VkDeviceMemory       imageMemory = VK_NULL_HANDLE;
            std::vector<VkImage> images =
                createImages(pLogicalDevice,
                             1,
                             pLogicalSwapchain->depthResolveExtent,
                             pLogicalSwapchain->depthResolveFormat,
                             useShaderResolve ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                                 | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                                              : (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             imageMemory);
            pLogicalSwapchain->depthResolveImages.push_back(images[0]);
            pLogicalSwapchain->depthResolveMemories.push_back(imageMemory);
        }

        pLogicalSwapchain->depthResolveImageViews = createImageViews(pLogicalDevice,
                                                                     pLogicalSwapchain->depthResolveFormat,
                                                                     pLogicalSwapchain->depthResolveImages,
                                                                     VK_IMAGE_VIEW_TYPE_2D,
                                                                     useShaderResolve ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT);

        if (!useShaderResolve)
            return;

        pLogicalSwapchain->depthResolveSampler             = createDepthSampler(pLogicalDevice);
        pLogicalSwapchain->depthResolveDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 1);

        VkDescriptorPoolSize imagePoolSize            = {};
        imagePoolSize.type                            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount                 = pLogicalSwapchain->imageCount;
        pLogicalSwapchain->depthResolveDescriptorPool = createDescriptorPool(pLogicalDevice, {imagePoolSize});

        std::vector<VkImageView> sourceViews(pLogicalSwapchain->imageCount, depth.imageView);
        const VkImageLayout      sourceImageLayout =
            depth.observedLayout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pLogicalSwapchain->depthResolveDescriptorSets =
            allocateAndWriteImageSamplerDescriptorSets(pLogicalDevice,
                                                       pLogicalSwapchain->depthResolveDescriptorPool,
                                                       pLogicalSwapchain->depthResolveDescriptorSetLayout,
                                                       {pLogicalSwapchain->depthResolveSampler},
                                                       {sourceViews},
                                                       std::vector<VkDescriptorType>{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                                                       {sourceImageLayout});

        pLogicalSwapchain->depthResolveRenderPass =
            createRenderPass(pLogicalDevice, pLogicalSwapchain->depthResolveFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pLogicalSwapchain->depthResolvePipelineLayout =
            createGraphicsPipelineLayout(pLogicalDevice, {pLogicalSwapchain->depthResolveDescriptorSetLayout});

        VkShaderModule vertexModule   = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        createShaderModule(pLogicalDevice, full_screen_triangle_vert, &vertexModule);
        createShaderModule(pLogicalDevice, depth_resolve_frag, &fragmentModule);

        VkExtent2D resolveExtent2D              = {pLogicalSwapchain->depthResolveExtent.width, pLogicalSwapchain->depthResolveExtent.height};
        pLogicalSwapchain->depthResolvePipeline = createGraphicsPipeline(pLogicalDevice,
                                                                         vertexModule,
                                                                         nullptr,
                                                                         "main",
                                                                         fragmentModule,
                                                                         nullptr,
                                                                         "main",
                                                                         resolveExtent2D,
                                                                         pLogicalSwapchain->depthResolveRenderPass,
                                                                         pLogicalSwapchain->depthResolvePipelineLayout);
        pLogicalDevice->vkd.DestroyShaderModule(pLogicalDevice->device, fragmentModule, nullptr);
        pLogicalDevice->vkd.DestroyShaderModule(pLogicalDevice->device, vertexModule, nullptr);

        pLogicalSwapchain->depthResolveFramebuffers = createFramebuffers(
            pLogicalDevice, pLogicalSwapchain->depthResolveRenderPass, resolveExtent2D, {pLogicalSwapchain->depthResolveImageViews});
    }

    static bool depthResolveResourcesMatch(LogicalSwapchain* pLogicalSwapchain, const DepthState& depth)
    {
        LogicalDevice* pLogicalDevice = pLogicalSwapchain->pLogicalDevice;
        if (!hasDepthState(depth) || depth.extent.width == 0 || depth.extent.height == 0)
            return pLogicalSwapchain->depthResolveFormat == VK_FORMAT_UNDEFINED;
        auto       metadataIt = pLogicalDevice->depthImageMetadata.find(depth.image);
        const bool sourceSupportsTransfer =
            metadataIt != pLogicalDevice->depthImageMetadata.end() && (metadataIt->second.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        const bool     useShaderResolve    = depth.observedLayout == VK_IMAGE_LAYOUT_GENERAL && !sourceSupportsTransfer;
        const VkFormat wantedResolveFormat = useShaderResolve ? VK_FORMAT_R32_SFLOAT : depth.format;
        return pLogicalSwapchain->depthResolveFormat == wantedResolveFormat && pLogicalSwapchain->depthResolveExtent.width == depth.extent.width
               && pLogicalSwapchain->depthResolveExtent.height == depth.extent.height && pLogicalSwapchain->depthResolveExtent.depth == 1
               && pLogicalSwapchain->depthResolveUsesShader == useShaderResolve
               && (!useShaderResolve
                   || (pLogicalSwapchain->depthResolveSourceImage == depth.image && pLogicalSwapchain->depthResolveSourceImageView == depth.imageView
                       && pLogicalSwapchain->depthResolveSourceId == depth.sourceId))
               && pLogicalSwapchain->depthResolveImages.size() == pLogicalSwapchain->imageCount
               && pLogicalSwapchain->depthResolveImageViews.size() == pLogicalSwapchain->imageCount
               && pLogicalSwapchain->depthResolveMemories.size() == pLogicalSwapchain->imageCount
               && pLogicalSwapchain->depthResolveInitialized.size() == pLogicalSwapchain->imageCount
               && pLogicalSwapchain->depthResolveVersions.size() == pLogicalSwapchain->imageCount
               && (!useShaderResolve
                   || (pLogicalSwapchain->depthResolveDescriptorSets.size() == pLogicalSwapchain->imageCount
                       && pLogicalSwapchain->depthResolveRenderPass != VK_NULL_HANDLE
                       && pLogicalSwapchain->depthResolvePipelineLayout != VK_NULL_HANDLE && pLogicalSwapchain->depthResolvePipeline != VK_NULL_HANDLE
                       && pLogicalSwapchain->depthResolveFramebuffers.size() == pLogicalSwapchain->imageCount));
    }

    void ensureDepthResolveResources(LogicalSwapchain* pLogicalSwapchain, const DepthState& depth)
    {
        if (depthResolveResourcesMatch(pLogicalSwapchain, depth))
            return;

        destroyDepthResolveResources(pLogicalSwapchain);
        if (hasDepthState(depth) && depth.extent.width != 0 && depth.extent.height != 0)
            initializeDepthResolveLayout(pLogicalSwapchain, depth);
    }

    // Get depth state from logical device (returns null handles if no depth images)
    DepthState getDepthState(LogicalDevice* pLogicalDevice)
    {
        return pLogicalDevice->activeDepthState;
    }

    void updateDeviceDepthStateLocked(LogicalDevice* pLogicalDevice, const DepthState& depth, const char* reason, bool presentTimeFallback)
    {
        if (sameDepthState(pLogicalDevice->activeDepthState, depth) && pLogicalDevice->activeDepthStatePresentTimeFallback == presentTimeFallback)
            return;

        const bool sourceChanged =
            pLogicalDevice->activeDepthState.image != depth.image || pLogicalDevice->activeDepthState.sourceId != depth.sourceId;
        if (sourceChanged)
            for (auto& [_, swapchain] : swapchainMap)
                if (swapchain && swapchain->pLogicalDevice == pLogicalDevice)
                    invalidateDepthResolveOutput(swapchain.get());
        pLogicalDevice->activeDepthState                    = depth;
        pLogicalDevice->activeDepthStatePresentTimeFallback = presentTimeFallback;
        Logger::debug(std::string("active depth state updated from ") + reason + ": image=" + convertToString(depth.image)
                      + " view=" + convertToString(depth.imageView) + " format=" + convertToString(depth.format)
                      + " extent=" + std::to_string(depth.extent.width) + "x" + std::to_string(depth.extent.height) + " observedLayout="
                      + convertToString(depth.observedLayout) + " presentTimeFallback=" + std::string(presentTimeFallback ? "true" : "false"));

        auto metadataIt = pLogicalDevice->depthImageMetadata.find(depth.image);
        if (metadataIt != pLogicalDevice->depthImageMetadata.end())
        {
            Logger::debug(std::string("active depth state metadata from ") + reason + ": image=" + convertToString(depth.image) + " usage=0x"
                          + formatHexU64(static_cast<uint64_t>(metadataIt->second.usage)) + " samples=" + convertToString(metadataIt->second.samples)
                          + " tiling=" + convertToString(metadataIt->second.tiling) + " transient="
                          + std::string((metadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));
        }
    }

    static VkResult refreshDepthResourcesForPresent(LogicalDevice* device, LogicalSwapchain* swapchain, const DepthState& depth, bool& ready)
    {
        ready = depthResolveResourcesMatch(swapchain, depth);
        if (ready)
            return VK_SUCCESS;
        bool       idle   = false;
        const auto result = depthResourcesIdle(device, swapchain, idle);
        if (result != VK_SUCCESS || !idle)
            return result;
        if (!swapchain->commandBuffersEffect.empty())
            device->vkd.FreeCommandBuffers(
                device->device, device->commandPool, swapchain->commandBuffersEffect.size(), swapchain->commandBuffersEffect.data());
        ensureDepthResolveResources(swapchain, depth);
        swapchain->commandBuffersEffect = allocateCommandBuffer(device, swapchain->imageCount);
        writeCommandBuffers(device, swapchain, swapchain->effects, swapchain->commandBuffersEffect);
        ready = true;
        return VK_SUCCESS;
    }

    // Helper to reallocate and rewrite command buffers for a swapchain
    void reallocateCommandBuffers(LogicalDevice* pLogicalDevice, LogicalSwapchain* pLogicalSwapchain, const DepthState& depth)
    {
        if (pLogicalDevice->queue != VK_NULL_HANDLE)
            pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);

        // Free existing command buffers
        if (!pLogicalSwapchain->commandBuffersEffect.empty())
        {
            pLogicalDevice->vkd.FreeCommandBuffers(pLogicalDevice->device,
                                                   pLogicalDevice->commandPool,
                                                   pLogicalSwapchain->commandBuffersEffect.size(),
                                                   pLogicalSwapchain->commandBuffersEffect.data());
        }
        if (!pLogicalSwapchain->commandBuffersNoEffect.empty())
        {
            pLogicalDevice->vkd.FreeCommandBuffers(pLogicalDevice->device,
                                                   pLogicalDevice->commandPool,
                                                   pLogicalSwapchain->commandBuffersNoEffect.size(),
                                                   pLogicalSwapchain->commandBuffersNoEffect.data());
        }

        ensureDepthResolveResources(pLogicalSwapchain, depth);

        // Allocate and write effect command buffers
        pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
        writeCommandBuffers(pLogicalDevice, pLogicalSwapchain, pLogicalSwapchain->effects, pLogicalSwapchain->commandBuffersEffect);

        // Allocate and write no-effect command buffers
        pLogicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
        writeCommandBuffers(pLogicalDevice, pLogicalSwapchain, {pLogicalSwapchain->defaultTransfer}, pLogicalSwapchain->commandBuffersNoEffect);
    }

    // Apply modified parameters from overlay to config
    void applyOverlayParams(LogicalDevice* pLogicalDevice)
    {
        // Parameters are already in EffectRegistry (the single source of truth)
        // Effects read directly from the registry when recreated
        // This function just logs for debugging
        if (!pLogicalDevice->imguiOverlay)
            return;

        Logger::info("Applying parameters from overlay - effects will read from EffectRegistry");
    }

    // Detected game info (set once at init, used by overlay for profiles)
    static std::string detectedGameName;
    static std::string activeProfileName;
    static std::string activeProfilePath;

    // Initialize configs: base (vkShade.conf) + current (from game profile / env / default)
    void initConfigs()
    {
        if (pBaseConfig != nullptr)
            return; // Already initialized

        // Ensure config directory exists for later saves
        {
            std::string baseDir = ConfigSerializer::getBaseConfigDir();
            if (!baseDir.empty())
                mkdir(baseDir.c_str(), 0755);
        }

        // Initialize settings manager (single source of truth for settings)
        settingsManager.initialize();
        // [DEBUG-TAG: depth-transfer-probe-20260424]
        Logger::debug(std::string("depth transfer probe env: trace=")
                      + (std::getenv("VKSHADE_DEBUG_TRACE_DEPTH_RESOLVE") ? std::getenv("VKSHADE_DEBUG_TRACE_DEPTH_RESOLVE") : "<unset>") + " dump="
                      + (std::getenv("VKSHADE_DEBUG_DUMP_DEPTH_COPY") ? std::getenv("VKSHADE_DEBUG_DUMP_DEPTH_COPY") : "<unset>") + " uniformLog="
                      + (std::getenv("VKSHADE_DEBUG_LOG_RESHADE_UI_UNIFORMS") ? std::getenv("VKSHADE_DEBUG_LOG_RESHADE_UI_UNIFORMS") : "<unset>")
                      + " logLevel=" + (std::getenv("VKSHADE_LOG_LEVEL") ? std::getenv("VKSHADE_LOG_LEVEL") : "<unset>"));

        // Load base config (vkShade.conf) - used for paths, effect definitions
        pBaseConfig = std::make_shared<Config>();

        // Detect the game executable
        detectedGameName = ConfigSerializer::detectGameName();

        // Determine current config path (priority order):
        // 1. VKSHADE_CONFIG_FILE env var (explicit override)
        // 2. Per-game profile (auto-created if needed)
        // 3. Legacy default_config file
        // 4. Base vkShade.conf
        std::string currentConfigPath;

        const char* envConfig = std::getenv("VKSHADE_CONFIG_FILE");
        if (envConfig && *envConfig)
        {
            currentConfigPath = envConfig;
            Logger::info("config from env: " + currentConfigPath);
        }
        else if (!detectedGameName.empty())
        {
            ConfigSerializer::migrateGameProfiles(detectedGameName, ConfigSerializer::detectExecutableName());
            // Auto-create profile for this game if needed, then load it
            activeProfileName = ConfigSerializer::getActiveProfile(detectedGameName);
            activeProfilePath = ConfigSerializer::getProfilePath(detectedGameName, activeProfileName);

            // Ensure the profile file exists
            struct stat st;
            if (stat(activeProfilePath.c_str(), &st) != 0)
            {
                // Profile doesn't exist yet — create it
                activeProfilePath = ConfigSerializer::ensureGameProfile(detectedGameName);
                if (!activeProfilePath.empty() && activeProfileName != "default")
                {
                    activeProfileName = "default";
                    ConfigSerializer::setActiveProfile(detectedGameName, activeProfileName);
                }
            }

            if (!activeProfilePath.empty())
            {
                currentConfigPath = activeProfilePath;
                Logger::info("game: " + detectedGameName + " | profile: " + activeProfileName);
            }
        }

        // Fallback: legacy default_config
        if (currentConfigPath.empty())
        {
            std::string defaultName = ConfigSerializer::getDefaultConfig();
            if (!defaultName.empty())
                currentConfigPath = ConfigSerializer::getConfigsDir() + "/" + defaultName + ".conf";
        }

        // Load current config if specified, otherwise use base
        if (!currentConfigPath.empty())
        {
            std::ifstream file(currentConfigPath);
            if (file.good())
            {
                pConfig = std::make_shared<Config>(currentConfigPath);
                pConfig->setFallback(pBaseConfig.get());
                Logger::info("current config: " + currentConfigPath);
            }
            else
            {
                pConfig = pBaseConfig;
            }
        }
        else
        {
            pConfig = pBaseConfig;
        }
        pConfig->setGameName(detectedGameName);

        // Enforce per-profile safe anti-cheat: override global depthCapture + hide layer
        if (!activeProfilePath.empty())
        {
            ProfileSettings ps = ConfigSerializer::loadProfileSettings(activeProfilePath);
            if (ps.safeAntiCheat)
            {
                settingsManager.setSafeAntiCheat(true);
                settingsManager.setDepthCapture(false);
                Logger::info("safeAntiCheat enabled — depth capture forced off, layer hidden");
            }
        }

        // Initialize effect registry with current config
        effectRegistry.initialize(pConfig.get());
    }

    // Switch to a new config (called from overlay)
    void switchConfig(const std::string& configPath)
    {
        Logger::info("switching to config: " + configPath);

        // Create new config from file (starts with no overrides)
        pConfig = std::make_shared<Config>(configPath);
        pConfig->setFallback(pBaseConfig.get());
        pConfig->setGameName(detectedGameName);

        // Also clear any overrides on the base config to avoid stale values
        if (pBaseConfig)
            pBaseConfig->clearOverrides();

        // Re-initialize registry with new config
        effectRegistry.initialize(pConfig.get());
        cachedParams.dirty = true;

        Logger::info("switched to config: " + configPath);
    }

    // Helper function to get available effects separated by source (uses cache)
    void getAvailableEffects(Config*                             pConfig,
                             std::vector<std::string>&           currentConfigEffects,
                             std::vector<std::string>&           defaultConfigEffects,
                             std::map<std::string, std::string>& effectPaths,
                             std::map<std::string, std::string>& configuredEffectPaths,
                             std::vector<std::string>&           effectFiles,
                             std::vector<std::string>&           shaderIncludePaths)
    {
        // Use cache if available and config hasn't changed
        if (cachedEffects.initialized && cachedEffects.configPath == pConfig->getConfigFilePath())
        {
            currentConfigEffects  = cachedEffects.currentConfigEffects;
            defaultConfigEffects  = cachedEffects.defaultConfigEffects;
            effectPaths           = cachedEffects.effectPaths;
            configuredEffectPaths = cachedEffects.configuredEffectPaths;
            effectFiles           = cachedEffects.effectFiles;
            shaderIncludePaths    = cachedEffects.shaderIncludePaths;
            return;
        }

        currentConfigEffects.clear();
        defaultConfigEffects.clear();
        effectPaths.clear();
        configuredEffectPaths.clear();
        effectFiles.clear();
        shaderIncludePaths.clear();

        std::set<std::string> knownFiles;
        auto                  addFile = [&](const std::filesystem::path& path) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                return;
            auto canonical = std::filesystem::canonical(path, ec);
            if (!ec && knownFiles.insert(canonical.string()).second)
                effectFiles.push_back(canonical.string());
        };

        // Collect all known effect names (to avoid duplicates)
        std::set<std::string> knownEffects;

        // Get effect definitions from current config
        auto configEffects = pConfig->getEffectDefinitions();
        for (const auto& [name, path] : configEffects)
        {
            currentConfigEffects.push_back(name);
            effectPaths[name]           = path;
            configuredEffectPaths[name] = path;
            addFile(path);
            knownEffects.insert(name);
        }

        // Also load effect definitions from the base config file (vkShade.conf)
        if (pBaseConfig && pBaseConfig->getConfigFilePath() != pConfig->getConfigFilePath())
        {
            auto defaultEffects = pBaseConfig->getEffectDefinitions();
            for (const auto& [name, path] : defaultEffects)
            {
                if (knownEffects.find(name) == knownEffects.end())
                {
                    defaultConfigEffects.push_back(name);
                    effectPaths[name]           = path;
                    configuredEffectPaths[name] = path;
                    addFile(path);
                    knownEffects.insert(name);
                }
            }
        }

        // Auto-discover .fx files in all shader manager discovered paths
        ShaderManagerConfig      shaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();
        std::vector<std::string> scanPaths;
        std::stringstream        localPaths(pConfig->getOption<std::string>("reshadeIncludePath", ""));
        for (std::string path; std::getline(localPaths, path, ':');)
            if (!path.empty())
                scanPaths.push_back(path);
        scanPaths.insert(scanPaths.end(), shaderMgrConfig.discoveredShaderPaths.begin(), shaderMgrConfig.discoveredShaderPaths.end());
        shaderIncludePaths = scanPaths;
        for (const auto& shaderPath : scanPaths)
        {
            try
            {
                for (const auto& entry :
                     std::filesystem::recursive_directory_iterator(shaderPath, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file())
                        continue;

                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".fx")
                        continue;

                    addFile(entry.path());

                    // Effect name is filename without .fx extension
                    std::string effectName = entry.path().stem().string();

                    // Skip if already known (from config definitions or other paths)
                    if (knownEffects.find(effectName) != knownEffects.end())
                        continue;

                    defaultConfigEffects.push_back(effectName);
                    effectPaths[effectName] = entry.path().string();
                    knownEffects.insert(effectName);
                }
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                Logger::warn("failed to scan shader path " + shaderPath + ": " + std::string(e.what()));
            }
        }

        // Sort discovered effects alphabetically
        std::sort(defaultConfigEffects.begin(), defaultConfigEffects.end());
        std::sort(effectFiles.begin(), effectFiles.end());

        // Update cache
        cachedEffects.currentConfigEffects  = currentConfigEffects;
        cachedEffects.defaultConfigEffects  = defaultConfigEffects;
        cachedEffects.effectPaths           = effectPaths;
        cachedEffects.configuredEffectPaths = configuredEffectPaths;
        cachedEffects.effectFiles           = effectFiles;
        cachedEffects.shaderIncludePaths    = shaderIncludePaths;
        cachedEffects.configPath            = pConfig->getConfigFilePath();
        cachedEffects.initialized           = true;
    }

    static std::shared_ptr<Effect> createForwardingEffect(LogicalSwapchain*           pLogicalSwapchain,
                                                          LogicalDevice*              pLogicalDevice,
                                                          const std::vector<VkImage>& inputImages,
                                                          const std::vector<VkImage>& outputImages,
                                                          Config*                     pConfig,
                                                          bool                        outputIsSwapchain)
    {
        if (outputIsSwapchain && !pLogicalSwapchain->useTransferToSwapchain)
            return std::make_shared<PresentCopyEffect>(
                pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent, inputImages, outputImages, pConfig);
        return std::make_shared<TransferEffect>(
            pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent, inputImages, outputImages, pConfig);
    }

    // Helper function to create effects for a swapchain
    // This centralizes the effect creation logic used by both initial swapchain setup and hot-reload
    void createEffectsForSwapchain(LogicalSwapchain*                  pLogicalSwapchain,
                                   LogicalDevice*                     pLogicalDevice,
                                   Config*                            pConfig,
                                   const std::vector<TechniqueStage>& stages,
                                   bool                               checkEnabledState = true)
    {
        const bool directOutput = canUseDirectEffectOutput(pLogicalSwapchain->useMutableFormat, pLogicalSwapchain->useTransferToSwapchain);
        std::vector<std::string> effectStrings;
        effectStrings.reserve(stages.size());
        for (const auto& stage : stages)
            effectStrings.push_back(stage.effectName);
        std::unordered_map<std::string, std::shared_ptr<ReshadeEffect>> sharedEffects;

        if (pLogicalSwapchain->imageCount == 0)
        {
            Logger::err("Cannot create effects for swapchain with imageCount=0");
            return;
        }

        const size_t requiredSlots      = effectStrings.empty() ? 1 : effectStrings.size() + (directOutput ? 0u : 1u);
        const size_t requiredFakeImages = static_cast<size_t>(pLogicalSwapchain->imageCount) * requiredSlots;
        if (pLogicalSwapchain->fakeImages.size() < requiredFakeImages)
        {
            Logger::err("Insufficient fake images for effect chain: have " + std::to_string(pLogicalSwapchain->fakeImages.size()) + ", need "
                        + std::to_string(requiredFakeImages));
            return;
        }

        // If no effects, add pass-through so rendering still works
        if (effectStrings.empty())
        {
            std::vector<VkImage> firstImages(pLogicalSwapchain->fakeImages.begin(),
                                             pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount);
            pLogicalSwapchain->effects.push_back(
                createForwardingEffect(pLogicalSwapchain, pLogicalDevice, firstImages, pLogicalSwapchain->images, pConfig, true));
            return;
        }

        for (uint32_t i = 0; i < effectStrings.size(); i++)
        {
            Logger::debug("creating effect " + std::to_string(i) + ": " + effectStrings[i]);

            // Calculate input images for this effect
            std::vector<VkImage> firstImages(pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * i,
                                             pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 1));

            // Calculate output images - last effect writes to swapchain or final fake images
            std::vector<VkImage> secondImages;
            if (i == effectStrings.size() - 1)
            {
                secondImages = directOutput ? pLogicalSwapchain->images
                                            : std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount,
                                                                   pLogicalSwapchain->fakeImages.end());
            }
            else
            {
                secondImages = std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 1),
                                                    pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 2));
            }

            const bool outputIsSwapchain = directOutput && i == effectStrings.size() - 1;

            // Check if effect should be skipped (disabled or failed)
            bool effectFailed   = effectRegistry.hasEffectFailed(effectStrings[i]);
            bool effectDisabled = checkEnabledState && !effectRegistry.isEffectEnabled(effectStrings[i]);

            if (effectFailed || effectDisabled)
            {
                Logger::debug("effect " + std::string(effectFailed ? "failed" : "disabled") + ", using pass-through: " + effectStrings[i]);
                pLogicalSwapchain->effects.push_back(
                    createForwardingEffect(pLogicalSwapchain, pLogicalDevice, firstImages, secondImages, pConfig, outputIsSwapchain));
                continue;
            }

            {
                // ReShade effect - wrap in try-catch + signal handler to handle compilation failures gracefully
                // The embedded reshadefx compiler can trigger SIGFPE/SIGABRT in edge cases
                std::string effectPath = effectRegistry.getEffectFilePath(effectStrings[i]);
                auto        customDefs = effectRegistry.getPreprocessorDefs(effectStrings[i]);

                installCrashHandlers();
                bool signalCrash   = false;
                auto setStageError = [&](const std::string& error) {
                    if (stages[i].technique)
                        effectRegistry.setTechniqueError(effectStrings[i], *stages[i].technique, error);
                    else if (!error.empty())
                        effectRegistry.setEffectError(effectStrings[i], error);
                };
                if (sigsetjmp(signalJmpBuf, 1) != 0)
                {
                    // Returned here from signal handler (SIGFPE/SIGABRT)
                    signalJmpActive     = 0;
                    signalCrash         = true;
                    std::string sigName = (caughtSignal == SIGFPE) ? "SIGFPE" : "SIGABRT";
                    Logger::err("Caught " + sigName + " creating ReshadeEffect " + effectStrings[i]);
                    setStageError(sigName + " during shader compilation");
                    pLogicalSwapchain->effects.push_back(
                        createForwardingEffect(pLogicalSwapchain, pLogicalDevice, firstImages, secondImages, pConfig, outputIsSwapchain));
                }

                if (!signalCrash)
                {
                    signalJmpActive = 1;
                    try
                    {
                        std::shared_ptr<ReshadeEffect> sharedOwner;
                        if (stages[i].technique)
                        {
                            auto it = sharedEffects.find(effectStrings[i]);
                            if (it != sharedEffects.end())
                                sharedOwner = it->second;
                        }
                        auto reshadeEffect = std::make_shared<ReshadeEffect>(pLogicalDevice,
                                                                             pLogicalSwapchain->format,
                                                                             pLogicalSwapchain->imageExtent,
                                                                             firstImages,
                                                                             secondImages,
                                                                             &effectRegistry,
                                                                             effectStrings[i],
                                                                             effectPath,
                                                                             customDefs,
                                                                             stages[i].technique,
                                                                             sharedOwner);
                        if (stages[i].technique && !sharedOwner)
                            sharedEffects.emplace(effectStrings[i], reshadeEffect);
                        setStageError("");
                        pLogicalSwapchain->effects.push_back(reshadeEffect);
                        if (reshadeEffect->getOutputWrites() == 0)
                        {
                            Logger::debug("deterministic forwarding for zero-output-write effect: " + effectStrings[i]);
                            pLogicalSwapchain->effects.push_back(
                                createForwardingEffect(pLogicalSwapchain, pLogicalDevice, firstImages, secondImages, pConfig, outputIsSwapchain));
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Logger::err("Failed to create ReshadeEffect " + effectStrings[i] + ": " + e.what());
                        setStageError(e.what());
                        pLogicalSwapchain->effects.push_back(
                            createForwardingEffect(pLogicalSwapchain, pLogicalDevice, firstImages, secondImages, pConfig, outputIsSwapchain));
                    }
                    signalJmpActive = 0;
                }
            }
        }

        // Forward the final intermediate image when effects cannot write directly to the swapchain.
        if (!directOutput)
        {
            pLogicalSwapchain->effects.push_back(createForwardingEffect(
                pLogicalSwapchain,
                pLogicalDevice,
                std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount, pLogicalSwapchain->fakeImages.end()),
                pLogicalSwapchain->images,
                pConfig,
                true));
        }
    }

    // Helper function to reload effects for a swapchain (for hot-reload)
    void reloadEffectsForSwapchain(LogicalSwapchain* pLogicalSwapchain, Config* pConfig, const std::vector<std::string>& activeEffects = {})
    {
        LogicalDevice* pLogicalDevice = pLogicalSwapchain->pLogicalDevice;

        // Wait for GPU to finish
        pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);

        const auto settings = pLogicalDevice->imguiOverlay && pLogicalDevice->imguiOverlay->getTechniqueSettingsPath() == pConfig->getConfigFilePath()
                                  ? pLogicalDevice->imguiOverlay->getTechniqueSettings()
                                  : ConfigSerializer::loadProfileSettings(pConfig->getConfigFilePath());
        auto       stages   = buildTechniquePlan(activeEffects, settings);

        // Keep the images already handed to the application and append intermediate slots if an existing profile needs more.
        if (stages.size() > pLogicalSwapchain->maxEffectSlots)
        {
            const uint64_t extraCount  = static_cast<uint64_t>(stages.size() - pLogicalSwapchain->maxEffectSlots) * pLogicalSwapchain->imageCount;
            VkDeviceMemory extraMemory = VK_NULL_HANDLE;
            std::vector<VkImage> extraImages;
            if (extraCount <= std::numeric_limits<uint32_t>::max())
                extraImages =
                    createFakeSwapchainImages(pLogicalDevice, pLogicalSwapchain->swapchainCreateInfo, static_cast<uint32_t>(extraCount), extraMemory);
            if (extraImages.empty())
            {
                Logger::err("Could not allocate additional intermediate images for the technique chain");
                if (pLogicalDevice->imguiOverlay)
                    pLogicalDevice->imguiOverlay->setTechniqueNotice(
                        "Could not allocate images for the new technique order. The previous order is still active.");
                return;
            }
            else
            {
                pLogicalSwapchain->fakeImages.insert(pLogicalSwapchain->fakeImages.end(), extraImages.begin(), extraImages.end());
                pLogicalSwapchain->extraFakeImageMemory.push_back(extraMemory);
                pLogicalSwapchain->maxEffectSlots = stages.size();
            }
        }

        // Command buffers will be freed by reallocateCommandBuffers.
        pLogicalSwapchain->effects.clear();
        pLogicalSwapchain->defaultTransfer.reset();

        Logger::info("reloading " + std::to_string(stages.size()) + " effect stages");

        // Create effects using centralized helper
        createEffectsForSwapchain(pLogicalSwapchain, pLogicalDevice, pConfig, stages, true);

        // Create default transfer effect (needed for no-effect command buffers)
        pLogicalSwapchain->defaultTransfer = createForwardingEffect(
            pLogicalSwapchain,
            pLogicalDevice,
            std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin(), pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount),
            pLogicalSwapchain->images,
            pConfig,
            true);

        // Free old command buffers and allocate/write new ones
        DepthState depth = getDepthState(pLogicalDevice);
        reallocateCommandBuffers(pLogicalDevice, pLogicalSwapchain, depth);

        Logger::info("effects reloaded successfully");
    }

    // Reload effects for all swapchains belonging to a device
    void reloadAllSwapchains(LogicalDevice* pLogicalDevice, const std::vector<std::string>& activeEffects)
    {
        for (auto& [_, pLogicalSwapchain] : swapchainMap)
        {
            if (!pLogicalSwapchain->fakeImages.empty())
                reloadEffectsForSwapchain(pLogicalSwapchain.get(), pConfig.get(), activeEffects);
        }
    }

    // Build and update overlay state for rendering
    void updateOverlayState(LogicalDevice* pLogicalDevice, bool effectsEnabled)
    {
        if (!pLogicalDevice->imguiOverlay || !pLogicalDevice->imguiOverlay->isVisible())
            return;

        OverlayState overlayState;
        overlayState.effectNames = pLogicalDevice->imguiOverlay->getActiveEffects();

        // No fallback to config - registry is the single source of truth
        // (initialized from config at first swapchain creation)

        getAvailableEffects(pConfig.get(),
                            overlayState.currentConfigEffects,
                            overlayState.defaultConfigEffects,
                            overlayState.effectPaths,
                            overlayState.configuredEffectPaths,
                            overlayState.effectFiles,
                            overlayState.shaderIncludePaths);
        overlayState.configPath     = pConfig->getConfigFilePath();
        overlayState.baseConfigPath = pBaseConfig ? pBaseConfig->getConfigFilePath() : "";

        // Cache the filename extraction — config path rarely changes
        static std::string cachedConfigPath;
        static std::string cachedConfigName;
        if (overlayState.configPath != cachedConfigPath)
        {
            cachedConfigPath = overlayState.configPath;
            cachedConfigName = std::filesystem::path(cachedConfigPath).filename().string();
        }
        overlayState.configName     = cachedConfigName;
        overlayState.effectsEnabled = effectsEnabled;
        for (const auto& [_, swapchain] : swapchainMap)
            if (swapchain->pLogicalDevice == pLogicalDevice && swapchain->maxEffectSlots > 0)
                overlayState.maxTechniqueSlots = overlayState.maxTechniqueSlots == 0
                                                     ? swapchain->maxEffectSlots
                                                     : std::min(overlayState.maxTechniqueSlots, swapchain->maxEffectSlots);
        overlayState.depth.candidates   = pLogicalDevice->depthSourceCandidates;
        overlayState.depth.activeSource = pLogicalDevice->activeDepthState;
        if (pLogicalDevice->automaticClearSelection.selectedId != 0)
            overlayState.depth.automaticClear = static_cast<size_t>(pLogicalDevice->automaticClearSelection.selectedId - 1);
        if (const auto* candidate = selectAutomaticDepthCandidate(pLogicalDevice))
            overlayState.depth.automaticSource = candidate->depthState;
        overlayState.depth.sourceIndex = getDepthSourceOverrideIndex(pLogicalDevice, &overlayState.depth.sourceFromEnvironment);
        if (!overlayState.depth.sourceFromEnvironment && pLogicalDevice->depthSourcePinnedIndex == overlayState.depth.sourceIndex)
        {
            overlayState.depth.pinnedSource          = pLogicalDevice->depthSourceOverrideImage;
            overlayState.depth.pinnedSourceDestroyed = pLogicalDevice->depthSourceOverrideImageDestroyed;
        }
        overlayState.depth.captureEnabled = settingsManager.isDepthCaptureActive();

        // Ensure all selected effects are in the registry
        for (const auto& effectName : pLogicalDevice->imguiOverlay->getSelectedEffects())
        {
            if (effectRegistry.hasEffect(effectName))
                continue;
            auto        pathIt     = overlayState.effectPaths.find(effectName);
            std::string effectPath = (pathIt != overlayState.effectPaths.end()) ? pathIt->second : "";
            effectRegistry.ensureEffect(effectName, effectPath);
        }

        // Parameters now read directly from EffectRegistry, no need to pass via state
        pLogicalDevice->imguiOverlay->updateState(std::move(overlayState));
    }

    // Submit overlay command buffer if visible, returns semaphore to wait on
    VkResult submitOverlayFrame(LogicalDevice* pLogicalDevice, LogicalSwapchain* pSwapchain, uint32_t index, VkSemaphore& outSemaphore)
    {
        outSemaphore = pSwapchain->semaphores[index]; // Default: wait on effects semaphore

        if (!pLogicalDevice->imguiOverlay)
            return VK_SUCCESS;

        VkCommandBuffer overlayCmd = pLogicalDevice->imguiOverlay->recordFrame(
            index, pSwapchain->imageViews[index], pSwapchain->imageExtent.width, pSwapchain->imageExtent.height);

        if (overlayCmd == VK_NULL_HANDLE)
            return VK_SUCCESS;

        VkPipelineStageFlags overlayWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo         overlaySubmit    = {};
        overlaySubmit.sType                   = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        overlaySubmit.waitSemaphoreCount      = 1;
        overlaySubmit.pWaitSemaphores         = &pSwapchain->semaphores[index];
        overlaySubmit.pWaitDstStageMask       = &overlayWaitStage;
        overlaySubmit.commandBufferCount      = 1;
        overlaySubmit.pCommandBuffers         = &overlayCmd;
        overlaySubmit.signalSemaphoreCount    = 1;
        overlaySubmit.pSignalSemaphores       = &pSwapchain->overlaySemaphores[index];

        // Use fence to track command buffer completion (prevents reuse while in flight)
        VkFence  overlayFence = pLogicalDevice->imguiOverlay->getCommandBufferFence(index);
        VkResult vr;
        {
            scoped_lock l(globalLock);
            vr = pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &overlaySubmit, overlayFence);
        }
        if (vr == VK_SUCCESS)
            outSemaphore = pSwapchain->overlaySemaphores[index];

        return vr;
    }

    VkResult VKAPI_CALL vkShade_CreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                                               const VkAllocationCallbacks* pAllocator,
                                               VkInstance*                  pInstance)
    {
        VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerInstanceCreateInfo*) layerCreateInfo->pNext;
        }

        Logger::trace("vkCreateInstance");

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance) gpa(VK_NULL_HANDLE, "vkCreateInstance");

        VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
        VkApplicationInfo    appInfo;
        if (modifiedCreateInfo.pApplicationInfo)
        {
            appInfo = *(modifiedCreateInfo.pApplicationInfo);
            if (appInfo.apiVersion < VK_API_VERSION_1_1)
            {
                appInfo.apiVersion = VK_API_VERSION_1_1;
            }
        }
        else
        {
            appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pNext              = nullptr;
            appInfo.pApplicationName   = nullptr;
            appInfo.applicationVersion = 0;
            appInfo.pEngineName        = nullptr;
            appInfo.engineVersion      = 0;
            appInfo.apiVersion         = VK_API_VERSION_1_1;
        }

        modifiedCreateInfo.pApplicationInfo = &appInfo;
        VkResult ret                        = createFunc(&modifiedCreateInfo, pAllocator, pInstance);

        // fetch our own dispatch table for the functions we need, into the next layer
        InstanceDispatch dispatchTable;
        fillDispatchTableInstance(*pInstance, gpa, &dispatchTable);

        // store the table by key
        {
            scoped_lock l(globalLock);
            instanceDispatchMap[GetKey(*pInstance)] = dispatchTable;
            instanceMap[GetKey(*pInstance)]         = *pInstance;
            instanceVersionMap[GetKey(*pInstance)]  = modifiedCreateInfo.pApplicationInfo->apiVersion;
        }

        if (ret == VK_SUCCESS)
            vkshade_start_proton_bridge();

        return ret;
    }

    void VKAPI_CALL vkShade_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
    {
        if (!instance)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyInstance");

        InstanceDispatch dispatchTable = instanceDispatchMap[GetKey(instance)];

        dispatchTable.DestroyInstance(instance, pAllocator);

        instanceDispatchMap.erase(GetKey(instance));
        instanceMap.erase(GetKey(instance));
        instanceVersionMap.erase(GetKey(instance));
    }

    VkResult VKAPI_CALL vkShade_CreateDevice(VkPhysicalDevice             physicalDevice,
                                             const VkDeviceCreateInfo*    pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkDevice*                    pDevice)
    {
        scoped_lock l(globalLock);
        Logger::trace("vkCreateDevice");
        VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerDeviceCreateInfo*) layerCreateInfo->pNext;
        }

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr   gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice) gipa(VK_NULL_HANDLE, "vkCreateDevice");

        // check and activate extentions
        uint32_t extensionCount = 0;

        instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &extensionCount, extensionProperties.data());

        auto hasDeviceExtension = [&](const char* extensionName) {
            for (const VkExtensionProperties& properties : extensionProperties)
            {
                if (std::strcmp(properties.extensionName, extensionName) == 0)
                    return true;
            }
            return false;
        };

        const bool supportsMutableFormat          = hasDeviceExtension("VK_KHR_swapchain_mutable_format");
        const bool supportsNvCheckpointExt        = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        const bool supportsNvDiagnosticsConfigExt = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        const bool supportsDeviceFaultExt         = hasDeviceExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        const bool gpuCrashDiagnosticsRequested   = isGpuCrashDiagEnabled();
        if (supportsMutableFormat)
            Logger::debug("device supports VK_KHR_swapchain_mutable_format");

        bool hasMutableEnvOverride = false;
        bool mutableRequested      = getMutableSwapchainEnvOverride(hasMutableEnvOverride, true);

        VkPhysicalDeviceProperties deviceProps;
        instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceProperties(physicalDevice, &deviceProps);

        VkDeviceCreateInfo       modifiedCreateInfo = *pCreateInfo;
        std::vector<const char*> enabledExtensionNames;
        if (modifiedCreateInfo.enabledExtensionCount)
        {
            enabledExtensionNames = std::vector<const char*>(modifiedCreateInfo.ppEnabledExtensionNames,
                                                             modifiedCreateInfo.ppEnabledExtensionNames + modifiedCreateInfo.enabledExtensionCount);
        }

        if (supportsMutableFormat && mutableRequested)
        {
            Logger::debug("activating mutable_format");
            addUniqueCString(enabledExtensionNames, "VK_KHR_swapchain_mutable_format");
        }
        else if (hasMutableEnvOverride)
        {
            Logger::info(std::string("Mutable swapchain device extension set via VKSHADE_ENABLE_MUTABLE_SWAPCHAIN=")
                         + (mutableRequested ? "1" : "0"));
        }
        if (deviceProps.apiVersion < VK_API_VERSION_1_2 || instanceVersionMap[GetKey(physicalDevice)] < VK_API_VERSION_1_2)
        {
            addUniqueCString(enabledExtensionNames, "VK_KHR_image_format_list");
        }

        bool                                  enableNvCheckpoints       = false;
        bool                                  enableNvDiagnosticsConfig = false;
        bool                                  enableDeviceFault         = false;
        VkDeviceDiagnosticsConfigCreateInfoNV diagnosticsConfigInfo     = {};
        if (gpuCrashDiagnosticsRequested)
        {
            if (supportsNvCheckpointExt)
            {
                addUniqueCString(enabledExtensionNames, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
                enableNvCheckpoints = true;
            }
            else
            {
                Logger::warn("VKSHADE_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostic_checkpoints is not supported by this device");
            }

            if (supportsNvDiagnosticsConfigExt)
            {
                addUniqueCString(enabledExtensionNames, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
                enableNvDiagnosticsConfig = true;
            }
            else
            {
                Logger::warn("VKSHADE_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostics_config is not supported by this device");
            }

            if (supportsDeviceFaultExt)
            {
                addUniqueCString(enabledExtensionNames, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
                enableDeviceFault = true;
            }
            else
            {
                Logger::warn("VKSHADE_GPU_CRASH_DIAGNOSTICS=1 but VK_EXT_device_fault is not supported by this device");
            }
        }
        modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensionNames.data();
        modifiedCreateInfo.enabledExtensionCount   = enabledExtensionNames.size();

        const auto&                instanceDispatch = instanceDispatchMap[GetKey(physicalDevice)];
        const DeviceFeatureSupport featureSupport   = queryDeviceFeatureSupport(
            physicalDevice, instanceDispatch.GetPhysicalDeviceFeatures2, instanceDispatch.GetPhysicalDeviceFeatures, enableDeviceFault);
        DeviceFeatureConfiguration featureConfiguration(modifiedCreateInfo, featureSupport, enableDeviceFault);
        if (enableDeviceFault && !featureConfiguration.deviceFaultEnabled())
        {
            if (pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT))
                Logger::warn("VK_EXT_device_fault feature is disabled by the application");
            else if (!instanceDispatch.GetPhysicalDeviceFeatures2)
                Logger::warn("Cannot query VkPhysicalDeviceFaultFeaturesEXT (vkGetPhysicalDeviceFeatures2 unavailable)");
            else
                Logger::warn("VK_EXT_device_fault is present but deviceFault feature is not supported");
        }
        modifiedCreateInfo                     = featureConfiguration.createInfo();
        const bool deviceFaultExtensionEnabled = std::any_of(enabledExtensionNames.begin(), enabledExtensionNames.end(), [](const char* name) {
            return std::strcmp(name, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0;
        });
        void*      extendedDevicePNext         = const_cast<void*>(modifiedCreateInfo.pNext);

        if (enableNvDiagnosticsConfig
            && !pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV))
        {
            diagnosticsConfigInfo       = {};
            diagnosticsConfigInfo.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
            diagnosticsConfigInfo.flags =
                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
            diagnosticsConfigInfo.pNext = extendedDevicePNext;
            extendedDevicePNext         = &diagnosticsConfigInfo;
        }

        modifiedCreateInfo.pNext = extendedDevicePNext;

        VkResult ret = createFunc(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);

        if (ret != VK_SUCCESS)
            return ret;

        std::shared_ptr<LogicalDevice> pLogicalDevice(new LogicalDevice());
        pLogicalDevice->vki                             = instanceDispatchMap[GetKey(physicalDevice)];
        pLogicalDevice->device                          = *pDevice;
        pLogicalDevice->physicalDevice                  = physicalDevice;
        pLogicalDevice->enabledFeatures                 = featureConfiguration.enabledFeatures();
        pLogicalDevice->instance                        = instanceMap[GetKey(physicalDevice)];
        pLogicalDevice->queue                           = VK_NULL_HANDLE;
        pLogicalDevice->queueFamilyIndex                = 0;
        pLogicalDevice->commandPool                     = VK_NULL_HANDLE;
        pLogicalDevice->depthCopyBeforeClears           = pConfig->getOption<bool>("depthCopyBeforeClears", false);
        pLogicalDevice->depthClearIndex                 = pConfig->getOption<int32_t>("depthClearIndex", 0);
        pLogicalDevice->supportsMutableFormat           = supportsMutableFormat && mutableRequested;
        pLogicalDevice->isNvidiaGpu                     = (deviceProps.vendorID == 0x10DE);
        pLogicalDevice->gpuCrashDiagnosticsEnabled      = gpuCrashDiagnosticsRequested;
        pLogicalDevice->supportsNvDiagnosticCheckpoints = enableNvCheckpoints;
        pLogicalDevice->supportsNvDiagnosticsConfig     = enableNvDiagnosticsConfig;
        pLogicalDevice->supportsDeviceFaultExt          = deviceFaultExtensionEnabled && featureConfiguration.deviceFaultEnabled();

        fillDispatchTableDevice(*pDevice, gdpa, &pLogicalDevice->vkd);

        if (pLogicalDevice->gpuCrashDiagnosticsEnabled)
        {
            if (pLogicalDevice->supportsNvDiagnosticCheckpoints
                && !(pLogicalDevice->vkd.GetQueueCheckpointDataNV || pLogicalDevice->vkd.GetQueueCheckpointData2NV))
            {
                Logger::warn("VK_NV_device_diagnostic_checkpoints enabled but checkpoint query entry points are unavailable");
                pLogicalDevice->supportsNvDiagnosticCheckpoints = false;
            }

            if (pLogicalDevice->supportsDeviceFaultExt && !pLogicalDevice->vkd.GetDeviceFaultInfoEXT)
            {
                Logger::warn("VK_EXT_device_fault enabled but vkGetDeviceFaultInfoEXT entry point is unavailable");
                pLogicalDevice->supportsDeviceFaultExt = false;
            }

            Logger::info(std::string("GPU crash diagnostics enabled (NV checkpoints=")
                         + (pLogicalDevice->supportsNvDiagnosticCheckpoints ? "on" : "off")
                         + ", NV diagnostics config=" + (pLogicalDevice->supportsNvDiagnosticsConfig ? "on" : "off")
                         + ", device fault=" + (pLogicalDevice->supportsDeviceFaultExt ? "on" : "off") + ")");
        }

        uint32_t count;

        pLogicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(pLogicalDevice->physicalDevice, &count, nullptr);

        std::vector<VkQueueFamilyProperties> queueProperties(count);

        pLogicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(pLogicalDevice->physicalDevice, &count, queueProperties.data());
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
        {
            auto& queueInfo = pCreateInfo->pQueueCreateInfos[i];
            if ((queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                pLogicalDevice->vkd.GetDeviceQueue(pLogicalDevice->device, queueInfo.queueFamilyIndex, 0, &pLogicalDevice->queue);

                VkCommandPoolCreateInfo commandPoolCreateInfo;
                commandPoolCreateInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                commandPoolCreateInfo.pNext            = nullptr;
                commandPoolCreateInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                commandPoolCreateInfo.queueFamilyIndex = queueInfo.queueFamilyIndex;

                Logger::debug("Found graphics capable queue");
                pLogicalDevice->vkd.CreateCommandPool(pLogicalDevice->device, &commandPoolCreateInfo, nullptr, &pLogicalDevice->commandPool);
                pLogicalDevice->queueFamilyIndex = queueInfo.queueFamilyIndex;
                uint64_t queuesInFamily          = 0;
                for (uint32_t j = 0; j < pCreateInfo->queueCreateInfoCount; ++j)
                    if (pCreateInfo->pQueueCreateInfos[j].queueFamilyIndex == queueInfo.queueFamilyIndex)
                        queuesInFamily += pCreateInfo->pQueueCreateInfos[j].queueCount;
                pLogicalDevice->singleGraphicsQueue = queuesInFamily == 1 && !(queueInfo.flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT);

                initializeDispatchTable(pLogicalDevice->queue, pLogicalDevice->device);

                break;
            }
        }

        if (!pLogicalDevice->queue)
        {
            Logger::err("Did not find a graphics queue! vkShade requires a graphics-capable queue.");
            // Still register the device so destruction works, but effects won't function
        }

        deviceMap[GetKey(*pDevice)] = pLogicalDevice;

        return VK_SUCCESS;
    }

    void VKAPI_CALL vkShade_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        if (!device)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyDevice");

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        // Destroy ImGui overlay before device (it uses device resources)
        pLogicalDevice->imguiOverlay.reset();
        pLogicalDevice->depthSourceCandidates.clear();
        pLogicalDevice->depthFrameTracker.reset();
        pLogicalDevice->depthBackupPool.clear();

        // Clean up Wayland input resources (no-op if not initialized)
        cleanupWaylandKeyboard();
        cleanupWaylandMouse();
        cleanupWaylandInputCommon();

        if (pLogicalDevice->commandPool != VK_NULL_HANDLE)
        {
            Logger::debug("DestroyCommandPool");
            pLogicalDevice->vkd.DestroyCommandPool(device, pLogicalDevice->commandPool, pAllocator);
        }

        pLogicalDevice->vkd.DestroyDevice(device, pAllocator);

        deviceMap.erase(GetKey(device));
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateSwapchainKHR(VkDevice                        device,
                                                              const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                              const VkAllocationCallbacks*    pAllocator,
                                                              VkSwapchainKHR*                 pSwapchain)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateSwapchainKHR");

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        const SwapchainUsagePlan usagePlan = planSwapchainUsage(*pCreateInfo);
        if (usagePlan.passthrough)
        {
            Logger::warn("Passing through swapchain with unsupported protection, present mode, or image usage");
            auto pLogicalSwapchain            = std::make_shared<LogicalSwapchain>();
            pLogicalSwapchain->pLogicalDevice = pLogicalDevice;
            pLogicalSwapchain->passthrough    = true;
            VkResult result                   = pLogicalDevice->vkd.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
            if (result == VK_SUCCESS && pSwapchain != nullptr && *pSwapchain != VK_NULL_HANDLE)
                swapchainMap[*pSwapchain] = pLogicalSwapchain;
            return result;
        }

        VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.imageUsage               = usagePlan.imageUsage;

        VkFormat format = modifiedCreateInfo.imageFormat;

        VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
        VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;
        Logger::debug(std::to_string(srgbFormat) + " " + std::to_string(unormFormat));

        VkFormat formats[] = {unormFormat, srgbFormat};

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        bool                           hasMutableEnvOverride = false;
        bool useMutableFormat = getMutableSwapchainEnvOverride(hasMutableEnvOverride, pLogicalDevice->supportsMutableFormat);
        if (useMutableFormat && !pLogicalDevice->supportsMutableFormat)
        {
            Logger::warn(
                "Mutable swapchain forced on via VKSHADE_ENABLE_MUTABLE_SWAPCHAIN, but device does not support it. Falling back to disabled.");
            useMutableFormat = false;
        }
        else if (useMutableFormat && pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
        {
            Logger::warn(
                "Application already provides VkImageFormatListCreateInfo in swapchain pNext; disabling mutable override for compatibility.");
            useMutableFormat = false;
        }
        else if (hasMutableEnvOverride)
        {
            Logger::info(std::string("Mutable swapchain explicitly set via VKSHADE_ENABLE_MUTABLE_SWAPCHAIN=") + (useMutableFormat ? "1" : "0"));
        }

        if (useMutableFormat)
        {
            // Keep application-requested usage bits and add the ones vkShade needs.
            modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // mutable path renders final pass directly into swapchain
            modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
            // TODO what if the application already uses multiple formats for the swapchain?

            imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
            imageFormatListCreateInfo.pNext           = modifiedCreateInfo.pNext;
            imageFormatListCreateInfo.viewFormatCount = (srgbFormat == unormFormat) ? 1 : 2;
            imageFormatListCreateInfo.pViewFormats    = formats;

            modifiedCreateInfo.pNext = &imageFormatListCreateInfo;
        }

        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "vkShade diag: CreateSwapchainKHR mutable=%d reqUsage=0x%x finalUsage=0x%x reqFlags=0x%x finalFlags=0x%x format=%u "
                         "extent=%ux%u minImages=%u\n",
                         useMutableFormat ? 1 : 0,
                         static_cast<unsigned int>(pCreateInfo->imageUsage),
                         static_cast<unsigned int>(modifiedCreateInfo.imageUsage),
                         static_cast<unsigned int>(pCreateInfo->flags),
                         static_cast<unsigned int>(modifiedCreateInfo.flags),
                         static_cast<unsigned int>(modifiedCreateInfo.imageFormat),
                         static_cast<unsigned int>(modifiedCreateInfo.imageExtent.width),
                         static_cast<unsigned int>(modifiedCreateInfo.imageExtent.height),
                         static_cast<unsigned int>(modifiedCreateInfo.minImageCount));
        }

        Logger::debug("format " + std::to_string(modifiedCreateInfo.imageFormat));
        std::shared_ptr<LogicalSwapchain> pLogicalSwapchain(new LogicalSwapchain());
        pLogicalSwapchain->pLogicalDevice      = pLogicalDevice;
        pLogicalSwapchain->swapchainCreateInfo = *pCreateInfo;
        if (pCreateInfo->imageSharingMode == VK_SHARING_MODE_CONCURRENT && pCreateInfo->queueFamilyIndexCount && pCreateInfo->pQueueFamilyIndices)
        {
            pLogicalSwapchain->queueFamilyIndices.assign(pCreateInfo->pQueueFamilyIndices,
                                                         pCreateInfo->pQueueFamilyIndices + pCreateInfo->queueFamilyIndexCount);
            pLogicalSwapchain->swapchainCreateInfo.pQueueFamilyIndices = pLogicalSwapchain->queueFamilyIndices.data();
        }
        else if (pCreateInfo->imageSharingMode != VK_SHARING_MODE_CONCURRENT)
        {
            pLogicalSwapchain->swapchainCreateInfo.queueFamilyIndexCount = 0;
            pLogicalSwapchain->swapchainCreateInfo.pQueueFamilyIndices   = nullptr;
        }
        pLogicalSwapchain->imageExtent            = modifiedCreateInfo.imageExtent;
        pLogicalSwapchain->format                 = modifiedCreateInfo.imageFormat;
        pLogicalSwapchain->imageCount             = 0;
        pLogicalSwapchain->useMutableFormat       = useMutableFormat;
        pLogicalSwapchain->useTransferToSwapchain = usagePlan.useTransfer;

        VkResult result = pLogicalDevice->vkd.CreateSwapchainKHR(device, &modifiedCreateInfo, pAllocator, pSwapchain);

        if (result == VK_SUCCESS && pSwapchain != nullptr && *pSwapchain != VK_NULL_HANDLE)
            swapchainMap[*pSwapchain] = pLogicalSwapchain;
        else
            Logger::err("vkCreateSwapchainKHR failed: " + std::to_string(result));

        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_GetSwapchainImagesKHR(VkDevice       device,
                                                                 VkSwapchainKHR swapchain,
                                                                 uint32_t*      pCount,
                                                                 VkImage*       pSwapchainImages)
    {
        scoped_lock l(globalLock);
        if (pCount == nullptr)
            return VK_ERROR_INITIALIZATION_FAILED;
        Logger::trace("vkGetSwapchainImagesKHR " + std::to_string(*pCount));

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (pLogicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;

        if (pSwapchainImages == nullptr)
        {
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);
        }

        auto swapchainIt = swapchainMap.find(swapchain);
        if (swapchainIt == swapchainMap.end() || !swapchainIt->second)
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);
        LogicalSwapchain* pLogicalSwapchain = swapchainIt->second.get();
        if (pLogicalSwapchain->passthrough)
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);

        // If the images got already requested once, return them again instead of creating new images
        if (pLogicalSwapchain->fakeImages.size())
        {
            if (pLogicalSwapchain->fakeImages.size() < pLogicalSwapchain->imageCount)
            {
                Logger::err("fake image cache is smaller than imageCount");
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            *pCount = std::min<uint32_t>(*pCount, pLogicalSwapchain->imageCount);
            std::memcpy(pSwapchainImages, pLogicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
            // [DEBUG-TAG: swapchain-snapshot-probe-20260424]
            static uint32_t cachedSwapchainImageProbeCount = 0;
            if (cachedSwapchainImageProbeCount++ < 8)
            {
                Logger::debug("swapchain snapshot probe cached images: swapchain=" + convertToString(swapchain)
                              + " returned=" + std::to_string(*pCount) + " imageCount=" + std::to_string(pLogicalSwapchain->imageCount)
                              + " fakeImageCount=" + std::to_string(pLogicalSwapchain->fakeImages.size()));
                for (uint32_t i = 0; i < *pCount; ++i)
                    Logger::debug("swapchain snapshot probe cached image[" + std::to_string(i) + "]: fake=" + convertToString(pSwapchainImages[i]));
            }
            return *pCount < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }

        const uint32_t requestedImageCapacity = *pCount;
        uint32_t       realImageCount         = 0;
        VkResult       getCountResult         = pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &realImageCount, nullptr);
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "vkShade diag: GetSwapchainImagesKHR count-query result=%d count=%u\n",
                         static_cast<int>(getCountResult),
                         static_cast<unsigned int>(realImageCount));
        }
        if (getCountResult != VK_SUCCESS && getCountResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(count) failed: " + std::to_string(getCountResult));
            return getCountResult;
        }
        if (realImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR returned zero images");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkImage> realImages(realImageCount);
        uint32_t             fetchedImageCount = realImageCount;
        VkResult             getImagesResult   = pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &fetchedImageCount, realImages.data());
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "vkShade diag: GetSwapchainImagesKHR fetch result=%d fetched=%u\n",
                         static_cast<int>(getImagesResult),
                         static_cast<unsigned int>(fetchedImageCount));
        }
        if (getImagesResult != VK_SUCCESS && getImagesResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(images) failed: " + std::to_string(getImagesResult));
            return getImagesResult;
        }
        if (fetchedImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR fetched zero images");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        realImages.resize(fetchedImageCount);
        pLogicalSwapchain->imageCount = fetchedImageCount;
        pLogicalSwapchain->images     = std::move(realImages);

        // Create image views for overlay rendering
        pLogicalSwapchain->imageViews.resize(pLogicalSwapchain->imageCount);
        for (uint32_t i = 0; i < pLogicalSwapchain->imageCount; i++)
        {
            VkImageViewCreateInfo viewInfo           = {};
            viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image                           = pLogicalSwapchain->images[i];
            viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format                          = pLogicalSwapchain->format;
            viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel   = 0;
            viewInfo.subresourceRange.levelCount     = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount     = 1;
            VkResult viewResult = pLogicalDevice->vkd.CreateImageView(pLogicalDevice->device, &viewInfo, nullptr, &pLogicalSwapchain->imageViews[i]);
            if (viewResult != VK_SUCCESS)
                Logger::err("Failed to create swapchain image view " + std::to_string(i) + ": " + std::to_string(viewResult));
        }

        // Initialize registry from config on first run (before calculating effect slots)
        bool isFirstRun = !effectRegistry.isInitializedFromConfig();
        if (isFirstRun)
            effectRegistry.initializeSelectedEffectsFromConfig();

        const auto& selectedEffects   = effectRegistry.getSelectedEffects();
        const auto  techniqueSettings = ConfigSerializer::loadProfileSettings(pConfig->getConfigFilePath());
        const auto  stages            = buildTechniquePlan(selectedEffects, techniqueSettings);

        // Allow dynamic effect loading by allocating for more effects than configured.
        // Clamp maxEffects to a safe range to avoid pathological allocations.
        int32_t maxEffects                = std::clamp(settingsManager.getMaxEffects(), 1, 200);
        size_t  effectSlots               = std::max(stages.size(), static_cast<size_t>(maxEffects));
        pLogicalSwapchain->maxEffectSlots = effectSlots;

        // A ReShade pass may copy into its output, so direct output also requires transfer-destination usage.
        const bool     directOutput     = canUseDirectEffectOutput(pLogicalSwapchain->useMutableFormat, pLogicalSwapchain->useTransferToSwapchain);
        const uint64_t slotCount        = static_cast<uint64_t>(effectSlots) + (directOutput ? 0u : 1u);
        const uint64_t fakeImageCount64 = static_cast<uint64_t>(pLogicalSwapchain->imageCount) * slotCount;
        if (fakeImageCount64 == 0 || fakeImageCount64 > std::numeric_limits<uint32_t>::max())
        {
            Logger::err("Invalid fake image count computed: " + std::to_string(fakeImageCount64));
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        uint32_t fakeImageCount = static_cast<uint32_t>(fakeImageCount64);

        pLogicalSwapchain->fakeImages =
            createFakeSwapchainImages(pLogicalDevice, pLogicalSwapchain->swapchainCreateInfo, fakeImageCount, pLogicalSwapchain->fakeImageMemory);
        if (pLogicalSwapchain->fakeImages.empty())
        {
            Logger::err("Failed to create fake swapchain images");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        Logger::debug("created fake swapchain images");
        resetDepthSelectionForSwapchainRebuildLocked(pLogicalDevice);
        // [DEBUG-TAG: swapchain-snapshot-probe-20260424]
        static uint32_t createdSwapchainImageProbeCount = 0;
        if (createdSwapchainImageProbeCount++ < 8)
        {
            Logger::debug("swapchain snapshot probe created images: swapchain=" + convertToString(swapchain)
                          + " realImageCount=" + std::to_string(pLogicalSwapchain->images.size())
                          + " fakeImageCount=" + std::to_string(pLogicalSwapchain->fakeImages.size()) + " presentableFakeCount="
                          + std::to_string(pLogicalSwapchain->imageCount) + " requestedCapacity=" + std::to_string(requestedImageCapacity));
            for (uint32_t i = 0; i < pLogicalSwapchain->imageCount; ++i)
            {
                Logger::debug("swapchain snapshot probe image[" + std::to_string(i) + "]: real=" + convertToString(pLogicalSwapchain->images[i])
                              + " fake=" + convertToString(pLogicalSwapchain->fakeImages[i])
                              + " internalView=" + convertToString(pLogicalSwapchain->imageViews[i]));
            }
        }

        if (!isFirstRun && !selectedEffects.empty())
        {
            // Resize with effects - use pass-through and debounce for smooth resize
            Logger::debug("using pass-through during resize, will restore effects after debounce");
            std::vector<VkImage> firstImages(pLogicalSwapchain->fakeImages.begin(),
                                             pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount);
            pLogicalSwapchain->effects.push_back(
                createForwardingEffect(pLogicalSwapchain, pLogicalDevice, firstImages, pLogicalSwapchain->images, pConfig.get(), true));

            resizeDebounce.pending        = true;
            resizeDebounce.lastResizeTime = std::chrono::steady_clock::now();
        }
        else
        {
            // First run OR empty effects - create effects from registry
            createEffectsForSwapchain(pLogicalSwapchain, pLogicalDevice, pConfig.get(), stages, true);
        }

        DepthState depth = getDepthState(pLogicalDevice);

        Logger::debug("selected effect count: " + std::to_string(selectedEffects.size()));
        Logger::debug("effect count: " + std::to_string(pLogicalSwapchain->effects.size()));

        pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
        Logger::debug("allocated ComandBuffers " + std::to_string(pLogicalSwapchain->commandBuffersEffect.size()) + " for swapchain "
                      + convertToString(swapchain));

        ensureDepthResolveResources(pLogicalSwapchain, depth);
        writeCommandBuffers(pLogicalDevice, pLogicalSwapchain, pLogicalSwapchain->effects, pLogicalSwapchain->commandBuffersEffect);
        Logger::debug("wrote CommandBuffers");

        pLogicalSwapchain->semaphores        = createSemaphores(pLogicalDevice, pLogicalSwapchain->imageCount);
        pLogicalSwapchain->overlaySemaphores = createSemaphores(pLogicalDevice, pLogicalSwapchain->imageCount);
        Logger::debug("created semaphores");
        for (unsigned int i = 0; i < pLogicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(pLogicalSwapchain->commandBuffersEffect[i]));
        }
        Logger::trace("vkGetSwapchainImagesKHR");

        pLogicalSwapchain->defaultTransfer = createForwardingEffect(
            pLogicalSwapchain,
            pLogicalDevice,
            std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin(), pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount),
            pLogicalSwapchain->images,
            pConfig.get(),
            true);

        pLogicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);

        writeCommandBuffers(pLogicalDevice, pLogicalSwapchain, {pLogicalSwapchain->defaultTransfer}, pLogicalSwapchain->commandBuffersNoEffect);

        for (unsigned int i = 0; i < pLogicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(pLogicalSwapchain->commandBuffersNoEffect[i]));
        }

        // Create ImGui overlay at device level (if not already created)
        // This survives swapchain recreation during resize
        if (!pLogicalDevice->imguiOverlay)
        {
            if (!pLogicalDevice->overlayPersistentState)
                pLogicalDevice->overlayPersistentState = std::make_unique<OverlayPersistentState>();
            pLogicalDevice->imguiOverlay = std::make_unique<ImGuiOverlay>(
                pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageCount, pLogicalDevice->overlayPersistentState.get());
            // Set the effect registry pointer (single source of truth for enabled states)
            pLogicalDevice->imguiOverlay->setEffectRegistry(&effectRegistry);

            // Set game/profile info for auto-save
            pLogicalDevice->imguiOverlay->setGameProfile(detectedGameName, activeProfileName, activeProfilePath);

            // Initialize input blocking (grabs all input when overlay is visible)
            static bool inputBlockerInited = false;
            if (!inputBlockerInited)
            {
                initInputBlocker(settingsManager.getOverlayBlockInput());
                setInputBlocked(pLogicalDevice->imguiOverlay->isVisible());
                inputBlockerInited = true;
            }
        }

        if (pLogicalSwapchain->fakeImages.size() < pLogicalSwapchain->imageCount)
        {
            Logger::err("fake image vector too small for swapchain copy");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        *pCount = std::min<uint32_t>(requestedImageCapacity, pLogicalSwapchain->imageCount);
        std::memcpy(pSwapchainImages, pLogicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
        // [DEBUG-TAG: swapchain-snapshot-probe-20260424]
        static uint32_t returnedSwapchainImageProbeCount = 0;
        if (returnedSwapchainImageProbeCount++ < 8)
        {
            Logger::debug("swapchain snapshot probe returned images: swapchain=" + convertToString(swapchain) + " returned=" + std::to_string(*pCount)
                          + " requestedCapacity=" + std::to_string(requestedImageCapacity)
                          + " imageCount=" + std::to_string(pLogicalSwapchain->imageCount));
            for (uint32_t i = 0; i < *pCount; ++i)
                Logger::debug("swapchain snapshot probe returned image[" + std::to_string(i) + "]: fake=" + convertToString(pSwapchainImages[i]));
        }
        return requestedImageCapacity < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(queue));
        if (device == deviceMap.end() || !device->second)
            return VK_ERROR_DEVICE_LOST;
        auto*      logicalDevice = device->second.get();
        const auto result        = logicalDevice->vkd.QueueSubmit(queue, submitCount, submits, fence);
        if (result == VK_SUCCESS && settingsManager.isDepthCaptureActive() && submits)
            for (uint32_t i = 0; i < submitCount; ++i)
                for (uint32_t j = 0; j < submits[i].commandBufferCount; ++j)
                    logicalDevice->depthFrameTracker.submit(submits[i].pCommandBuffers[j]);
        return result;
    }

    static VkResult submitTracked2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits, VkFence fence, bool khr)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(queue));
        if (device == deviceMap.end() || !device->second)
            return VK_ERROR_DEVICE_LOST;
        auto*      logicalDevice = device->second.get();
        const auto submit        = khr ? logicalDevice->vkd.QueueSubmit2KHR : logicalDevice->vkd.QueueSubmit2;
        if (!submit)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto result = submit(queue, submitCount, submits, fence);
        if (result == VK_SUCCESS && settingsManager.isDepthCaptureActive() && submits)
            for (uint32_t i = 0; i < submitCount; ++i)
                for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j)
                    logicalDevice->depthFrameTracker.submit(submits[i].pCommandBufferInfos[j].commandBuffer);
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_QueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits, VkFence fence)
    {
        return submitTracked2(queue, submitCount, submits, fence, false);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_QueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2KHR* submits, VkFence fence)
    {
        return submitTracked2(queue, submitCount, submits, fence, true);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        if (isNonWaylandSurface())
        {
            std::shared_ptr<LogicalDevice> passThroughDevice;

            {
                scoped_lock l(globalLock);

                auto devIt = deviceMap.find(GetKey(queue));
                if (devIt == deviceMap.end() || !devIt->second)
                    return VK_ERROR_DEVICE_LOST;
                if (!devIt->second->queue)
                    return devIt->second->vkd.QueuePresentKHR(queue, pPresentInfo);

                passThroughDevice = devIt->second;
            }

            return passThroughDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
        }

        std::shared_ptr<LogicalDevice> passThroughDevice;
        {
            scoped_lock l(globalLock);
            bool        allPassthrough = pPresentInfo->swapchainCount > 0;
            for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
            {
                const auto it = swapchainMap.find(pPresentInfo->pSwapchains[i]);
                if (it == swapchainMap.end() || !it->second || !it->second->passthrough)
                {
                    allPassthrough = false;
                    break;
                }
            }
            if (allPassthrough)
            {
                const auto it = deviceMap.find(GetKey(queue));
                if (it != deviceMap.end())
                    passThroughDevice = it->second;
            }
        }
        if (passThroughDevice)
        {
            VkResult    result = passThroughDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
            scoped_lock l(globalLock);
            passThroughDevice->frameDepthCandidates.clear();
            passThroughDevice->bestDepthCandidate = {};
            return result;
        }

        // Mark new input frame so dispatch deduplication resets
        beginWaylandInputFrame();
        beginKeyboardInputFrame();

        // Keybindings - read from settingsManager (can be updated when settings are saved)
        static uint32_t keySymbol        = convertToKeySym(settingsManager.getToggleKey());
        static uint32_t reloadKeySymbol  = convertToKeySym(settingsManager.getReloadKey());
        static uint32_t overlayKeySymbol = convertToKeySym(settingsManager.getOverlayKey());
        static bool     initLogged       = false;

        static bool pressed        = false;
        static bool presentEffect  = settingsManager.getEnableOnLaunch();
        static bool reloadPressed  = false;
        static bool overlayPressed = false;

        std::shared_ptr<LogicalDevice>                 pLogicalDeviceShared;
        bool                                           presentEffectSnapshot = false;
        std::vector<std::shared_ptr<LogicalSwapchain>> presentSwapchains;
        std::vector<uint32_t>                          presentIndices;
        std::shared_ptr<DepthBackup>                   presentDepthBackup;
        DepthState                                     presentDepthState;
        bool                                           presentCopyBeforeClears = false;
        bool                                           presentDepthFallback    = false;
        bool                                           presentDepthFresh       = false;

        {
            scoped_lock l(globalLock);

            // Guard: if no device for this queue, pass through
            auto devIt = deviceMap.find(GetKey(queue));
            if (devIt == deviceMap.end() || !devIt->second)
                return VK_ERROR_DEVICE_LOST;
            if (!devIt->second->queue)
                return devIt->second->vkd.QueuePresentKHR(queue, pPresentInfo);

            LogicalDevice* pDeviceForSettings = devIt->second.get();

            // Check if settings were saved (re-read from settingsManager which is already updated by UI)
            if (pDeviceForSettings && pDeviceForSettings->imguiOverlay && pDeviceForSettings->imguiOverlay->hasSettingsSaved())
            {
                // settingsManager is already updated by the UI, just re-read the values
                keySymbol        = convertToKeySym(settingsManager.getToggleKey());
                reloadKeySymbol  = convertToKeySym(settingsManager.getReloadKey());
                overlayKeySymbol = convertToKeySym(settingsManager.getOverlayKey());
                initInputBlocker(settingsManager.getOverlayBlockInput());
                if (pDeviceForSettings->imguiOverlay)
                    setInputBlocked(pDeviceForSettings->imguiOverlay->isVisible());
                pDeviceForSettings->imguiOverlay->clearSettingsSaved();
                Logger::info("Settings reloaded from SettingsManager");
            }

            // Check if shader paths were changed (refresh available effects list)
            if (pDeviceForSettings && pDeviceForSettings->imguiOverlay && pDeviceForSettings->imguiOverlay->hasShaderPathsChanged())
            {
                cachedEffects.initialized = false; // Force re-scan of available effects
                pDeviceForSettings->imguiOverlay->clearShaderPathsChanged();
                pDeviceForSettings->imguiOverlay->invalidateShaderMetadata();
                Logger::info("Shader paths changed, effect list refreshed");
            }

            if (!initLogged)
            {
                Logger::info("hot-reload initialized, config: " + pConfig->getConfigFilePath());
                initLogged = true;
            }

            // Toggle effect on/off (keyboard)
            if (handleKeyPress(keySymbol, pressed))
                presentEffect = !presentEffect;

            // Hot-reload: check for key press or config file change
            bool shouldReload = false;
            if (handleKeyPress(reloadKeySymbol, reloadPressed))
            {
                Logger::debug("reload key pressed");
                if (pDeviceForSettings && pDeviceForSettings->imguiOverlay)
                    pDeviceForSettings->imguiOverlay->invalidateShaderMetadata();
                shouldReload = true;
            }
            if (pConfig->hasConfigChanged())
            {
                Logger::debug("config file changed detected");
                shouldReload = true;
            }

            // Toggle overlay on/off
            if (handleKeyPress(overlayKeySymbol, overlayPressed))
            {
                if (pDeviceForSettings->imguiOverlay)
                    pDeviceForSettings->imguiOverlay->toggle();
            }

            // Check for Apply button press in overlay (overlay is at device level)
            LogicalDevice* pLogicalDevice = pDeviceForSettings;

            if (pLogicalDevice->imguiOverlay)
                pLogicalDevice->imguiOverlay->pollPreprocessorEdit();

            // Toggle effects on/off via overlay checkbox
            if (pLogicalDevice->imguiOverlay && pLogicalDevice->imguiOverlay->hasToggleEffectsRequest())
            {
                presentEffect = !presentEffect;
                pLogicalDevice->imguiOverlay->clearToggleEffectsRequest();
            }

            if (pLogicalDevice->imguiOverlay && pLogicalDevice->imguiOverlay->hasModifiedParams())
            {
                // If we're loading a new config, don't apply old params - just trigger reload
                if (!pLogicalDevice->imguiOverlay->hasPendingConfig())
                    applyOverlayParams(pLogicalDevice);

                pLogicalDevice->imguiOverlay->clearApplyRequest();
                shouldReload = true;
            }

            if (shouldReload)
            {
                Logger::info("hot-reloading config and effects...");

                // Check if overlay wants to load a different config
                if (pLogicalDevice->imguiOverlay && pLogicalDevice->imguiOverlay->hasPendingConfig())
                {
                    std::string newConfigPath = pLogicalDevice->imguiOverlay->getPendingConfigPath();
                    switchConfig(newConfigPath);
                    // Update overlay with effects from the new config
                    std::vector<std::string> newEffects      = pConfig->getOption<std::vector<std::string>>("effects", {});
                    std::vector<std::string> disabledEffects = pConfig->getOption<std::vector<std::string>>("disabledEffects", {});
                    pLogicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                    pLogicalDevice->imguiOverlay->clearPendingConfig();
                    pLogicalDevice->imguiOverlay->clearDepthDefinitionsChanged();
                    pLogicalDevice->imguiOverlay->markDirty(); // Defer reload via debounce
                }
                else
                {
                    pConfig->reload();
                    bool depthDefinitionsChanged = false;
                    if (pLogicalDevice->imguiOverlay)
                    {
                        depthDefinitionsChanged = pLogicalDevice->imguiOverlay->refreshDepthDefinitionsFromConfig();
                        depthDefinitionsChanged |= pLogicalDevice->imguiOverlay->takeDepthDefinitionsChanged();
                    }
                    if (depthDefinitionsChanged)
                        effectRegistry.initialize(pConfig.get());
                    else if (!pLogicalDevice->imguiOverlay)
                        effectRegistry.setDepthDefinitions(ConfigSerializer::loadDepthDefinitions(pConfig.get()));
                    cachedEffects.initialized = false;
                    cachedParams.dirty        = true;

                    std::vector<std::string> activeEffects = pLogicalDevice->imguiOverlay
                                                                 ? pLogicalDevice->imguiOverlay->getActiveEffects()
                                                                 : pConfig->getOption<std::vector<std::string>>("effects", {});

                    reloadAllSwapchains(pLogicalDevice, activeEffects);
                }
            }

            // Check for debounced resize reload (separate from config reload)
            // Only call steady_clock::now() when a resize is actually pending
            if (resizeDebounce.pending)
            {
                auto resizeElapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - resizeDebounce.lastResizeTime).count();

                if (resizeElapsed >= RESIZE_DEBOUNCE_MS)
                {
                    Logger::info("debounced resize reload after " + std::to_string(resizeElapsed) + "ms");
                    resizeDebounce.pending = false;

                    // Get selected effects from registry (single source of truth)
                    const auto& selectedEffects = effectRegistry.getSelectedEffects();
                    for (auto& [_, pSwapchain] : swapchainMap)
                    {
                        if (pSwapchain->fakeImages.empty())
                            continue;
                        reloadEffectsForSwapchain(pSwapchain.get(), pConfig.get(), selectedEffects);
                    }
                }
            }

            // Keep lock scope small: snapshot pointers and immutable per-present state,
            // then do command submission and present outside the global mutex.
            const bool depthFrameHadWork             = publishDepthSourceCandidatesLocked(pLogicalDevice);
            pLogicalDevice->presentUsesGraphicsQueue = queue == pLogicalDevice->queue;
            if (!pLogicalDevice->presentUsesGraphicsQueue)
                for (auto& candidate : pLogicalDevice->depthSourceCandidates)
                    for (auto& clear : candidate.clears)
                        if (clear.requiresPresentQueue)
                        {
                            clear.copyPossible = false;
                            clear.copied       = false;
                            clear.backup.reset();
                        }
            bool depthSelectionChanged = false;
            bool depthCopyModeChanged  = false;
            if (pLogicalDevice->imguiOverlay)
            {
                const auto request = pLogicalDevice->imguiOverlay->takeDepthSourceRequest();
                if (request)
                {
                    pLogicalDevice->depthSourceOverrideIndex = request->index;
                    pLogicalDevice->depthSourceOverrideImage = request->pinnedSource;
                    pLogicalDevice->depthSourcePinnedIndex =
                        request->pinnedSource.image != VK_NULL_HANDLE ? request->index.value_or(DEPTH_SOURCE_AUTO) : DEPTH_SOURCE_AUTO;
                    pLogicalDevice->depthSourceOverrideImageDestroyed = false;
                    if (request->pinnedSource.image != VK_NULL_HANDLE)
                    {
                        const auto image = pLogicalDevice->depthImageMetadata.find(request->pinnedSource.image);
                        if (image == pLogicalDevice->depthImageMetadata.end() || image->second.sourceId != request->pinnedSource.sourceId)
                        {
                            pLogicalDevice->depthSourceOverrideImage          = {};
                            pLogicalDevice->depthSourceOverrideImageDestroyed = true;
                        }
                    }
                    depthSelectionChanged = true;
                }
                const auto copyRequest = pLogicalDevice->imguiOverlay->takeDepthCopyRequest();
                if (copyRequest)
                {
                    depthCopyModeChanged = pLogicalDevice->depthCopyBeforeClears != copyRequest->enabled;
                    if (depthCopyModeChanged || pLogicalDevice->depthClearIndex != copyRequest->clearIndex)
                    {
                        pLogicalDevice->depthCopyTargets.clear();
                        pLogicalDevice->depthCopyFallbackTarget = {};
                        pLogicalDevice->automaticClearSelection = {};
                        for (auto& [_, swapchain] : swapchainMap)
                            if (swapchain && swapchain->pLogicalDevice == pLogicalDevice)
                                invalidateDepthResolveOutput(swapchain.get());
                    }
                    pLogicalDevice->depthCopyBeforeClears = copyRequest->enabled;
                    pLogicalDevice->depthClearIndex       = copyRequest->clearIndex;
                }
            }
            bool          fromEnvironment = false;
            const int32_t overrideIndex   = getDepthSourceOverrideIndex(pLogicalDevice, &fromEnvironment);
            if (overrideIndex != DEPTH_SOURCE_AUTO || depthSelectionChanged)
                pLogicalDevice->automaticDepthSelection = {};
            LogicalDevice::DepthCandidateTrackingState selectedOverrideCandidate;
            const auto*                                overrideCandidate =
                selectDepthSourceOverrideCandidate(pLogicalDevice, overrideIndex, selectedOverrideCandidate, fromEnvironment);
            if (!settingsManager.isDepthCaptureActive())
                updateDeviceDepthStateLocked(pLogicalDevice, {}, "QueuePresentKHR(depth-capture-disabled)", true);
            else if (overrideCandidate != nullptr)
            {
                if (!fromEnvironment && overrideIndex >= 0 && pLogicalDevice->depthSourcePinnedIndex != overrideIndex)
                {
                    pLogicalDevice->depthSourceOverrideImage          = overrideCandidate->depthState;
                    pLogicalDevice->depthSourcePinnedIndex            = overrideIndex;
                    pLogicalDevice->depthSourceOverrideImageDestroyed = false;
                }
                updateDeviceDepthStateLocked(pLogicalDevice, overrideCandidate->depthState, "QueuePresentKHR(depth-source-override)", true);
            }
            else if (overrideIndex != DEPTH_SOURCE_AUTO)
            {
                updateDeviceDepthStateLocked(pLogicalDevice, {}, "QueuePresentKHR(depth-source-override-missing)", true);
                Logger::debug("depth source override unavailable: index=" + std::to_string(overrideIndex)
                              + " candidates=" + std::to_string(pLogicalDevice->depthSourceCandidates.size()));
            }
            else if (overrideIndex == DEPTH_SOURCE_AUTO)
            {
                const auto index = selectStableAutomaticDepthSource(
                    pLogicalDevice->depthSourceCandidates,
                    pLogicalDevice->automaticDepthSelection,
                    std::chrono::steady_clock::now(),
                    [pLogicalDevice](const auto& candidate) { return isAutomaticDepthCandidate(pLogicalDevice, candidate); });
                const auto* candidate = index ? &pLogicalDevice->depthSourceCandidates[*index] : nullptr;
                if (candidate)
                {
                    auto& best = pLogicalDevice->bestDepthCandidate;
                    if (!best.valid || !sameDepthSource(best.depthState, candidate->depthState))
                        best = {};
                    best.valid      = true;
                    best.depthState = candidate->depthState;
                    best.stats      = candidate->stats;
                    if (!sameDepthState(pLogicalDevice->activeDepthState, candidate->depthState))
                        updateDeviceDepthStateLocked(pLogicalDevice, candidate->depthState, "QueuePresentKHR(depth-source-auto)", true);
                }
                else
                    updateDeviceDepthStateLocked(pLogicalDevice, {}, "QueuePresentKHR(depth-source-auto-empty)", true);
            }
            presentDepthState       = pLogicalDevice->activeDepthState;
            presentCopyBeforeClears = pLogicalDevice->depthCopyBeforeClears;
            presentDepthFallback    = pLogicalDevice->activeDepthStatePresentTimeFallback;
            presentDepthFresh =
                depthFrameHadWork
                && std::any_of(pLogicalDevice->depthSourceCandidates.begin(),
                               pLogicalDevice->depthSourceCandidates.end(),
                               [&](const auto& candidate) { return !candidate.stale && sameDepthSource(candidate.depthState, presentDepthState); });
            if (pLogicalDevice->depthCopySourceId != presentDepthState.sourceId)
            {
                pLogicalDevice->depthCopySourceId = presentDepthState.sourceId;
                pLogicalDevice->depthCopyTargets.clear();
                pLogicalDevice->depthCopyFallbackTarget = {};
                pLogicalDevice->automaticClearSelection = {};
            }
            if (presentCopyBeforeClears && presentDepthFresh)
            {
                pLogicalDevice->depthCopyTargets.clear();
                pLogicalDevice->depthCopyFallbackTarget = {};
                for (const auto& candidate : pLogicalDevice->depthSourceCandidates)
                    if (!candidate.stale && sameDepthSource(candidate.depthState, presentDepthState))
                    {
                        const auto clearIndex = pLogicalDevice->depthClearIndex == 0
                                                    ? selectStableAutomaticDepthClear(
                                                          candidate.clears, pLogicalDevice->automaticClearSelection, std::chrono::steady_clock::now())
                                                    : selectDepthClear(candidate.clears, pLogicalDevice->depthClearIndex);
                        if (clearIndex)
                        {
                            const auto& clear                                     = candidate.clears[*clearIndex];
                            pLogicalDevice->depthCopyTargets[clear.commandBuffer] = {candidate.depthState.sourceId, clear.ordinal};
                            pLogicalDevice->depthCopyFallbackTarget               = {candidate.depthState.sourceId, clear.ordinal};
                            presentDepthBackup                                    = clear.backup;
                        }
                        break;
                    }
            }
            updateOverlayState(pLogicalDevice, presentEffect);
            presentEffectSnapshot = presentEffect;
            pLogicalDeviceShared  = devIt->second;

            presentSwapchains.reserve(pPresentInfo->swapchainCount);
            presentIndices.reserve(pPresentInfo->swapchainCount);
            for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
            {
                auto swapIt = swapchainMap.find(pPresentInfo->pSwapchains[i]);
                if (swapIt == swapchainMap.end() || !swapIt->second)
                {
                    Logger::err("present references unknown swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                LogicalSwapchain* pLogicalSwapchain = swapIt->second.get();
                uint32_t          index             = pPresentInfo->pImageIndices[i];
                if (pLogicalSwapchain->passthrough)
                {
                    presentSwapchains.push_back(nullptr);
                    presentIndices.push_back(index);
                    continue;
                }
                if (index >= pLogicalSwapchain->imageCount || index >= pLogicalSwapchain->semaphores.size()
                    || index >= pLogicalSwapchain->overlaySemaphores.size() || index >= pLogicalSwapchain->imageViews.size())
                {
                    Logger::err("present image index out of bounds for swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                const auto& commandBuffers =
                    presentEffectSnapshot ? pLogicalSwapchain->commandBuffersEffect : pLogicalSwapchain->commandBuffersNoEffect;
                if (index >= commandBuffers.size())
                {
                    Logger::err("present command buffer index out of bounds");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                presentSwapchains.push_back(swapIt->second);
                presentIndices.push_back(index);
            }
        }

        LogicalDevice* pLogicalDevice = pLogicalDeviceShared.get();
        if (!pLogicalDevice)
            return VK_ERROR_DEVICE_LOST;

        // Reuse static buffers to avoid per-frame heap allocations
        static thread_local std::vector<VkSemaphore>          presentSemaphores;
        static thread_local std::vector<VkPipelineStageFlags> waitStages;
        presentSemaphores.clear();
        presentSemaphores.reserve(pPresentInfo->swapchainCount);
        waitStages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        bool waitedForApplication = false;
        for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
        {
            LogicalSwapchain* pLogicalSwapchain = presentSwapchains[i].get();
            if (!pLogicalSwapchain)
                continue;
            uint32_t index = presentIndices[i];

            // Submit effect command buffer
            VkSubmitInfo submitInfo             = {};
            submitInfo.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.waitSemaphoreCount       = !waitedForApplication ? pPresentInfo->waitSemaphoreCount : 0;
            submitInfo.pWaitSemaphores          = !waitedForApplication ? pPresentInfo->pWaitSemaphores : nullptr;
            submitInfo.pWaitDstStageMask        = !waitedForApplication ? waitStages.data() : nullptr;
            waitedForApplication                = true;
            VkCommandBuffer submittedBuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            VkFence         copyFence           = VK_NULL_HANDLE;
            VkResult        vr;
            {
                scoped_lock l(globalLock);
                const auto  swapchain = swapchainMap.find(pPresentInfo->pSwapchains[i]);
                if (swapchain == swapchainMap.end() || swapchain->second.get() != pLogicalSwapchain)
                    return VK_ERROR_OUT_OF_DATE_KHR;
                bool       depthReady = false;
                const auto refreshResult =
                    refreshDepthResourcesForPresent(pLogicalDevice, pLogicalSwapchain, pLogicalDevice->activeDepthState, depthReady);
                if (refreshResult != VK_SUCCESS)
                    return refreshResult;
                const bool stateMatches = sameDepthState(pLogicalDevice->activeDepthState, presentDepthState)
                                          && pLogicalDevice->depthCopyBeforeClears == presentCopyBeforeClears;
                const bool captureCurrent =
                    depthReady && stateMatches && presentDepthFresh
                    && ((presentCopyBeforeClears && presentEffectSnapshot) || (!presentCopyBeforeClears && presentDepthFallback))
                    && hasDepthState(presentDepthState) && index < pLogicalSwapchain->depthResolveImages.size();
                std::optional<uint32_t> previousImage;
                const auto              latest = pLogicalSwapchain->depthResolveLatest;
                if (!captureCurrent && depthReady && stateMatches && presentEffectSnapshot && hasDepthState(presentDepthState) && latest
                    && *latest != index && *latest < pLogicalSwapchain->depthResolveVersions.size()
                    && index < pLogicalSwapchain->depthResolveVersions.size() && pLogicalSwapchain->depthResolveVersions[*latest] != 0
                    && pLogicalSwapchain->depthResolveVersions[index] != pLogicalSwapchain->depthResolveVersions[*latest])
                    previousImage = latest;
                const bool recordCopy = captureCurrent || previousImage.has_value();
                const bool hasDepthOutput =
                    !hasDepthState(presentDepthState) || recordCopy
                    || (index < pLogicalSwapchain->depthResolveVersions.size() && pLogicalSwapchain->depthResolveVersions[index] != 0);
                const bool useEffects = presentEffectSnapshot && depthReady && stateMatches && hasDepthOutput;
                if (useEffects)
                    for (auto& effect : pLogicalSwapchain->effects)
                        effect->updateEffect();
                const auto& commandBuffers = useEffects ? pLogicalSwapchain->commandBuffersEffect : pLogicalSwapchain->commandBuffersNoEffect;
                if (index >= commandBuffers.size() || index >= pLogicalSwapchain->semaphores.size())
                    return VK_ERROR_OUT_OF_DATE_KHR;
                submittedBuffers[1]   = commandBuffers[index];
                const auto copyResult = prepareDepthCopySubmission(
                    pLogicalDevice, pLogicalSwapchain, index, presentDepthState, presentDepthBackup, recordCopy, previousImage);
                if (copyResult != VK_SUCCESS)
                    return copyResult;
                auto& copy                      = pLogicalSwapchain->depthCopySubmissions[index];
                submittedBuffers[0]             = recordCopy ? copy.commandBuffer : VK_NULL_HANDLE;
                copyFence                       = copy.fence;
                submitInfo.commandBufferCount   = recordCopy ? 2 : 1;
                submitInfo.pCommandBuffers      = recordCopy ? submittedBuffers : &submittedBuffers[1];
                submitInfo.signalSemaphoreCount = 1;
                submitInfo.pSignalSemaphores    = &pLogicalSwapchain->semaphores[index];
                vr                              = pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &submitInfo, copyFence);
                if (vr == VK_SUCCESS)
                {
                    copy.pending         = true;
                    copy.referencesDepth = useEffects || recordCopy;
                    if (captureCurrent)
                    {
                        pLogicalSwapchain->depthResolveVersions[index] = ++pLogicalSwapchain->depthResolveVersion;
                        pLogicalSwapchain->depthResolveLatest          = index;
                    }
                    else if (previousImage)
                        pLogicalSwapchain->depthResolveVersions[index] = pLogicalSwapchain->depthResolveVersions[*previousImage];
                }
            }
            if (vr != VK_SUCCESS)
            {
                reportDeviceLostDiagnostics(pLogicalDevice, pLogicalDevice->queue, "vkQueueSubmit(effect)", vr);
                return vr;
            }
            maybeDumpDepthResolveImage(pLogicalDevice, pLogicalSwapchain, index, pLogicalDevice->queue);

            VkSemaphore finalSemaphore;
            vr = submitOverlayFrame(pLogicalDevice, pLogicalSwapchain, index, finalSemaphore);
            if (vr != VK_SUCCESS)
            {
                reportDeviceLostDiagnostics(pLogicalDevice, pLogicalDevice->queue, "submitOverlayFrame", vr);
                return vr;
            }

            presentSemaphores.push_back(finalSemaphore);
        }

        VkPresentInfoKHR presentInfo   = *pPresentInfo;
        presentInfo.waitSemaphoreCount = presentSemaphores.size();
        presentInfo.pWaitSemaphores    = presentSemaphores.data();

        VkResult presentResult = pLogicalDevice->vkd.QueuePresentKHR(queue, &presentInfo);
        reportDeviceLostDiagnostics(pLogicalDevice, queue, "vkQueuePresentKHR(final)", presentResult);
        {
            scoped_lock l(globalLock);
            pLogicalDevice->frameDepthCandidates.clear();
            pLogicalDevice->bestDepthCandidate = {};
        }
        return presentResult;
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
    {
        if (!swapchain)
            return;

        scoped_lock l(globalLock);
        // we need to delete the infos of the oldswapchain

        Logger::trace("vkDestroySwapchainKHR " + convertToString(swapchain));
        swapchainMap[swapchain]->destroy();
        swapchainMap.erase(swapchain);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        pLogicalDevice->vkd.DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateRenderPass(VkDevice                      device,
                                                            const VkRenderPassCreateInfo* pCreateInfo,
                                                            const VkAllocationCallbacks*  pAllocator,
                                                            VkRenderPass*                 pRenderPass)
    {
        scoped_lock    l(globalLock);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!vkShade::settingsManager.isDepthCaptureActive() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
            return pLogicalDevice->vkd.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);

        std::vector<VkAttachmentDescription> attachments(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
        {
            VkResult result = pLogicalDevice->vkd.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
            if (result == VK_SUCCESS)
                trackRenderPassMetadata(pLogicalDevice, *pRenderPass, pCreateInfo);
            return result;
        }

        VkRenderPassCreateInfo createInfo = *pCreateInfo;
        createInfo.pAttachments           = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        VkResult result = pLogicalDevice->vkd.CreateRenderPass(device, &createInfo, pAllocator, pRenderPass);
        if (result == VK_SUCCESS)
            trackRenderPassMetadata(pLogicalDevice, *pRenderPass, pCreateInfo);
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateRenderPass2(VkDevice                       device,
                                                             const VkRenderPassCreateInfo2* pCreateInfo,
                                                             const VkAllocationCallbacks*   pAllocator,
                                                             VkRenderPass*                  pRenderPass)
    {
        scoped_lock    l(globalLock);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!vkShade::settingsManager.isDepthCaptureActive() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
            return pLogicalDevice->vkd.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);

        std::vector<VkAttachmentDescription2> attachments(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
        {
            VkResult result = pLogicalDevice->vkd.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
            if (result == VK_SUCCESS)
                trackRenderPassMetadata(pLogicalDevice, *pRenderPass, pCreateInfo);
            return result;
        }

        VkRenderPassCreateInfo2 createInfo = *pCreateInfo;
        createInfo.pAttachments            = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo2 with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        VkResult result = pLogicalDevice->vkd.CreateRenderPass2(device, &createInfo, pAllocator, pRenderPass);
        if (result == VK_SUCCESS)
            trackRenderPassMetadata(pLogicalDevice, *pRenderPass, pCreateInfo);
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateRenderPass2KHR(VkDevice                       device,
                                                                const VkRenderPassCreateInfo2* pCreateInfo,
                                                                const VkAllocationCallbacks*   pAllocator,
                                                                VkRenderPass*                  pRenderPass)
    {
        scoped_lock    l(globalLock);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!vkShade::settingsManager.isDepthCaptureActive() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
            return pLogicalDevice->vkd.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);

        std::vector<VkAttachmentDescription2> attachments(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
        {
            VkResult result = pLogicalDevice->vkd.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
            if (result == VK_SUCCESS)
                trackRenderPassMetadata(pLogicalDevice, *pRenderPass, pCreateInfo);
            return result;
        }

        VkRenderPassCreateInfo2 createInfo = *pCreateInfo;
        createInfo.pAttachments            = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo2KHR with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        VkResult result = pLogicalDevice->vkd.CreateRenderPass2KHR(device, &createInfo, pAllocator, pRenderPass);
        if (result == VK_SUCCESS)
            trackRenderPassMetadata(pLogicalDevice, *pRenderPass, pCreateInfo);
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_DestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator)
    {
        if (!renderPass)
            return;

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        pLogicalDevice->renderPassMetadata.erase(renderPass);
        pLogicalDevice->vkd.DestroyRenderPass(device, renderPass, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateImage(VkDevice                     device,
                                                       const VkImageCreateInfo*     pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkImage*                     pImage)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock    l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        const auto     created        = createDepthCaptureImage(pLogicalDevice->physicalDevice,
                                                                device,
                                                                *pCreateInfo,
                                                                pAllocator,
                                                                pImage,
                                                                pLogicalDevice->vki.GetPhysicalDeviceImageFormatProperties,
                                                                pLogicalDevice->vkd.CreateImage);
        if (created.result != VK_SUCCESS || !isDepthFormat(pCreateInfo->format)
            || !(pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
            return created.result;

        DepthImageMetadata metadata = {
            pCreateInfo->usage,
            pCreateInfo->samples,
            pCreateInfo->tiling,
        };
        metadata.sourceId    = ++pLogicalDevice->nextDepthSourceId;
        metadata.mipLevels   = pCreateInfo->mipLevels;
        metadata.arrayLayers = pCreateInfo->arrayLayers;
        metadata.capturable  = created.captured && pCreateInfo->imageType == VK_IMAGE_TYPE_2D;
        if (created.captured)
            metadata.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        pLogicalDevice->depthImages.push_back(*pImage);
        pLogicalDevice->depthFormats.push_back(pCreateInfo->format);
        pLogicalDevice->depthImageExtents[*pImage]  = pCreateInfo->extent;
        pLogicalDevice->depthImageMetadata[*pImage] = metadata;
        Logger::debug("tracked depth image metadata: image=" + convertToString(*pImage) + " usage=0x"
                      + formatHexU64(static_cast<uint64_t>(metadata.usage)) + " samples=" + convertToString(metadata.samples)
                      + " tiling=" + convertToString(metadata.tiling));
        return created.result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock    l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.BindImageMemory(device, image, memory, memoryOffset);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        VkResult       result         = pLogicalDevice->vkd.BindImageMemory(device, image, memory, memoryOffset);

        auto it = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), image);
        if (it == pLogicalDevice->depthImages.end())
            return result;

        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateImageView(VkDevice                     device,
                                                           const VkImageViewCreateInfo* pCreateInfo,
                                                           const VkAllocationCallbacks* pAllocator,
                                                           VkImageView*                 pView)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock    l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, pView);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        VkResult       result         = pLogicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, pView);
        if (result != VK_SUCCESS)
            return result;

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImage(pLogicalDevice, pCreateInfo->image);
        if ((pCreateInfo->subresourceRange.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0)
        {
            // [DEBUG-TAG: swapchain-snapshot-probe-20260424]
            static uint32_t colorImageViewProbeCount = 0;
            if (colorImageViewProbeCount++ < 128)
            {
                Logger::debug("swapchain snapshot probe image view: appView=" + convertToString(*pView)
                              + " image=" + convertToString(pCreateInfo->image) + " format=" + std::to_string(pCreateInfo->format) + " aspect=0x"
                              + formatHexU64(static_cast<uint64_t>(pCreateInfo->subresourceRange.aspectMask))
                              + " matched=" + std::string(snapshotTarget.swapchain != VK_NULL_HANDLE ? "true" : "false") + " swapchain="
                              + convertToString(snapshotTarget.swapchain) + " imageIndex=" + std::to_string(snapshotTarget.imageIndex));
            }
        }
        if (snapshotTarget.swapchain != VK_NULL_HANDLE)
        {
            Logger::debug("tracked snapshot target image view: appView=" + convertToString(*pView) + " image=" + convertToString(pCreateInfo->image)
                          + " swapchain=" + convertToString(snapshotTarget.swapchain) + " imageIndex=" + std::to_string(snapshotTarget.imageIndex));
            pLogicalDevice->snapshotTargetViewStates[*pView] = snapshotTarget;
        }

        if ((pCreateInfo->subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0)
            return result;

        auto imageIt = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), pCreateInfo->image);
        if (imageIt == pLogicalDevice->depthImages.end())
            return result;

        size_t      i           = std::distance(pLogicalDevice->depthImages.begin(), imageIt);
        const auto& metadata    = pLogicalDevice->depthImageMetadata.at(pCreateInfo->image);
        const auto& range       = pCreateInfo->subresourceRange;
        const bool  singleMip   = range.levelCount == 1 || (range.levelCount == VK_REMAINING_MIP_LEVELS && metadata.mipLevels == 1);
        const bool  singleLayer = range.layerCount == 1 || (range.layerCount == VK_REMAINING_ARRAY_LAYERS && metadata.arrayLayers == 1);
        const bool  capturable  = metadata.capturable && range.baseMipLevel == 0 && range.baseArrayLayer == 0 && singleMip && singleLayer;
        VkImageView sampledView =
            capturable ? getOrCreateTrackedDepthSampleViewLocked(pLogicalDevice, pCreateInfo->image, pLogicalDevice->depthFormats[i]) : *pView;
        Logger::debug("tracked depth image view created: appView=" + convertToString(*pView) + " sampledView=" + convertToString(sampledView)
                      + " image=" + convertToString(pCreateInfo->image) + " aspect=" + convertToString(pCreateInfo->subresourceRange.aspectMask));
        DepthState depth;
        depth.image      = pCreateInfo->image;
        depth.imageView  = sampledView;
        depth.sourceId   = pLogicalDevice->depthImageMetadata.at(depth.image).sourceId;
        depth.samples    = metadata.samples;
        depth.capturable = capturable && sampledView != VK_NULL_HANDLE;
        depth.format     = pLogicalDevice->depthFormats[i];
        auto extentIt    = pLogicalDevice->depthImageExtents.find(depth.image);
        if (extentIt != pLogicalDevice->depthImageExtents.end())
            depth.extent = extentIt->second;
        pLogicalDevice->depthViewStates[*pView] = depth;

        return result;
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_DestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator)
    {
        if (!imageView)
            return;

        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock    l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            pLogicalDevice->vkd.DestroyImageView(device, imageView, pAllocator);
            return;
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        pLogicalDevice->snapshotTargetViewStates.erase(imageView);
        pLogicalDevice->depthViewStates.erase(imageView);
        pLogicalDevice->depthFrameTracker.forgetView(imageView);
        clearTrackedDepthScopesLocked(pLogicalDevice, [imageView](const DepthState& state) { return state.imageView == imageView; }, true);

        for (auto it = pLogicalDevice->framebufferDepthStates.begin(); it != pLogicalDevice->framebufferDepthStates.end();)
        {
            if (it->second.imageView == imageView)
                it = pLogicalDevice->framebufferDepthStates.erase(it);
            else
                ++it;
        }

        if (pLogicalDevice->activeDepthState.imageView == imageView)
            updateDeviceDepthStateLocked(pLogicalDevice, {}, "DestroyImageView");

        pLogicalDevice->vkd.DestroyImageView(device, imageView, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateFramebuffer(VkDevice                       device,
                                                             const VkFramebufferCreateInfo* pCreateInfo,
                                                             const VkAllocationCallbacks*   pAllocator,
                                                             VkFramebuffer*                 pFramebuffer)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock    l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        VkResult       result         = pLogicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        if (result != VK_SUCCESS)
            return result;

        if (pCreateInfo->pAttachments)
            pLogicalDevice->framebufferAttachments[*pFramebuffer] =
                std::vector<VkImageView>(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        DepthState depthState;
        if (selectDepthStateFromAttachments(
                pLogicalDevice, pCreateInfo->renderPass, pCreateInfo->pAttachments, pCreateInfo->attachmentCount, depthState))
        {
            Logger::debug("tracked depth framebuffer attachment: framebuffer=" + convertToString(*pFramebuffer)
                          + " sampledView=" + convertToString(depthState.imageView) + " image=" + convertToString(depthState.image)
                          + " observedLayout=" + convertToString(depthState.observedLayout));
            pLogicalDevice->framebufferDepthStates[*pFramebuffer] = depthState;
        }
        else if (findRenderPassMetadata(pLogicalDevice, pCreateInfo->renderPass) == nullptr)
        {
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++)
            {
                auto it = pLogicalDevice->depthViewStates.find(pCreateInfo->pAttachments[i]);
                if (it != pLogicalDevice->depthViewStates.end())
                {
                    Logger::debug("tracked depth framebuffer attachment: framebuffer=" + convertToString(*pFramebuffer)
                                  + " attachmentView=" + convertToString(pCreateInfo->pAttachments[i])
                                  + " sampledView=" + convertToString(it->second.imageView) + " image=" + convertToString(it->second.image));
                    pLogicalDevice->framebufferDepthStates[*pFramebuffer] = it->second;
                    break;
                }
            }
        }

        DepthSnapshotTarget snapshotTarget =
            selectDepthSnapshotTargetFromImageViews(pLogicalDevice, pCreateInfo->pAttachments, pCreateInfo->attachmentCount);
        if (snapshotTarget.swapchain != VK_NULL_HANDLE)
        {
            Logger::debug("tracked framebuffer snapshot target: framebuffer=" + convertToString(*pFramebuffer)
                          + " swapchain=" + convertToString(snapshotTarget.swapchain) + " imageIndex=" + std::to_string(snapshotTarget.imageIndex));
            pLogicalDevice->framebufferSnapshotTargets[*pFramebuffer] = snapshotTarget;
        }
        else
        {
            Logger::debug("framebuffer has no snapshot target: framebuffer=" + convertToString(*pFramebuffer)
                          + " attachments=" + std::to_string(pCreateInfo->attachmentCount));
        }

        return result;
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator)
    {
        if (!framebuffer)
            return;

        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock    l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            pLogicalDevice->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
            return;
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        pLogicalDevice->framebufferDepthStates.erase(framebuffer);
        pLogicalDevice->framebufferAttachments.erase(framebuffer);
        pLogicalDevice->framebufferSnapshotTargets.erase(framebuffer);
        pLogicalDevice->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
    }

    static void
    trackDepthClear(LogicalDevice* device, VkCommandBuffer commandBuffer, const DepthState& depth, DepthClearKind kind, bool outsideRenderPass)
    {
        const auto level = device->commandBufferLevels.find(commandBuffer);
        const auto pool  = device->commandBufferPools.find(commandBuffer);
        const auto usage = device->commandBufferUsage.find(commandBuffer);
        const auto family =
            pool == device->commandBufferPools.end() ? device->commandPoolFamilies.end() : device->commandPoolFamilies.find(pool->second);
        const bool singleUse = usage != device->commandBufferUsage.end() && (usage->second & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
                               && !(usage->second & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
        const bool possible  = outsideRenderPass && canCopyDepthBeforeClear(depth) && level != device->commandBufferLevels.end()
                               && level->second == VK_COMMAND_BUFFER_LEVEL_PRIMARY && (singleUse || device->singleGraphicsQueue)
                               && family != device->commandPoolFamilies.end() && family->second.family == device->queueFamilyIndex
                               && !(family->second.flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT);
        std::shared_ptr<DepthBackup> backup;
        const auto                   target   = device->depthCopyTargets.find(commandBuffer);
        const auto&                  selected = target == device->depthCopyTargets.end() ? device->depthCopyFallbackTarget : target->second;
        if (possible && (singleUse || device->presentUsesGraphicsQueue) && device->depthCopyBeforeClears && selected.sourceId != 0
            && selected.sourceId == depth.sourceId && selected.ordinal == device->depthFrameTracker.nextClearOrdinal(commandBuffer, depth.sourceId))
        {
            const auto reusable = std::find_if(device->depthBackupPool.begin(), device->depthBackupPool.end(), [&depth](const auto& item) {
                return item.use_count() == 1 && item->format == depth.format && item->extent.width == depth.extent.width
                       && item->extent.height == depth.extent.height;
            });
            if (reusable != device->depthBackupPool.end())
                backup = *reusable;
            else
            {
                backup = createDepthBackup(device, depth);
                if (backup)
                    device->depthBackupPool.push_back(backup);
            }
            if (backup)
                recordDepthBackup(device, commandBuffer, depth, *backup);
        }
        device->depthFrameTracker.clear(commandBuffer, depth, kind, possible, std::move(backup), !singleUse);
    }

    static void trackRenderSubpass(LogicalDevice* device, VkCommandBuffer commandBuffer, LogicalDevice::RenderPassTrackingState& pass)
    {
        const auto* metadata = findRenderPassMetadata(device, pass.renderPass);
        if (!metadata)
            return;
        for (size_t i = 0; i < pass.attachments.size() && i < metadata->attachmentFirstUse.size(); ++i)
            if (pass.attachments[i].image != VK_NULL_HANDLE && metadata->attachmentFirstUse[i] == pass.subpass
                && metadata->attachmentLoadOps[i] == VK_ATTACHMENT_LOAD_OP_CLEAR)
            {
                auto beforeClear           = pass.attachments[i];
                beforeClear.observedLayout = metadata->attachmentInitialLayouts[i];
                trackDepthClear(device, commandBuffer, beforeClear, DepthClearKind::RenderPassLoad, pass.subpass == 0);
            }
        DepthState depth;
        if (pass.subpass < metadata->subpassDepthAttachments.size())
        {
            const auto index = metadata->subpassDepthAttachments[pass.subpass];
            if (index < pass.attachments.size())
            {
                depth                = pass.attachments[index];
                depth.observedLayout = metadata->attachmentFinalLayouts[index];
            }
        }
        beginTrackedDepthScope(device, commandBuffer, depth, pass.target);
    }

    static void seedTrackedDepthScopeFromRenderPass(LogicalDevice* device, VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* begin)
    {
        const auto* metadata = begin ? findRenderPassMetadata(device, begin->renderPass) : nullptr;
        const auto  target   = selectDepthSnapshotTargetFromRenderPassBegin(device, begin);
        if (!metadata)
        {
            beginTrackedDepthScope(device, commandBuffer, selectDepthStateFromRenderPassBegin(device, begin), target);
            return;
        }
        const VkImageView* views     = nullptr;
        size_t             viewCount = 0;
        for (auto* next = static_cast<const VkBaseInStructure*>(begin->pNext); next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
            {
                const auto* attachmentInfo = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);
                views                      = attachmentInfo->pAttachments;
                viewCount                  = attachmentInfo->attachmentCount;
                break;
            }
        if (!views)
        {
            const auto framebuffer = device->framebufferAttachments.find(begin->framebuffer);
            if (framebuffer != device->framebufferAttachments.end())
            {
                views     = framebuffer->second.data();
                viewCount = framebuffer->second.size();
            }
        }
        auto& pass      = device->commandBufferRenderPasses[commandBuffer];
        pass.renderPass = begin->renderPass;
        pass.subpass    = 0;
        pass.target     = target;
        pass.attachments.assign(metadata->attachmentLoadOps.size(), {});
        for (size_t i = 0; i < viewCount && i < pass.attachments.size(); ++i)
        {
            const auto state = device->depthViewStates.find(views[i]);
            if (state != device->depthViewStates.end())
                pass.attachments[i] = state->second;
        }
        trackRenderSubpass(device, commandBuffer, pass);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderPass(VkCommandBuffer              commandBuffer,
                                                          const VkRenderPassBeginInfo* pRenderPassBegin,
                                                          VkSubpassContents            contents)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                seedTrackedDepthScopeFromRenderPass(pLogicalDevice, commandBuffer, pRenderPassBegin);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderPass2(VkCommandBuffer              commandBuffer,
                                                           const VkRenderPassBeginInfo* pRenderPassBegin,
                                                           const VkSubpassBeginInfo*    pSubpassBeginInfo)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                seedTrackedDepthScopeFromRenderPass(pLogicalDevice, commandBuffer, pRenderPassBegin);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderPass2KHR(VkCommandBuffer              commandBuffer,
                                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                                              const VkSubpassBeginInfo*    pSubpassBeginInfo)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                seedTrackedDepthScopeFromRenderPass(pLogicalDevice, commandBuffer, pRenderPassBegin);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    void seedTrackedDepthScopeFromRenderingInfo(LogicalDevice* pLogicalDevice, VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        const auto depth = selectDepthStateFromRenderingInfo(pLogicalDevice, pRenderingInfo);
        if (pRenderingInfo && !(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT) && pRenderingInfo->pDepthAttachment
            && pRenderingInfo->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
            trackDepthClear(pLogicalDevice, commandBuffer, depth, DepthClearKind::DynamicRenderingLoad, true);
        beginTrackedDepthScope(pLogicalDevice, commandBuffer, depth, selectDepthSnapshotTargetFromRenderingInfo(pLogicalDevice, pRenderingInfo));
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRendering(commandBuffer, pRenderingInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                seedTrackedDepthScopeFromRenderingInfo(pLogicalDevice, commandBuffer, pRenderingInfo);
            }
        }

        if (pLogicalDevice)
        {
            if (pRenderingInfo)
            {
                VkRenderingInfo           renderingInfo     = *pRenderingInfo;
                VkRenderingAttachmentInfo depthAttachment   = {};
                VkRenderingAttachmentInfo stencilAttachment = {};
                bool                      changed           = false;

                if (pRenderingInfo->pDepthAttachment)
                {
                    depthAttachment                = *pRenderingInfo->pDepthAttachment;
                    changed                        = forceDepthAttachmentStoreOp(depthAttachment, false) || changed;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                }

                if (pRenderingInfo->pStencilAttachment)
                {
                    stencilAttachment                = *pRenderingInfo->pStencilAttachment;
                    changed                          = forceDepthAttachmentStoreOp(stencilAttachment, true) || changed;
                    renderingInfo.pStencilAttachment = &stencilAttachment;
                }

                if (changed)
                {
                    Logger::debug("forcing depth attachment storeOp=STORE for VkRenderingInfo on commandBuffer=" + convertToString(commandBuffer));
                    pLogicalDevice->vkd.CmdBeginRendering(commandBuffer, &renderingInfo);
                    return;
                }
            }

            pLogicalDevice->vkd.CmdBeginRendering(commandBuffer, pRenderingInfo);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                seedTrackedDepthScopeFromRenderingInfo(pLogicalDevice, commandBuffer, pRenderingInfo);
            }
        }

        if (pLogicalDevice)
        {
            if (pRenderingInfo)
            {
                VkRenderingInfo           renderingInfo     = *pRenderingInfo;
                VkRenderingAttachmentInfo depthAttachment   = {};
                VkRenderingAttachmentInfo stencilAttachment = {};
                bool                      changed           = false;

                if (pRenderingInfo->pDepthAttachment)
                {
                    depthAttachment                = *pRenderingInfo->pDepthAttachment;
                    changed                        = forceDepthAttachmentStoreOp(depthAttachment, false) || changed;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                }

                if (pRenderingInfo->pStencilAttachment)
                {
                    stencilAttachment                = *pRenderingInfo->pStencilAttachment;
                    changed                          = forceDepthAttachmentStoreOp(stencilAttachment, true) || changed;
                    renderingInfo.pStencilAttachment = &stencilAttachment;
                }

                if (changed)
                {
                    Logger::debug("forcing depth attachment storeOp=STORE for VkRenderingInfoKHR on commandBuffer=" + convertToString(commandBuffer));
                    pLogicalDevice->vkd.CmdBeginRenderingKHR(commandBuffer, &renderingInfo);
                    return;
                }
            }

            pLogicalDevice->vkd.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdClearAttachments(
        VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment* attachments, uint32_t rectCount, const VkClearRect* rects)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(commandBuffer));
        if (device == deviceMap.end() || !device->second)
            return;
        auto* logicalDevice = device->second.get();
        if (settingsManager.isDepthCaptureActive() && attachments && rects && rectCount)
        {
            const auto scope     = logicalDevice->commandBufferDepthStates.find(commandBuffer);
            const auto level     = logicalDevice->commandBufferLevels.find(commandBuffer);
            const bool inherited = level != logicalDevice->commandBufferLevels.end() && level->second == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            if ((scope != logicalDevice->commandBufferDepthStates.end() && scope->second.inRenderScope) || inherited)
                for (uint32_t i = 0; i < attachmentCount; ++i)
                    if (attachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
                    {
                        const auto depth = scope != logicalDevice->commandBufferDepthStates.end() && scope->second.inRenderScope
                                               ? scope->second.depthState
                                               : DepthState{};
                        trackDepthClear(logicalDevice, commandBuffer, depth, DepthClearKind::Attachment, false);
                        break;
                    }
        }
        logicalDevice->vkd.CmdClearAttachments(commandBuffer, attachmentCount, attachments, rectCount, rects);
    }

    template<typename ImageBarrier>
    static void trackDepthLayouts(LogicalDevice* device, VkCommandBuffer commandBuffer, uint32_t count, const ImageBarrier* barriers)
    {
        if (!settingsManager.isDepthCaptureActive() || !barriers)
            return;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto& barrier = barriers[i];
            if (!(barrier.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) || barrier.subresourceRange.baseMipLevel != 0
                || barrier.subresourceRange.baseArrayLayer != 0 || barrier.oldLayout == barrier.newLayout)
                continue;
            const auto metadata = device->depthImageMetadata.find(barrier.image);
            if (metadata == device->depthImageMetadata.end())
                continue;
            DepthState depth;
            depth.image          = barrier.image;
            depth.sourceId       = metadata->second.sourceId;
            depth.observedLayout = barrier.newLayout;
            device->depthFrameTracker.layout(commandBuffer, depth);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdPipelineBarrier(VkCommandBuffer              commandBuffer,
                                                          VkPipelineStageFlags         srcStageMask,
                                                          VkPipelineStageFlags         dstStageMask,
                                                          VkDependencyFlags            dependencyFlags,
                                                          uint32_t                     memoryBarrierCount,
                                                          const VkMemoryBarrier*       memoryBarriers,
                                                          uint32_t                     bufferBarrierCount,
                                                          const VkBufferMemoryBarrier* bufferBarriers,
                                                          uint32_t                     imageBarrierCount,
                                                          const VkImageMemoryBarrier*  imageBarriers)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(commandBuffer));
        if (device == deviceMap.end() || !device->second)
            return;
        auto* logicalDevice = device->second.get();
        trackDepthLayouts(logicalDevice, commandBuffer, imageBarrierCount, imageBarriers);
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                              srcStageMask,
                                              dstStageMask,
                                              dependencyFlags,
                                              memoryBarrierCount,
                                              memoryBarriers,
                                              bufferBarrierCount,
                                              bufferBarriers,
                                              imageBarrierCount,
                                              imageBarriers);
    }

    static void cmdTrackedPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* dependency, bool khr)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(commandBuffer));
        if (device == deviceMap.end() || !device->second)
            return;
        auto* logicalDevice = device->second.get();
        if (dependency)
            trackDepthLayouts(logicalDevice, commandBuffer, dependency->imageMemoryBarrierCount, dependency->pImageMemoryBarriers);
        const auto barrier = khr ? logicalDevice->vkd.CmdPipelineBarrier2KHR : logicalDevice->vkd.CmdPipelineBarrier2;
        if (barrier)
            barrier(commandBuffer, dependency);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* dependency)
    {
        cmdTrackedPipelineBarrier2(commandBuffer, dependency, false);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer, const VkDependencyInfoKHR* dependency)
    {
        cmdTrackedPipelineBarrier2(commandBuffer, dependency, true);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdClearDepthStencilImage(VkCommandBuffer                 commandBuffer,
                                                                 VkImage                         image,
                                                                 VkImageLayout                   imageLayout,
                                                                 const VkClearDepthStencilValue* depthStencil,
                                                                 uint32_t                        rangeCount,
                                                                 const VkImageSubresourceRange*  ranges)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(commandBuffer));
        if (device == deviceMap.end() || !device->second)
            return;
        auto* logicalDevice = device->second.get();
        if (settingsManager.isDepthCaptureActive() && ranges)
        {
            bool clearsDepth = false;
            for (uint32_t i = 0; i < rangeCount; ++i)
                clearsDepth |=
                    (ranges[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 && ranges[i].baseMipLevel == 0 && ranges[i].baseArrayLayer == 0;
            const auto metadata = logicalDevice->depthImageMetadata.find(image);
            const auto imageIt  = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), image);
            if (clearsDepth && metadata != logicalDevice->depthImageMetadata.end() && imageIt != logicalDevice->depthImages.end())
            {
                const auto index = static_cast<size_t>(std::distance(logicalDevice->depthImages.begin(), imageIt));
                DepthState depth;
                depth.image          = image;
                depth.sourceId       = metadata->second.sourceId;
                depth.samples        = metadata->second.samples;
                depth.capturable     = metadata->second.capturable;
                depth.format         = logicalDevice->depthFormats[index];
                depth.extent         = logicalDevice->depthImageExtents[image];
                depth.observedLayout = imageLayout;
                for (const auto& entry : logicalDevice->depthViewStates)
                    if (entry.second.image == image)
                    {
                        depth                = entry.second;
                        depth.observedLayout = imageLayout;
                        break;
                    }
                trackDepthClear(logicalDevice, commandBuffer, depth, DepthClearKind::Image, true);
            }
        }
        logicalDevice->vkd.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, depthStencil, rangeCount, ranges);
    }

    static void advanceTrackedRenderPass(LogicalDevice* device, VkCommandBuffer commandBuffer)
    {
        const auto pass = device->commandBufferRenderPasses.find(commandBuffer);
        if (pass == device->commandBufferRenderPasses.end())
            return;
        endTrackedDepthScope(device, commandBuffer, "CmdNextSubpass");
        ++pass->second.subpass;
        trackRenderSubpass(device, commandBuffer, pass->second);
    }

    static void finishTrackedRenderPass(LogicalDevice* device, VkCommandBuffer commandBuffer)
    {
        const auto pass = device->commandBufferRenderPasses.find(commandBuffer);
        if (pass == device->commandBufferRenderPasses.end())
            return;
        const auto* metadata = findRenderPassMetadata(device, pass->second.renderPass);
        if (metadata)
            for (size_t i = 0; i < pass->second.attachments.size() && i < metadata->attachmentFirstUse.size(); ++i)
                if (pass->second.attachments[i].image != VK_NULL_HANDLE && metadata->attachmentFirstUse[i] != VK_ATTACHMENT_UNUSED)
                {
                    auto depth           = pass->second.attachments[i];
                    depth.observedLayout = metadata->attachmentFinalLayouts[i];
                    device->depthFrameTracker.layout(commandBuffer, depth);
                }
        pass->second.renderPass = VK_NULL_HANDLE;
        pass->second.attachments.clear();
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(commandBuffer));
        if (device == deviceMap.end() || !device->second)
            return;
        if (settingsManager.isDepthCaptureActive())
            advanceTrackedRenderPass(device->second.get(), commandBuffer);
        device->second->vkd.CmdNextSubpass(commandBuffer, contents);
    }

    static void cmdTrackedNextSubpass2(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo* begin, const VkSubpassEndInfo* end, bool khr)
    {
        scoped_lock l(globalLock);
        const auto  device = deviceMap.find(GetKey(commandBuffer));
        if (device == deviceMap.end() || !device->second)
            return;
        if (settingsManager.isDepthCaptureActive())
            advanceTrackedRenderPass(device->second.get(), commandBuffer);
        const auto next = khr ? device->second->vkd.CmdNextSubpass2KHR : device->second->vkd.CmdNextSubpass2;
        if (next)
            next(commandBuffer, begin, end);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdNextSubpass2(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo* begin, const VkSubpassEndInfo* end)
    {
        cmdTrackedNextSubpass2(commandBuffer, begin, end, false);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdNextSubpass2KHR(VkCommandBuffer              commandBuffer,
                                                          const VkSubpassBeginInfoKHR* begin,
                                                          const VkSubpassEndInfoKHR*   end)
    {
        cmdTrackedNextSubpass2(commandBuffer, begin, end, true);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass(commandBuffer);
            return;
        }

        LogicalDevice*      pLogicalDevice          = nullptr;
        DepthSnapshotTarget colorOnlySnapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice, commandBuffer, "CmdEndRenderPass");
                finishTrackedRenderPass(pLogicalDevice, commandBuffer);
                colorOnlySnapshotTarget = takeColorOnlySnapshotScope(pLogicalDevice, commandBuffer);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);
            if (colorOnlySnapshotTarget.swapchain != VK_NULL_HANDLE)
                tryActivatePendingDepthScopeForSnapshotTarget(pLogicalDevice, commandBuffer, colorOnlySnapshotTarget, "CmdEndRenderPass(color)");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
            return;
        }

        LogicalDevice*      pLogicalDevice          = nullptr;
        DepthSnapshotTarget colorOnlySnapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice, commandBuffer, "CmdEndRenderPass2");
                finishTrackedRenderPass(pLogicalDevice, commandBuffer);
                colorOnlySnapshotTarget = takeColorOnlySnapshotScope(pLogicalDevice, commandBuffer);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
            if (colorOnlySnapshotTarget.swapchain != VK_NULL_HANDLE)
                tryActivatePendingDepthScopeForSnapshotTarget(pLogicalDevice, commandBuffer, colorOnlySnapshotTarget, "CmdEndRenderPass2(color)");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
            return;
        }

        LogicalDevice*      pLogicalDevice          = nullptr;
        DepthSnapshotTarget colorOnlySnapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice, commandBuffer, "CmdEndRenderPass2KHR");
                finishTrackedRenderPass(pLogicalDevice, commandBuffer);
                colorOnlySnapshotTarget = takeColorOnlySnapshotScope(pLogicalDevice, commandBuffer);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
            if (colorOnlySnapshotTarget.swapchain != VK_NULL_HANDLE)
                tryActivatePendingDepthScopeForSnapshotTarget(pLogicalDevice, commandBuffer, colorOnlySnapshotTarget, "CmdEndRenderPass2KHR(color)");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRendering(VkCommandBuffer commandBuffer)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRendering(commandBuffer);
            return;
        }

        LogicalDevice*      pLogicalDevice          = nullptr;
        DepthSnapshotTarget colorOnlySnapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice, commandBuffer, "CmdEndRendering");
                colorOnlySnapshotTarget = takeColorOnlySnapshotScope(pLogicalDevice, commandBuffer);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRendering(commandBuffer);
            if (colorOnlySnapshotTarget.swapchain != VK_NULL_HANDLE)
                tryActivatePendingDepthScopeForSnapshotTarget(pLogicalDevice, commandBuffer, colorOnlySnapshotTarget, "CmdEndRendering(color)");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderingKHR(VkCommandBuffer commandBuffer)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderingKHR(commandBuffer);
            return;
        }

        LogicalDevice*      pLogicalDevice          = nullptr;
        DepthSnapshotTarget colorOnlySnapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice, commandBuffer, "CmdEndRenderingKHR");
                colorOnlySnapshotTarget = takeColorOnlySnapshotScope(pLogicalDevice, commandBuffer);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderingKHR(commandBuffer);
            if (colorOnlySnapshotTarget.swapchain != VK_NULL_HANDLE)
                tryActivatePendingDepthScopeForSnapshotTarget(pLogicalDevice, commandBuffer, colorOnlySnapshotTarget, "CmdEndRenderingKHR(color)");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdCopyImage(VkCommandBuffer    commandBuffer,
                                                    VkImage            srcImage,
                                                    VkImageLayout      srcImageLayout,
                                                    VkImage            dstImage,
                                                    VkImageLayout      dstImageLayout,
                                                    uint32_t           regionCount,
                                                    const VkImageCopy* pRegions)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                pLogicalDevice = devIt->second.get();
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
            scoped_lock l(globalLock);
            tryActivatePendingTransferLinkedDepthScope(pLogicalDevice, commandBuffer, srcImage, dstImage, "CmdCopyImage");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdExecuteCommands(VkCommandBuffer        commandBuffer,
                                                          uint32_t               commandBufferCount,
                                                          const VkCommandBuffer* pCommandBuffers)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                accumulateExecutedCommandBufferDraws(pLogicalDevice, commandBuffer, pCommandBuffers, commandBufferCount);
                const auto scope = pLogicalDevice->commandBufferDepthStates.find(commandBuffer);
                if (pCommandBuffers)
                    for (uint32_t i = 0; i < commandBufferCount; ++i)
                        pLogicalDevice->depthFrameTracker.execute(
                            commandBuffer,
                            pCommandBuffers[i],
                            scope != pLogicalDevice->commandBufferDepthStates.end() && scope->second.inRenderScope ? &scope->second.depthState
                                                                                                                   : nullptr);
                if (isDepthResolveTraceEnabled())
                    Logger::debug("depth resolve trace execute: primary=" + convertToString(commandBuffer)
                                  + " secondaryCount=" + std::to_string(commandBufferCount));
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBlitImage(VkCommandBuffer    commandBuffer,
                                                    VkImage            srcImage,
                                                    VkImageLayout      srcImageLayout,
                                                    VkImage            dstImage,
                                                    VkImageLayout      dstImageLayout,
                                                    uint32_t           regionCount,
                                                    const VkImageBlit* pRegions,
                                                    VkFilter           filter)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                pLogicalDevice = devIt->second.get();
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
            scoped_lock l(globalLock);
            tryActivatePendingTransferLinkedDepthScope(pLogicalDevice, commandBuffer, srcImage, dstImage, "CmdBlitImage");
        }
    }

    VKAPI_ATTR void VKAPI_CALL
    vkShade_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, vertexCount, instanceCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndexed(
        VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, indexCount, instanceCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    VKAPI_ATTR void VKAPI_CALL
    vkShade_CmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDrawIndirect(pLogicalDevice, commandBuffer, drawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL
    vkShade_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDrawIndirect(pLogicalDevice, commandBuffer, drawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndirectCount(VkCommandBuffer commandBuffer,
                                                            VkBuffer        buffer,
                                                            VkDeviceSize    offset,
                                                            VkBuffer        countBuffer,
                                                            VkDeviceSize    countBufferOffset,
                                                            uint32_t        maxDrawCount,
                                                            uint32_t        stride)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDrawIndirect(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndirectCountKHR(VkCommandBuffer commandBuffer,
                                                               VkBuffer        buffer,
                                                               VkDeviceSize    offset,
                                                               VkBuffer        countBuffer,
                                                               VkDeviceSize    countBufferOffset,
                                                               uint32_t        maxDrawCount,
                                                               uint32_t        stride)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDrawIndirect(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer,
                                                                   VkBuffer        buffer,
                                                                   VkDeviceSize    offset,
                                                                   VkBuffer        countBuffer,
                                                                   VkDeviceSize    countBufferOffset,
                                                                   uint32_t        maxDrawCount,
                                                                   uint32_t        stride)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDrawIndirect(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndexedIndirectCountKHR(VkCommandBuffer commandBuffer,
                                                                      VkBuffer        buffer,
                                                                      VkDeviceSize    offset,
                                                                      VkBuffer        countBuffer,
                                                                      VkDeviceSize    countBufferOffset,
                                                                      uint32_t        maxDrawCount,
                                                                      uint32_t        stride)
    {
        if (!vkShade::settingsManager.isDepthCaptureActive())
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirectCountKHR(
                    commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto        devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDrawIndirect(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexedIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    static void trimDepthBackupPool(LogicalDevice* device)
    {
        size_t idle = 0;
        auto&  pool = device->depthBackupPool;
        pool.erase(std::remove_if(pool.begin(),
                                  pool.end(),
                                  [&](const auto& backup) { return backup.use_count() == 1 && (!device->depthCopyBeforeClears || ++idle > 4); }),
                   pool.end());
    }

    static void resetTrackedRenderPass(LogicalDevice* device, VkCommandBuffer commandBuffer, bool forget)
    {
        const auto pass = device->commandBufferRenderPasses.find(commandBuffer);
        if (pass == device->commandBufferRenderPasses.end())
            return;
        if (forget)
            device->commandBufferRenderPasses.erase(pass);
        else
        {
            pass->second.renderPass = VK_NULL_HANDLE;
            pass->second.attachments.clear();
        }
    }

    static void clearCommandBufferDepthTracking(LogicalDevice* device, VkCommandBuffer commandBuffer, bool forgetLevel)
    {
        if (forgetLevel)
        {
            device->commandBufferLevels.erase(commandBuffer);
            device->commandBufferPools.erase(commandBuffer);
            device->depthCopyTargets.erase(commandBuffer);
        }
        device->commandBufferUsage.erase(commandBuffer);
        device->commandBufferRecordedDraws.erase(commandBuffer);
        device->depthFrameTracker.forget(commandBuffer);
        trimDepthBackupPool(device);
        device->commandBufferDepthStates.erase(commandBuffer);
        resetTrackedRenderPass(device, commandBuffer, forgetLevel);
        device->commandBufferColorOnlySnapshotTargets.erase(commandBuffer);
        device->pendingTransferLinkedDepthScopes.erase(commandBuffer);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateCommandPool(VkDevice                       device,
                                                             const VkCommandPoolCreateInfo* createInfo,
                                                             const VkAllocationCallbacks*   allocator,
                                                             VkCommandPool*                 pool)
    {
        scoped_lock l(globalLock);
        const auto  it = deviceMap.find(GetKey(device));
        if (it == deviceMap.end() || !it->second)
            return VK_ERROR_DEVICE_LOST;
        const auto result = it->second->vkd.CreateCommandPool(device, createInfo, allocator, pool);
        if (result == VK_SUCCESS && createInfo && pool)
            it->second->commandPoolFamilies[*pool] = {createInfo->queueFamilyIndex, createInfo->flags};
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_AllocateCommandBuffers(VkDevice                           device,
                                                                  const VkCommandBufferAllocateInfo* allocateInfo,
                                                                  VkCommandBuffer*                   commandBuffers)
    {
        scoped_lock l(globalLock);
        const auto  it = deviceMap.find(GetKey(device));
        if (it == deviceMap.end() || !it->second)
            return VK_ERROR_DEVICE_LOST;
        auto*      logicalDevice = it->second.get();
        const auto result        = logicalDevice->vkd.AllocateCommandBuffers(device, allocateInfo, commandBuffers);
        if (result == VK_SUCCESS && settingsManager.isDepthCaptureActive() && allocateInfo && commandBuffers)
        {
            auto& pool = logicalDevice->commandPoolBuffers[allocateInfo->commandPool];
            for (uint32_t i = 0; i < allocateInfo->commandBufferCount; ++i)
            {
                pool.push_back(commandBuffers[i]);
                logicalDevice->commandBufferLevels[commandBuffers[i]] = allocateInfo->level;
                logicalDevice->commandBufferPools[commandBuffers[i]]  = allocateInfo->commandPool;
            }
        }
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
    {
        scoped_lock l(globalLock);
        const auto  it = deviceMap.find(GetKey(commandBuffer));
        if (it == deviceMap.end() || !it->second)
            return VK_ERROR_DEVICE_LOST;
        const auto result = it->second->vkd.ResetCommandBuffer(commandBuffer, flags);
        if (result == VK_SUCCESS)
            clearCommandBufferDepthTracking(it->second.get(), commandBuffer, false);
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
    {
        scoped_lock l(globalLock);
        const auto  it = deviceMap.find(GetKey(device));
        if (it == deviceMap.end() || !it->second)
            return VK_ERROR_DEVICE_LOST;
        const auto result  = it->second->vkd.ResetCommandPool(device, pool, flags);
        const auto buffers = it->second->commandPoolBuffers.find(pool);
        if (result == VK_SUCCESS && buffers != it->second->commandPoolBuffers.end())
            for (const auto commandBuffer : buffers->second)
                clearCommandBufferDepthTracking(it->second.get(), commandBuffer, false);
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator)
    {
        scoped_lock l(globalLock);
        const auto  it = deviceMap.find(GetKey(device));
        if (it == deviceMap.end() || !it->second)
            return;
        const auto buffers = it->second->commandPoolBuffers.find(pool);
        if (buffers != it->second->commandPoolBuffers.end())
        {
            for (const auto commandBuffer : buffers->second)
                clearCommandBufferDepthTracking(it->second.get(), commandBuffer, true);
            it->second->commandPoolBuffers.erase(buffers);
        }
        it->second->commandPoolFamilies.erase(pool);
        it->second->vkd.DestroyCommandPool(device, pool, allocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo)
    {
        scoped_lock l(globalLock);

        auto devIt = deviceMap.find(GetKey(commandBuffer));
        if (devIt == deviceMap.end())
            return VK_ERROR_DEVICE_LOST;

        LogicalDevice* pLogicalDevice = devIt->second.get();
        const auto     result         = pLogicalDevice->vkd.BeginCommandBuffer(commandBuffer, pBeginInfo);
        if (result != VK_SUCCESS)
            return result;
        pLogicalDevice->commandBufferUsage[commandBuffer] = pBeginInfo ? pBeginInfo->flags : 0;
        if (!pLogicalDevice->commandBufferLevels.count(commandBuffer))
            pLogicalDevice->commandBufferLevels[commandBuffer] = (pBeginInfo != nullptr && pBeginInfo->pInheritanceInfo != nullptr)
                                                                     ? VK_COMMAND_BUFFER_LEVEL_SECONDARY
                                                                     : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        if (isDepthResolveTraceEnabled())
            Logger::debug("depth resolve trace begin cb: cb=" + convertToString(commandBuffer)
                          + " level=" + std::to_string(static_cast<int>(pLogicalDevice->commandBufferLevels[commandBuffer]))
                          + " hasInheritance=" + std::string(pBeginInfo != nullptr && pBeginInfo->pInheritanceInfo != nullptr ? "true" : "false"));
        pLogicalDevice->commandBufferRecordedDraws[commandBuffer] = LogicalDevice::DrawStats{};
        pLogicalDevice->depthFrameTracker.begin(commandBuffer);
        trimDepthBackupPool(pLogicalDevice);
        pLogicalDevice->commandBufferDepthStates.erase(commandBuffer);
        resetTrackedRenderPass(pLogicalDevice, commandBuffer, false);
        pLogicalDevice->commandBufferColorOnlySnapshotTargets.erase(commandBuffer);
        pLogicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);

        return result;
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_FreeCommandBuffers(VkDevice               device,
                                                          VkCommandPool          commandPool,
                                                          uint32_t               commandBufferCount,
                                                          const VkCommandBuffer* pCommandBuffers)
    {
        scoped_lock l(globalLock);

        auto devIt = deviceMap.find(GetKey(device));
        if (devIt == deviceMap.end())
            return;

        LogicalDevice* pLogicalDevice = devIt->second.get();
        if (pCommandBuffers)
        {
            for (uint32_t i = 0; i < commandBufferCount; ++i)
            {
                clearCommandBufferDepthTracking(pLogicalDevice, pCommandBuffers[i], true);
            }
            auto pool = pLogicalDevice->commandPoolBuffers.find(commandPool);
            if (pool != pLogicalDevice->commandPoolBuffers.end())
            {
                for (uint32_t i = 0; i < commandBufferCount; ++i)
                    pool->second.erase(std::remove(pool->second.begin(), pool->second.end(), pCommandBuffers[i]), pool->second.end());
                if (pool->second.empty())
                    pLogicalDevice->commandPoolBuffers.erase(pool);
            }
        }

        pLogicalDevice->vkd.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
    {
        if (!image)
            return;

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        // Check if this is a tracked depth image
        auto it = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), image);
        if (it != pLogicalDevice->depthImages.end())
        {
            size_t     i        = std::distance(pLogicalDevice->depthImages.begin(), it);
            const auto metadata = pLogicalDevice->depthImageMetadata.find(image);
            if (metadata != pLogicalDevice->depthImageMetadata.end())
                pLogicalDevice->depthFrameTracker.forgetSource(metadata->second.sourceId);

            // Remove from tracking lists
            pLogicalDevice->depthImageExtents.erase(image);
            pLogicalDevice->depthImageMetadata.erase(image);
            pLogicalDevice->depthImages.erase(it);
            // TODO what if an image gets destroyed before binding memory?
            if (i < pLogicalDevice->depthImageViews.size())
            {
                pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, pLogicalDevice->depthImageViews[i], nullptr);
                pLogicalDevice->depthImageViews.erase(pLogicalDevice->depthImageViews.begin() + i);
            }
            if (i < pLogicalDevice->depthFormats.size())
                pLogicalDevice->depthFormats.erase(pLogicalDevice->depthFormats.begin() + i);

            for (auto viewIt = pLogicalDevice->depthViewStates.begin(); viewIt != pLogicalDevice->depthViewStates.end();)
            {
                if (viewIt->second.image == image)
                    viewIt = pLogicalDevice->depthViewStates.erase(viewIt);
                else
                    ++viewIt;
            }
            for (auto fbIt = pLogicalDevice->framebufferDepthStates.begin(); fbIt != pLogicalDevice->framebufferDepthStates.end();)
            {
                if (fbIt->second.image == image)
                    fbIt = pLogicalDevice->framebufferDepthStates.erase(fbIt);
                else
                    ++fbIt;
            }

            clearTrackedDepthScopesLocked(pLogicalDevice, [image](const DepthState& state) { return state.image == image; });

            if (pLogicalDevice->activeDepthState.image == image)
                updateDeviceDepthStateLocked(pLogicalDevice, {}, "DestroyImage");
        }

        pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, image, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Wayland surface interception — capture wl_display for input

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateWaylandSurfaceKHR(VkInstance                           instance,
                                                                   const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
                                                                   const VkAllocationCallbacks*         pAllocator,
                                                                   VkSurfaceKHR*                        pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateWaylandSurfaceKHR");

        // Capture the wl_display and wl_surface for Wayland input
        if (pCreateInfo && pCreateInfo->display)
            setWaylandDisplay(pCreateInfo->display);
        if (pCreateInfo && pCreateInfo->surface)
            setWaylandSurface(pCreateInfo->surface);

        // Forward to the real implementation via the next layer
        auto nextFunc = (PFN_vkCreateWaylandSurfaceKHR) instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(instanceMap[GetKey(instance)],
                                                                                                                  "vkCreateWaylandSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateXlibSurfaceKHR(VkInstance                        instance,
                                                                const VkXlibSurfaceCreateInfoKHR* pCreateInfo,
                                                                const VkAllocationCallbacks*      pAllocator,
                                                                VkSurfaceKHR*                     pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateXlibSurfaceKHR");
        markNonWaylandSurface("vkCreateXlibSurfaceKHR");

        auto nextFunc = (PFN_vkCreateXlibSurfaceKHR) instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(instanceMap[GetKey(instance)],
                                                                                                               "vkCreateXlibSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateXcbSurfaceKHR(VkInstance                       instance,
                                                               const VkXcbSurfaceCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks*     pAllocator,
                                                               VkSurfaceKHR*                    pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateXcbSurfaceKHR");
        markNonWaylandSurface("vkCreateXcbSurfaceKHR");

        auto nextFunc = (PFN_vkCreateXcbSurfaceKHR) instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(instanceMap[GetKey(instance)],
                                                                                                              "vkCreateXcbSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Enumeration function

    VkResult VKAPI_CALL vkShade_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
    {
        // When safe anti-cheat is active, hide the layer from enumeration
        if (settingsManager.getSafeAntiCheat())
        {
            if (pPropertyCount)
                *pPropertyCount = 0;
            return VK_SUCCESS;
        }

        if (pPropertyCount)
            *pPropertyCount = 1;

        if (pProperties)
        {
            std::strcpy(pProperties->layerName, VKSHADE_LAYER_NAME);
            std::strcpy(pProperties->description, "a post processing layer");
            pProperties->implementationVersion = 1;
            pProperties->specVersion           = VK_MAKE_VERSION(1, 2, 0);
        }

        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL vkShade_EnumerateDeviceLayerProperties(VkPhysicalDevice   physicalDevice,
                                                               uint32_t*          pPropertyCount,
                                                               VkLayerProperties* pProperties)
    {
        return vkShade_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL vkShade_EnumerateInstanceExtensionProperties(const char*            pLayerName,
                                                                     uint32_t*              pPropertyCount,
                                                                     VkExtensionProperties* pProperties)
    {
        if (pLayerName == NULL || std::strcmp(pLayerName, VKSHADE_LAYER_NAME))
        {
            return VK_ERROR_LAYER_NOT_PRESENT;
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL vkShade_EnumerateDeviceExtensionProperties(VkPhysicalDevice       physicalDevice,
                                                                   const char*            pLayerName,
                                                                   uint32_t*              pPropertyCount,
                                                                   VkExtensionProperties* pProperties)
    {
        // pass through any queries that aren't to us
        if (pLayerName == NULL || std::strcmp(pLayerName, VKSHADE_LAYER_NAME))
        {
            if (physicalDevice == VK_NULL_HANDLE)
            {
                return VK_SUCCESS;
            }

            scoped_lock l(globalLock);
            return instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(
                physicalDevice, pLayerName, pPropertyCount, pProperties);
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }
} // namespace vkShade

extern "C"
{ // these are the entry points for the layer, so they need to be c-linkeable

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL vkShade_GetDeviceProcAddr(VkDevice device, const char* pName);
    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL vkShade_GetInstanceProcAddr(VkInstance instance, const char* pName);

    static PFN_vkGetInstanceProcAddr getNextInstanceProcAddr()
    {
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(RTLD_NEXT, "vkGetInstanceProcAddr"));
    }

    static PFN_vkGetDeviceProcAddr getNextDeviceProcAddr()
    {
        return reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(RTLD_NEXT, "vkGetDeviceProcAddr"));
    }

#define GETPROCADDR(func) \
    if (!std::strcmp(pName, "vk" #func)) \
        return (PFN_vkVoidFunction) & vkShade::vkShade_##func;
    /*
    Return our funktions for the funktions we want to intercept
    the macro takes the name and returns our vkShade_##func, if the name is equal
    */

    // vkGetDeviceProcAddr needs to behave like vkGetInstanceProcAddr thanks to some games
#define INTERCEPT_CALLS \
    /* instance chain functions we intercept */ \
    if (!std::strcmp(pName, "vkGetInstanceProcAddr")) \
        return (PFN_vkVoidFunction) & vkShade_GetInstanceProcAddr; \
    GETPROCADDR(EnumerateInstanceLayerProperties); \
    GETPROCADDR(EnumerateInstanceExtensionProperties); \
    GETPROCADDR(CreateInstance); \
    GETPROCADDR(DestroyInstance); \
    GETPROCADDR(CreateWaylandSurfaceKHR); \
    GETPROCADDR(CreateXlibSurfaceKHR); \
    GETPROCADDR(CreateXcbSurfaceKHR); \
\
    /* device chain functions we intercept*/ \
    if (!std::strcmp(pName, "vkGetDeviceProcAddr")) \
        return (PFN_vkVoidFunction) & vkShade_GetDeviceProcAddr; \
    GETPROCADDR(EnumerateDeviceLayerProperties); \
    GETPROCADDR(EnumerateDeviceExtensionProperties); \
    GETPROCADDR(CreateDevice); \
    GETPROCADDR(DestroyDevice); \
    GETPROCADDR(CreateSwapchainKHR); \
    GETPROCADDR(GetSwapchainImagesKHR); \
    GETPROCADDR(QueueSubmit); \
    GETPROCADDR(QueueSubmit2); \
    GETPROCADDR(QueueSubmit2KHR); \
    GETPROCADDR(QueuePresentKHR); \
    GETPROCADDR(DestroySwapchainKHR); \
\
    GETPROCADDR(CreateImage); \
    GETPROCADDR(DestroyImage); \
    GETPROCADDR(BindImageMemory); \
    GETPROCADDR(CreateImageView); \
    GETPROCADDR(DestroyImageView); \
    GETPROCADDR(CreateRenderPass); \
    GETPROCADDR(CreateRenderPass2); \
    GETPROCADDR(CreateRenderPass2KHR); \
    GETPROCADDR(DestroyRenderPass); \
    GETPROCADDR(CreateFramebuffer); \
    GETPROCADDR(DestroyFramebuffer); \
    GETPROCADDR(CreateCommandPool); \
    GETPROCADDR(AllocateCommandBuffers); \
    GETPROCADDR(BeginCommandBuffer); \
    GETPROCADDR(ResetCommandBuffer); \
    GETPROCADDR(ResetCommandPool); \
    GETPROCADDR(DestroyCommandPool); \
    GETPROCADDR(FreeCommandBuffers); \
    GETPROCADDR(CmdBeginRenderPass); \
    GETPROCADDR(CmdBeginRenderPass2); \
    GETPROCADDR(CmdBeginRenderPass2KHR); \
    GETPROCADDR(CmdBeginRendering); \
    GETPROCADDR(CmdBeginRenderingKHR); \
    GETPROCADDR(CmdPipelineBarrier); \
    GETPROCADDR(CmdPipelineBarrier2); \
    GETPROCADDR(CmdPipelineBarrier2KHR); \
    GETPROCADDR(CmdClearAttachments); \
    GETPROCADDR(CmdClearDepthStencilImage); \
    GETPROCADDR(CmdEndRenderPass); \
    GETPROCADDR(CmdEndRenderPass2); \
    GETPROCADDR(CmdEndRenderPass2KHR); \
    GETPROCADDR(CmdEndRendering); \
    GETPROCADDR(CmdEndRenderingKHR); \
    GETPROCADDR(CmdNextSubpass); \
    GETPROCADDR(CmdNextSubpass2); \
    GETPROCADDR(CmdNextSubpass2KHR); \
    GETPROCADDR(CmdBlitImage); \
    GETPROCADDR(CmdCopyImage); \
    GETPROCADDR(CmdExecuteCommands); \
    GETPROCADDR(CmdDraw); \
    GETPROCADDR(CmdDrawIndexed); \
    GETPROCADDR(CmdDrawIndirect); \
    GETPROCADDR(CmdDrawIndexedIndirect); \
    GETPROCADDR(CmdDrawIndirectCount); \
    GETPROCADDR(CmdDrawIndirectCountKHR); \
    GETPROCADDR(CmdDrawIndexedIndirectCount); \
    GETPROCADDR(CmdDrawIndexedIndirectCountKHR);

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL vkShade_GetDeviceProcAddr(VkDevice device, const char* pName)
    {
        vkShade::initConfigs();

        if (device != VK_NULL_HANDLE
            && (!std::strcmp(pName, "vkQueueSubmit2") || !std::strcmp(pName, "vkQueueSubmit2KHR") || !std::strcmp(pName, "vkCmdPipelineBarrier2")
                || !std::strcmp(pName, "vkCmdPipelineBarrier2KHR") || !std::strcmp(pName, "vkCmdNextSubpass2")
                || !std::strcmp(pName, "vkCmdNextSubpass2KHR")))
        {
            vkShade::scoped_lock l(vkShade::globalLock);
            const auto           it = vkShade::deviceMap.find(vkShade::GetKey(device));
            if (it != vkShade::deviceMap.end() && it->second && it->second->vkd.GetDeviceProcAddr
                && !it->second->vkd.GetDeviceProcAddr(device, pName))
                return nullptr;
        }

        INTERCEPT_CALLS

        if (device == VK_NULL_HANDLE)
        {
            PFN_vkGetDeviceProcAddr next = getNextDeviceProcAddr();
            return next ? next(device, pName) : nullptr;
        }

        {
            vkShade::scoped_lock l(vkShade::globalLock);
            auto                 it = vkShade::deviceMap.find(vkShade::GetKey(device));
            if (it != vkShade::deviceMap.end() && it->second && it->second->vkd.GetDeviceProcAddr)
                return it->second->vkd.GetDeviceProcAddr(device, pName);
        }

        PFN_vkGetDeviceProcAddr next = getNextDeviceProcAddr();
        return next ? next(device, pName) : nullptr;
    }

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL vkShade_GetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        vkShade::initConfigs();

        INTERCEPT_CALLS

        if (instance == VK_NULL_HANDLE)
        {
            PFN_vkGetInstanceProcAddr next = getNextInstanceProcAddr();
            return next ? next(instance, pName) : nullptr;
        }

        {
            vkShade::scoped_lock l(vkShade::globalLock);
            auto                 it = vkShade::instanceDispatchMap.find(vkShade::GetKey(instance));
            if (it != vkShade::instanceDispatchMap.end() && it->second.GetInstanceProcAddr)
                return it->second.GetInstanceProcAddr(instance, pName);
        }

        PFN_vkGetInstanceProcAddr next = getNextInstanceProcAddr();
        return next ? next(instance, pName) : nullptr;
    }

} // extern "C"
