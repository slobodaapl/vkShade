#include "vulkan_include.hpp"

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
#include "fake_swapchain.hpp"
#include "renderpass.hpp"
#include "format.hpp"
#include "logger.hpp"
#include "shader_sources.hpp"

#include "effects/effect.hpp"
#include "effects/effect_reshade.hpp"
#include "effects/effect_transfer.hpp"
#include "effects/builtin/builtin_effects.hpp"
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
    std::shared_ptr<Config> pBaseConfig = nullptr;  // Always vkShade.conf
    std::shared_ptr<Config> pConfig = nullptr;      // Current config (base + overlay)
    EffectRegistry effectRegistry;                   // Single source of truth for effect configs

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
        std::vector<std::string> currentConfigEffects;
        std::vector<std::string> defaultConfigEffects;
        std::map<std::string, std::string> effectPaths;
        std::string configPath;
        bool initialized = false;
    };
    CachedEffectsData cachedEffects;

    // Cached parameters (to avoid re-parsing config every frame)
    struct CachedParametersData
    {
        std::vector<std::unique_ptr<EffectParam>> parameters;
        std::vector<std::string> effectNames;  // Effects when params were collected
        std::string configPath;
        bool dirty = true;  // Set to true to force recollection
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
        bool pending = false;
    };
    ResizeDebounceState resizeDebounce;
    constexpr int64_t RESIZE_DEBOUNCE_MS = 200;

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

    static VkImageLayout getInternalDepthReadOnlyLayoutForDebug(VkFormat format)
    {
        return isStencilFormat(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }

    static VkImageLayout getDepthResolveReadOnlyLayoutForDebug(const LogicalSwapchain* pLogicalSwapchain)
    {
        if (!pLogicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        return pLogicalSwapchain->depthResolveUsesShader
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : getInternalDepthReadOnlyLayoutForDebug(pLogicalSwapchain->depthResolveFormat);
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
        if (imageIndex >= pLogicalSwapchain->depthResolveImages.size()
            || imageIndex >= pLogicalSwapchain->depthResolveMemories.size()
            || imageIndex >= pLogicalSwapchain->depthResolveImageViews.size())
            return;
        if (pLogicalSwapchain->depthResolveFormat != VK_FORMAT_D32_SFLOAT
            && pLogicalSwapchain->depthResolveFormat != VK_FORMAT_R32_SFLOAT)
        {
            Logger::warn("depth copy dump only supports VK_FORMAT_D32_SFLOAT or VK_FORMAT_R32_SFLOAT right now, got format="
                         + convertToString(pLogicalSwapchain->depthResolveFormat));
            dumped = true;
            return;
        }

        VkExtent3D extent = pLogicalSwapchain->depthResolveExtent;
        if (extent.width == 0 || extent.height == 0)
            return;

        dumped = true;

        // One-shot debug path: wait for prior work so the sampled backup image is stable.
        pLogicalDevice->vkd.QueueWaitIdle(queue);

        const VkDeviceSize dumpSize = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * sizeof(float);
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        createBuffer(pLogicalDevice,
                     dumpSize,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer,
                     stagingMemory);

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = pLogicalDevice->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkResult vr = pLogicalDevice->vkd.AllocateCommandBuffers(pLogicalDevice->device, &allocInfo, &commandBuffer);
        ASSERT_VULKAN(vr);
        initializeDispatchTable(commandBuffer, pLogicalDevice->device);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vr = pLogicalDevice->vkd.BeginCommandBuffer(commandBuffer, &beginInfo);
        ASSERT_VULKAN(vr);

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = pLogicalSwapchain->depthResolveImages[imageIndex];
        barrier.oldLayout = getDepthResolveReadOnlyLayoutForDebug(pLogicalSwapchain);
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = getDepthResolveAspectMaskForDebug(pLogicalSwapchain);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

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

        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = getDepthResolveAspectMaskForDebug(pLogicalSwapchain);
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = extent.width;
        region.imageExtent.height = extent.height;
        region.imageExtent.depth = 1;

        pLogicalDevice->vkd.CmdCopyImageToBuffer(commandBuffer,
                                                 pLogicalSwapchain->depthResolveImages[imageIndex],
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 stagingBuffer,
                                                 1,
                                                 &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = getDepthResolveReadOnlyLayoutForDebug(pLogicalSwapchain);
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

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vr = pLogicalDevice->vkd.QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        ASSERT_VULKAN(vr);
        pLogicalDevice->vkd.QueueWaitIdle(queue);

        void* mapped = nullptr;
        vr = pLogicalDevice->vkd.MapMemory(pLogicalDevice->device, stagingMemory, 0, dumpSize, 0, &mapped);
        ASSERT_VULKAN(vr);

        const float* values = static_cast<const float*>(mapped);
        const size_t pixelCount = static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height);
        float minValue = std::numeric_limits<float>::infinity();
        float maxValue = -std::numeric_limits<float>::infinity();
        size_t finiteCount = 0;
        size_t zeroishCount = 0;
        size_t oneishCount = 0;
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

        auto sampleAt = [&](uint32_t x, uint32_t y) -> float {
            return values[static_cast<size_t>(y) * static_cast<size_t>(extent.width) + x];
        };

        const uint32_t centerX = extent.width / 2;
        const uint32_t centerY = extent.height / 2;
        Logger::warn("depth copy dump: imageIndex=" + std::to_string(imageIndex)
                     + " extent=" + std::to_string(extent.width) + "x" + std::to_string(extent.height)
                     + " finite=" + std::to_string(finiteCount)
                     + " min=" + std::to_string(minValue)
                     + " max=" + std::to_string(maxValue)
                     + " zeroish=" + std::to_string(zeroishCount)
                     + " oneish=" + std::to_string(oneishCount)
                     + " sample(0,0)=" + std::to_string(sampleAt(0, 0))
                     + " sample(center)=" + std::to_string(sampleAt(centerX, centerY))
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
                checkpoint = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV;
            }
            pLogicalDevice->vkd.GetQueueCheckpointData2NV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(
                    std::string(context) + ": checkpoint[" + std::to_string(i) +
                    "] stage=0x" + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) +
                    " marker=0x" + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
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
                checkpoint = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
            }
            pLogicalDevice->vkd.GetQueueCheckpointDataNV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(
                    std::string(context) + ": checkpoint[" + std::to_string(i) +
                    "] stage=0x" + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) +
                    " marker=0x" + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
            }
        }
    }

    static void logDeviceFaultInfo(LogicalDevice* pLogicalDevice, const char* context)
    {
        if (!pLogicalDevice || !pLogicalDevice->supportsDeviceFaultExt || !pLogicalDevice->vkd.GetDeviceFaultInfoEXT)
            return;

        VkDeviceFaultCountsEXT faultCounts = {};
        faultCounts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        VkResult countsResult = pLogicalDevice->vkd.GetDeviceFaultInfoEXT(pLogicalDevice->device, &faultCounts, nullptr);
        if (countsResult != VK_SUCCESS && countsResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(counts) failed: " + std::to_string(countsResult));
            return;
        }

        std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(faultCounts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(faultCounts.vendorInfoCount);
        std::vector<uint8_t> vendorBinary(faultCounts.vendorBinarySize);

        VkDeviceFaultInfoEXT faultInfo = {};
        faultInfo.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        faultInfo.pAddressInfos = addressInfos.empty() ? nullptr : addressInfos.data();
        faultInfo.pVendorInfos = vendorInfos.empty() ? nullptr : vendorInfos.data();
        faultInfo.pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data();

        VkResult infoResult = pLogicalDevice->vkd.GetDeviceFaultInfoEXT(pLogicalDevice->device, &faultCounts, &faultInfo);
        if (infoResult != VK_SUCCESS && infoResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(info) failed: " + std::to_string(infoResult));
            return;
        }

        Logger::err(
            std::string(context) + ": device fault description=\"" + faultInfo.description +
            "\", addresses=" + std::to_string(faultCounts.addressInfoCount) +
            ", vendorInfos=" + std::to_string(faultCounts.vendorInfoCount) +
            ", vendorBinaryBytes=" + std::to_string(faultCounts.vendorBinarySize));

        for (uint32_t i = 0; i < faultCounts.addressInfoCount; ++i)
        {
            const auto& address = addressInfos[i];
            Logger::warn(
                std::string(context) + ": faultAddress[" + std::to_string(i) +
                "] type=" + std::to_string(address.addressType) +
                " reported=0x" + formatHexU64(address.reportedAddress) +
                " precision=0x" + formatHexU64(address.addressPrecision));
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
    static thread_local sigjmp_buf signalJmpBuf;
    static thread_local volatile sig_atomic_t signalJmpActive = 0;
    static thread_local volatile sig_atomic_t caughtSignal = 0;

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
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, 2);  // fd 2 = stderr
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
        sa.sa_handler = crashSignalHandler;
        sa.sa_flags = 0;
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
            changed = true;
        }
        if (isStencilFormat(attachment.format) && needsStoredAttachmentPreservation(attachment.stencilStoreOp))
        {
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
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
            changed = true;
        }
        if (isStencilFormat(attachment.format) && needsStoredAttachmentPreservation(attachment.stencilStoreOp))
        {
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        return changed;
    }

    static bool forceDepthAttachmentStoreOp(VkRenderingAttachmentInfo& attachment, bool hasStencilAspect)
    {
        bool changed = false;
        if (attachment.imageView != VK_NULL_HANDLE && needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        if (hasStencilAspect && attachment.imageView != VK_NULL_HANDLE && needsStoredAttachmentPreservation(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed = true;
        }
        return changed;
    }

    bool sameDepthState(const DepthState& a, const DepthState& b)
    {
        return a.image == b.image && a.imageView == b.imageView && a.format == b.format
            && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.extent.depth == b.extent.depth
            && a.observedLayout == b.observedLayout;
    }

    static bool hasPresentableSnapshotTarget(const DepthSnapshotTarget& target)
    {
        return target.swapchain != VK_NULL_HANDLE
            && target.pLogicalSwapchain != nullptr
            && target.imageIndex < target.pLogicalSwapchain->imageCount;
    }

    static bool matchesPresentableSnapshotTargetExtent(const DepthState& depth, const DepthSnapshotTarget& target)
    {
        return hasPresentableSnapshotTarget(target)
            && depth.extent.width == target.pLogicalSwapchain->imageExtent.width
            && depth.extent.height == target.pLogicalSwapchain->imageExtent.height;
    }

    static bool matchesAnySwapchainExtent(LogicalDevice* pLogicalDevice, const DepthState& depth)
    {
        if (!hasDepthState(depth))
            return false;

        for (auto& [_, pLogicalSwapchain] : swapchainMap)
        {
            if (!pLogicalSwapchain || pLogicalSwapchain->pLogicalDevice != pLogicalDevice)
                continue;

            if (depth.extent.width == pLogicalSwapchain->imageExtent.width
                && depth.extent.height == pLogicalSwapchain->imageExtent.height)
                return true;
        }

        return false;
    }

    static bool isQualifiedDepthCandidate(LogicalDevice* pLogicalDevice,
                                          const LogicalDevice::DepthScopeTrackingState& scopeState)
    {
        return hasDepthState(scopeState.depthState)
            && scopeState.drawCount > 0
            && (hasPresentableSnapshotTarget(scopeState.snapshotTarget)
                || (scopeState.drawCount >= 64 && matchesAnySwapchainExtent(pLogicalDevice, scopeState.depthState)));
    }

    template <typename Predicate>
    void clearTrackedDepthScopesLocked(LogicalDevice* pLogicalDevice, Predicate predicate)
    {
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

        if (pLogicalDevice->bestDepthCandidate.valid && predicate(pLogicalDevice->bestDepthCandidate.depthState))
            pLogicalDevice->bestDepthCandidate = {};
    }

    DepthState selectDepthStateFromRenderPassBegin(LogicalDevice* pLogicalDevice, const VkRenderPassBeginInfo* pRenderPassBegin)
    {
        DepthState depth;
        if (!pRenderPassBegin)
            return depth;

        bool found = false;
        for (const VkBaseInStructure* next = reinterpret_cast<const VkBaseInStructure*>(pRenderPassBegin->pNext); next; next = next->pNext)
        {
            if (next->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                continue;

            const auto* attachmentBeginInfo = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);
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

    static bool findDepthSnapshotTargetForImageView(LogicalDevice* pLogicalDevice,
                                                    VkImageView imageView,
                                                    DepthSnapshotTarget& target)
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

            target.swapchain = swapchainHandle;
            target.pLogicalSwapchain = pLogicalSwapchain.get();
            target.imageIndex = static_cast<uint32_t>(imageIt - pLogicalSwapchain->imageViews.begin());
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
                target.swapchain = swapchainHandle;
                target.pLogicalSwapchain = pLogicalSwapchain.get();
                target.imageIndex = static_cast<uint32_t>(realImageIt - pLogicalSwapchain->images.begin());
                return target;
            }

            const size_t presentableFakeCount = std::min<size_t>(pLogicalSwapchain->fakeImages.size(), pLogicalSwapchain->imageCount);
            auto fakeImageIt = std::find(pLogicalSwapchain->fakeImages.begin(),
                                         pLogicalSwapchain->fakeImages.begin() + presentableFakeCount,
                                         image);
            if (fakeImageIt != pLogicalSwapchain->fakeImages.begin() + presentableFakeCount)
            {
                target.swapchain = swapchainHandle;
                target.pLogicalSwapchain = pLogicalSwapchain.get();
                target.imageIndex = static_cast<uint32_t>(fakeImageIt - pLogicalSwapchain->fakeImages.begin());
                return target;
            }
        }

        return target;
    }

    static DepthSnapshotTarget selectDepthSnapshotTargetFromImageViews(LogicalDevice* pLogicalDevice,
                                                                        const VkImageView* imageViews,
                                                                        uint32_t imageViewCount)
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

    static DepthSnapshotTarget selectDepthSnapshotTargetFromRenderPassBegin(LogicalDevice* pLogicalDevice,
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

    static DepthSnapshotTarget selectDepthSnapshotTargetFromRenderingInfo(LogicalDevice* pLogicalDevice,
                                                                           const VkRenderingInfo* pRenderingInfo)
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

        if (pRenderingInfo->pDepthAttachment && findDepthSnapshotTargetForImageView(pLogicalDevice, pRenderingInfo->pDepthAttachment->imageView, target))
            return target;

        if (pRenderingInfo->pStencilAttachment && findDepthSnapshotTargetForImageView(pLogicalDevice, pRenderingInfo->pStencilAttachment->imageView, target))
            return target;

        return target;
    }

    void beginTrackedDepthScope(LogicalDevice* pLogicalDevice,
                                VkCommandBuffer commandBuffer,
                                const DepthState& depthState,
                                DepthSnapshotTarget snapshotTarget)
    {
        pLogicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);
        auto& scopeState = pLogicalDevice->commandBufferDepthStates[commandBuffer];
        scopeState.inRenderScope = true;
        scopeState.depthState = depthState;
        scopeState.snapshotTarget = snapshotTarget;
        scopeState.drawCount = 0;
    }

    void countTrackedDepthDraw(LogicalDevice* pLogicalDevice, VkCommandBuffer commandBuffer, uint32_t drawCount = 1)
    {
        pLogicalDevice->commandBufferRecordedDrawCounts[commandBuffer] += drawCount;

        auto scopeIt = pLogicalDevice->commandBufferDepthStates.find(commandBuffer);
        if (scopeIt == pLogicalDevice->commandBufferDepthStates.end() || !scopeIt->second.inRenderScope)
            return;

        scopeIt->second.drawCount += drawCount;
    }

    void accumulateExecutedCommandBufferDraws(LogicalDevice* pLogicalDevice,
                                              VkCommandBuffer primaryCommandBuffer,
                                              const VkCommandBuffer* pCommandBuffers,
                                              uint32_t commandBufferCount)
    {
        auto scopeIt = pLogicalDevice->commandBufferDepthStates.find(primaryCommandBuffer);
        if (scopeIt == pLogicalDevice->commandBufferDepthStates.end() || !scopeIt->second.inRenderScope || !pCommandBuffers)
            return;

        uint32_t additionalDraws = 0;
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            auto countIt = pLogicalDevice->commandBufferRecordedDrawCounts.find(pCommandBuffers[i]);
            if (countIt != pLogicalDevice->commandBufferRecordedDrawCounts.end())
                additionalDraws += countIt->second;
        }

        if (additionalDraws == 0)
            return;

        scopeIt->second.drawCount += additionalDraws;
        Logger::debug("accumulated secondary command buffer draws: primary="
                      + convertToString(primaryCommandBuffer)
                      + " draws=" + std::to_string(additionalDraws)
                      + " total=" + std::to_string(scopeIt->second.drawCount));
    }

    void updateDeviceDepthStateLocked(LogicalDevice* pLogicalDevice, const DepthState& depth, const char* reason);
    void recordDepthResolveSnapshotForCommandBuffer(LogicalDevice* pLogicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState,
                                                    const DepthSnapshotTarget* pSnapshotTarget);
    void recordDepthResolveSnapshotForAllSwapchains(LogicalDevice* pLogicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState);

    bool endTrackedDepthScope(LogicalDevice* pLogicalDevice,
                              VkCommandBuffer commandBuffer,
                              const char* reason,
                              DepthState* pPromotedDepthState = nullptr,
                              DepthSnapshotTarget* pSnapshotTarget = nullptr)
    {
        auto scopeIt = pLogicalDevice->commandBufferDepthStates.find(commandBuffer);
        if (scopeIt == pLogicalDevice->commandBufferDepthStates.end())
            return false;

        LogicalDevice::DepthScopeTrackingState scopeState = scopeIt->second;
        pLogicalDevice->commandBufferDepthStates.erase(scopeIt);

        if (!scopeState.inRenderScope || !hasDepthState(scopeState.depthState))
            return false;

        if (pPromotedDepthState != nullptr)
            *pPromotedDepthState = scopeState.depthState;
        if (pSnapshotTarget != nullptr)
            *pSnapshotTarget = scopeState.snapshotTarget;

        const bool scopeHasPresentableSnapshotTarget = hasPresentableSnapshotTarget(scopeState.snapshotTarget);
        const bool scopeExtentMatchesPresentableTarget =
            matchesPresentableSnapshotTargetExtent(scopeState.depthState, scopeState.snapshotTarget);

        bool shouldPromote = !pLogicalDevice->bestDepthCandidate.valid;
        if (!shouldPromote)
        {
            const bool bestHasPresentableSnapshotTarget = pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
            if (scopeHasPresentableSnapshotTarget != bestHasPresentableSnapshotTarget)
            {
                shouldPromote = scopeHasPresentableSnapshotTarget;
            }
            else if (scopeHasPresentableSnapshotTarget)
            {
                if (scopeState.drawCount != pLogicalDevice->bestDepthCandidate.drawCount)
                {
                    shouldPromote = scopeState.drawCount > pLogicalDevice->bestDepthCandidate.drawCount;
                }
                else if (scopeExtentMatchesPresentableTarget != pLogicalDevice->bestDepthCandidate.extentMatchesPresentableTarget)
                {
                    shouldPromote = scopeExtentMatchesPresentableTarget;
                }
            }
            else if (scopeState.drawCount != pLogicalDevice->bestDepthCandidate.drawCount)
            {
                shouldPromote = scopeState.drawCount > pLogicalDevice->bestDepthCandidate.drawCount;
            }
        }

        if (!shouldPromote)
            return false;

        pLogicalDevice->bestDepthCandidate.valid = true;
        pLogicalDevice->bestDepthCandidate.depthState = scopeState.depthState;
        pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget = scopeHasPresentableSnapshotTarget;
        pLogicalDevice->bestDepthCandidate.extentMatchesPresentableTarget = scopeExtentMatchesPresentableTarget;
        pLogicalDevice->bestDepthCandidate.drawCount = scopeState.drawCount;

        Logger::debug("depth candidate promoted from " + std::string(reason)
                      + ": draws=" + std::to_string(scopeState.drawCount)
                      + " presentable=" + std::string(scopeHasPresentableSnapshotTarget ? "true" : "false")
                      + " extentMatch=" + std::string(scopeExtentMatchesPresentableTarget ? "true" : "false")
                      + " image=" + convertToString(scopeState.depthState.image)
                      + " view=" + convertToString(scopeState.depthState.imageView)
                      + " format=" + convertToString(scopeState.depthState.format)
                      + " extent=" + std::to_string(scopeState.depthState.extent.width) + "x"
                      + std::to_string(scopeState.depthState.extent.height));

        if (isQualifiedDepthCandidate(pLogicalDevice, scopeState))
        {
            updateDeviceDepthStateLocked(pLogicalDevice, scopeState.depthState, reason);
        }
        else
        {
            if (hasDepthState(scopeState.depthState) && scopeState.drawCount > 0)
                pLogicalDevice->pendingTransferLinkedDepthScopes[commandBuffer] = scopeState;

            Logger::debug("depth candidate not activated from " + std::string(reason)
                          + ": draws=" + std::to_string(scopeState.drawCount)
                          + " presentable=" + std::string(scopeHasPresentableSnapshotTarget ? "true" : "false")
                          + " extentMatch=" + std::string(scopeExtentMatchesPresentableTarget ? "true" : "false"));
        }
        return true;
    }

    void tryActivatePendingTransferLinkedDepthScope(LogicalDevice* pLogicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    VkImage destinationImage,
                                                    const char* reason)
    {
        if (!pLogicalDevice || destinationImage == VK_NULL_HANDLE)
            return;

        auto pendingIt = pLogicalDevice->pendingTransferLinkedDepthScopes.find(commandBuffer);
        if (pendingIt == pLogicalDevice->pendingTransferLinkedDepthScopes.end())
            return;

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImage(pLogicalDevice, destinationImage);
        if (!hasPresentableSnapshotTarget(snapshotTarget))
            return;

        auto scopeState = pendingIt->second;
        scopeState.snapshotTarget = snapshotTarget;
        pLogicalDevice->pendingTransferLinkedDepthScopes.erase(pendingIt);

        Logger::debug("depth candidate transfer-linked from " + std::string(reason)
                      + ": draws=" + std::to_string(scopeState.drawCount)
                      + " swapchain=" + convertToString(snapshotTarget.swapchain)
                      + " imageIndex=" + std::to_string(snapshotTarget.imageIndex)
                      + " depthImage=" + convertToString(scopeState.depthState.image)
                      + " depthView=" + convertToString(scopeState.depthState.imageView));

        pLogicalDevice->bestDepthCandidate.valid = true;
        pLogicalDevice->bestDepthCandidate.depthState = scopeState.depthState;
        pLogicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget = true;
        pLogicalDevice->bestDepthCandidate.extentMatchesPresentableTarget =
            matchesPresentableSnapshotTargetExtent(scopeState.depthState, snapshotTarget);
        pLogicalDevice->bestDepthCandidate.drawCount = scopeState.drawCount;

        updateDeviceDepthStateLocked(pLogicalDevice, scopeState.depthState, reason);
        recordDepthResolveSnapshotForCommandBuffer(pLogicalDevice, commandBuffer, scopeState.depthState, &snapshotTarget);
    }

    void recordDepthResolveSnapshotForCommandBuffer(LogicalDevice* pLogicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState,
                                                    const DepthSnapshotTarget* pSnapshotTarget = nullptr)
    {
        if (!pLogicalDevice || !hasDepthState(depthState))
            return;

        scoped_lock l(globalLock);

        if (pSnapshotTarget == nullptr || pSnapshotTarget->swapchain == VK_NULL_HANDLE)
        {
            Logger::debug("skip depth resolve snapshot: missing snapshot target for commandBuffer="
                          + convertToString(commandBuffer)
                          + " depthImage=" + convertToString(depthState.image)
                          + " depthView=" + convertToString(depthState.imageView));
            return;
        }

        DepthSnapshotTarget target = *pSnapshotTarget;
        auto swapIt = swapchainMap.find(target.swapchain);
        if (swapIt == swapchainMap.end() || !swapIt->second || swapIt->second->pLogicalDevice != pLogicalDevice)
        {
            Logger::debug("skip depth resolve snapshot: snapshot target swapchain not found for commandBuffer="
                          + convertToString(commandBuffer)
                          + " swapchain=" + convertToString(target.swapchain));
            return;
        }
        target.pLogicalSwapchain = swapIt->second.get();

        Logger::debug("record depth resolve snapshot: commandBuffer=" + convertToString(commandBuffer)
                      + " swapchain=" + convertToString(target.swapchain)
                      + " imageIndex=" + std::to_string(target.imageIndex)
                      + " depthImage=" + convertToString(depthState.image)
                      + " depthView=" + convertToString(depthState.imageView)
                      + " format=" + convertToString(depthState.format));

        recordDepthResolveSnapshot(pLogicalDevice, target.pLogicalSwapchain, commandBuffer, target.imageIndex, depthState);
    }

    void recordDepthResolveSnapshotForAllSwapchains(LogicalDevice* pLogicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState)
    {
        if (!pLogicalDevice || !hasDepthState(depthState))
            return;

        for (auto& [swapchainHandle, pLogicalSwapchain] : swapchainMap)
        {
            if (!pLogicalSwapchain || pLogicalSwapchain->pLogicalDevice != pLogicalDevice)
                continue;

            for (uint32_t imageIndex = 0; imageIndex < pLogicalSwapchain->imageCount; ++imageIndex)
            {
                Logger::debug("record depth resolve snapshot for all swapchain images: commandBuffer="
                              + convertToString(commandBuffer)
                              + " swapchain=" + convertToString(swapchainHandle)
                              + " imageIndex=" + std::to_string(imageIndex)
                              + " depthImage=" + convertToString(depthState.image)
                              + " depthView=" + convertToString(depthState.imageView)
                              + " format=" + convertToString(depthState.format));
                recordDepthResolveSnapshot(pLogicalDevice, pLogicalSwapchain.get(), commandBuffer, imageIndex, depthState);
            }
        }
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

    void updateDeviceDepthStateLocked(LogicalDevice* pLogicalDevice, const DepthState& depth, const char* reason);
    void reallocateCommandBuffers(LogicalDevice* pLogicalDevice, LogicalSwapchain* pLogicalSwapchain, const DepthState& depth);

    void destroyDepthResolveResources(LogicalSwapchain* pLogicalSwapchain)
    {
        for (auto& framebuffer : pLogicalSwapchain->depthResolveFramebuffers)
        {
            pLogicalSwapchain->pLogicalDevice->vkd.DestroyFramebuffer(pLogicalSwapchain->pLogicalDevice->device, framebuffer, nullptr);
        }
        pLogicalSwapchain->depthResolveFramebuffers.clear();

        if (pLogicalSwapchain->depthResolvePipeline)
        {
            pLogicalSwapchain->pLogicalDevice->vkd.DestroyPipeline(
                pLogicalSwapchain->pLogicalDevice->device, pLogicalSwapchain->depthResolvePipeline, nullptr);
            pLogicalSwapchain->depthResolvePipeline = VK_NULL_HANDLE;
        }
        if (pLogicalSwapchain->depthResolvePipelineLayout)
        {
            pLogicalSwapchain->pLogicalDevice->vkd.DestroyPipelineLayout(
                pLogicalSwapchain->pLogicalDevice->device, pLogicalSwapchain->depthResolvePipelineLayout, nullptr);
            pLogicalSwapchain->depthResolvePipelineLayout = VK_NULL_HANDLE;
        }
        if (pLogicalSwapchain->depthResolveRenderPass)
        {
            pLogicalSwapchain->pLogicalDevice->vkd.DestroyRenderPass(
                pLogicalSwapchain->pLogicalDevice->device, pLogicalSwapchain->depthResolveRenderPass, nullptr);
            pLogicalSwapchain->depthResolveRenderPass = VK_NULL_HANDLE;
        }
        if (pLogicalSwapchain->depthResolveDescriptorPool)
        {
            pLogicalSwapchain->pLogicalDevice->vkd.DestroyDescriptorPool(
                pLogicalSwapchain->pLogicalDevice->device, pLogicalSwapchain->depthResolveDescriptorPool, nullptr);
            pLogicalSwapchain->depthResolveDescriptorPool = VK_NULL_HANDLE;
        }
        if (pLogicalSwapchain->depthResolveDescriptorSetLayout)
        {
            pLogicalSwapchain->pLogicalDevice->vkd.DestroyDescriptorSetLayout(
                pLogicalSwapchain->pLogicalDevice->device, pLogicalSwapchain->depthResolveDescriptorSetLayout, nullptr);
            pLogicalSwapchain->depthResolveDescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (pLogicalSwapchain->depthResolveSampler)
        {
            pLogicalSwapchain->pLogicalDevice->vkd.DestroySampler(
                pLogicalSwapchain->pLogicalDevice->device, pLogicalSwapchain->depthResolveSampler, nullptr);
            pLogicalSwapchain->depthResolveSampler = VK_NULL_HANDLE;
        }

        for (auto& imageView : pLogicalSwapchain->depthResolveImageViews)
        {
            if (imageView != VK_NULL_HANDLE)
            {
                pLogicalSwapchain->pLogicalDevice->vkd.DestroyImageView(
                    pLogicalSwapchain->pLogicalDevice->device, imageView, nullptr);
            }
        }
        pLogicalSwapchain->depthResolveImageViews.clear();

        for (auto& image : pLogicalSwapchain->depthResolveImages)
        {
            if (image != VK_NULL_HANDLE)
            {
                pLogicalSwapchain->pLogicalDevice->vkd.DestroyImage(
                    pLogicalSwapchain->pLogicalDevice->device, image, nullptr);
            }
        }
        pLogicalSwapchain->depthResolveImages.clear();

        for (auto& memory : pLogicalSwapchain->depthResolveMemories)
        {
            if (memory != VK_NULL_HANDLE)
            {
                pLogicalSwapchain->pLogicalDevice->vkd.FreeMemory(
                    pLogicalSwapchain->pLogicalDevice->device, memory, nullptr);
            }
        }
        pLogicalSwapchain->depthResolveMemories.clear();
        pLogicalSwapchain->depthResolveInitialized.clear();

        pLogicalSwapchain->depthResolveDescriptorSets.clear();
        pLogicalSwapchain->depthResolveFormat = VK_FORMAT_UNDEFINED;
        pLogicalSwapchain->depthResolveExtent = {0, 0, 1};
        pLogicalSwapchain->depthResolveUsesShader = false;
    }

    void initializeDepthResolveLayout(LogicalSwapchain* pLogicalSwapchain, const DepthState& depth)
    {
        LogicalDevice* pLogicalDevice = pLogicalSwapchain->pLogicalDevice;
        const bool useShaderResolve = depth.observedLayout == VK_IMAGE_LAYOUT_GENERAL;
        pLogicalSwapchain->depthResolveUsesShader = useShaderResolve;
        pLogicalSwapchain->depthResolveFormat = useShaderResolve ? VK_FORMAT_R32_SFLOAT : depth.format;
        pLogicalSwapchain->depthResolveExtent = {depth.extent.width, depth.extent.height, 1};

        pLogicalSwapchain->depthResolveImages.clear();
        pLogicalSwapchain->depthResolveImageViews.clear();
        pLogicalSwapchain->depthResolveMemories.clear();
        pLogicalSwapchain->depthResolveInitialized.assign(pLogicalSwapchain->imageCount, false);
        pLogicalSwapchain->depthResolveImages.reserve(pLogicalSwapchain->imageCount);
        pLogicalSwapchain->depthResolveMemories.reserve(pLogicalSwapchain->imageCount);
        for (uint32_t i = 0; i < pLogicalSwapchain->imageCount; ++i)
        {
            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            std::vector<VkImage> images = createImages(pLogicalDevice,
                                                       1,
                                                       pLogicalSwapchain->depthResolveExtent,
                                                       pLogicalSwapchain->depthResolveFormat,
                                                       useShaderResolve
                                                           ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
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

        pLogicalSwapchain->depthResolveSampler = createSampler(pLogicalDevice);
        pLogicalSwapchain->depthResolveDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 1);

        VkDescriptorPoolSize imagePoolSize = {};
        imagePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = pLogicalSwapchain->imageCount;
        pLogicalSwapchain->depthResolveDescriptorPool = createDescriptorPool(pLogicalDevice, {imagePoolSize});

        std::vector<VkImageView> sourceViews(pLogicalSwapchain->imageCount, depth.imageView);
        pLogicalSwapchain->depthResolveDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
            pLogicalDevice,
            pLogicalSwapchain->depthResolveDescriptorPool,
            pLogicalSwapchain->depthResolveDescriptorSetLayout,
            {pLogicalSwapchain->depthResolveSampler},
            {sourceViews});

        pLogicalSwapchain->depthResolveRenderPass =
            createRenderPass(pLogicalDevice, pLogicalSwapchain->depthResolveFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pLogicalSwapchain->depthResolvePipelineLayout = createGraphicsPipelineLayout(
            pLogicalDevice, {pLogicalSwapchain->depthResolveDescriptorSetLayout});

        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        createShaderModule(pLogicalDevice, full_screen_triangle_vert, &vertexModule);
        createShaderModule(pLogicalDevice, depth_resolve_frag, &fragmentModule);

        VkExtent2D resolveExtent2D = {pLogicalSwapchain->depthResolveExtent.width, pLogicalSwapchain->depthResolveExtent.height};
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

    void ensureDepthResolveResources(LogicalSwapchain* pLogicalSwapchain, const DepthState& depth)
    {
        const bool useShaderResolve = depth.observedLayout == VK_IMAGE_LAYOUT_GENERAL;
        const VkFormat wantedResolveFormat = useShaderResolve ? VK_FORMAT_R32_SFLOAT : depth.format;
        if (!hasDepthState(depth) || depth.extent.width == 0 || depth.extent.height == 0)
        {
            destroyDepthResolveResources(pLogicalSwapchain);
            return;
        }

        if (pLogicalSwapchain->depthResolveFormat == wantedResolveFormat
            && pLogicalSwapchain->depthResolveExtent.width == depth.extent.width
            && pLogicalSwapchain->depthResolveExtent.height == depth.extent.height
            && pLogicalSwapchain->depthResolveExtent.depth == 1
            && pLogicalSwapchain->depthResolveUsesShader == useShaderResolve
            && pLogicalSwapchain->depthResolveImages.size() == pLogicalSwapchain->imageCount
            && pLogicalSwapchain->depthResolveImageViews.size() == pLogicalSwapchain->imageCount
            && pLogicalSwapchain->depthResolveMemories.size() == pLogicalSwapchain->imageCount
            && pLogicalSwapchain->depthResolveInitialized.size() == pLogicalSwapchain->imageCount
            && (!useShaderResolve
                || (pLogicalSwapchain->depthResolveDescriptorSets.size() == pLogicalSwapchain->imageCount
                    && pLogicalSwapchain->depthResolveRenderPass != VK_NULL_HANDLE
                    && pLogicalSwapchain->depthResolvePipelineLayout != VK_NULL_HANDLE
                    && pLogicalSwapchain->depthResolvePipeline != VK_NULL_HANDLE
                    && pLogicalSwapchain->depthResolveFramebuffers.size() == pLogicalSwapchain->imageCount)))
        {
            return;
        }

        destroyDepthResolveResources(pLogicalSwapchain);
        initializeDepthResolveLayout(pLogicalSwapchain, depth);
    }

    // Get depth state from logical device (returns null handles if no depth images)
    DepthState getDepthState(LogicalDevice* pLogicalDevice)
    {
        return pLogicalDevice->activeDepthState;
    }

    void updateDeviceDepthStateLocked(LogicalDevice* pLogicalDevice, const DepthState& depth, const char* reason)
    {
        if (sameDepthState(pLogicalDevice->activeDepthState, depth))
            return;

        pLogicalDevice->activeDepthState = depth;
        Logger::debug(std::string("active depth state updated from ") + reason + ": image=" + convertToString(depth.image)
                      + " view=" + convertToString(depth.imageView) + " format=" + convertToString(depth.format)
                      + " extent=" + std::to_string(depth.extent.width) + "x" + std::to_string(depth.extent.height)
                      + " observedLayout=" + convertToString(depth.observedLayout));

        auto metadataIt = pLogicalDevice->depthImageMetadata.find(depth.image);
        if (metadataIt != pLogicalDevice->depthImageMetadata.end())
        {
            Logger::debug(std::string("active depth state metadata from ") + reason
                          + ": image=" + convertToString(depth.image)
                          + " usage=0x" + formatHexU64(static_cast<uint64_t>(metadataIt->second.usage))
                          + " samples=" + convertToString(metadataIt->second.samples)
                          + " tiling=" + convertToString(metadataIt->second.tiling)
                          + " transient=" + std::string((metadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));
        }

        for (auto& [swapchainHandle, pLogicalSwapchain] : swapchainMap)
        {
            if (pLogicalSwapchain->pLogicalDevice != pLogicalDevice)
                continue;
            if (pLogicalSwapchain->commandBuffersEffect.empty())
                continue;

            reallocateCommandBuffers(pLogicalDevice, pLogicalSwapchain.get(), depth);
            Logger::debug("reallocated command buffers for swapchain " + convertToString(swapchainHandle) + " after depth change");
        }
    }

    // Helper to reallocate and rewrite command buffers for a swapchain
    void reallocateCommandBuffers(
        LogicalDevice* pLogicalDevice,
        LogicalSwapchain* pLogicalSwapchain,
        const DepthState& depth)
    {
        // Free existing command buffers
        if (!pLogicalSwapchain->commandBuffersEffect.empty())
        {
            pLogicalDevice->vkd.FreeCommandBuffers(
                pLogicalDevice->device, pLogicalDevice->commandPool,
                pLogicalSwapchain->commandBuffersEffect.size(),
                pLogicalSwapchain->commandBuffersEffect.data());
        }
        if (!pLogicalSwapchain->commandBuffersNoEffect.empty())
        {
            pLogicalDevice->vkd.FreeCommandBuffers(
                pLogicalDevice->device, pLogicalDevice->commandPool,
                pLogicalSwapchain->commandBuffersNoEffect.size(),
                pLogicalSwapchain->commandBuffersNoEffect.data());
        }

        ensureDepthResolveResources(pLogicalSwapchain, depth);

        // Allocate and write effect command buffers
        pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
        writeCommandBuffers(pLogicalDevice,
                            pLogicalSwapchain,
                            pLogicalSwapchain->effects,
                            pLogicalSwapchain->commandBuffersEffect);

        // Allocate and write no-effect command buffers
        pLogicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
        writeCommandBuffers(pLogicalDevice,
                            pLogicalSwapchain,
                            {pLogicalSwapchain->defaultTransfer},
                            pLogicalSwapchain->commandBuffersNoEffect);
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
            return;  // Already initialized

        // Ensure config directory exists for later saves
        {
            std::string baseDir = ConfigSerializer::getBaseConfigDir();
            if (!baseDir.empty())
                mkdir(baseDir.c_str(), 0755);
        }

        // Initialize settings manager (single source of truth for settings)
        settingsManager.initialize();

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
            // Auto-create profile for this game if needed, then load it
            activeProfileName = ConfigSerializer::getActiveProfile(detectedGameName);
            activeProfilePath = ConfigSerializer::getProfilePath(detectedGameName, activeProfileName);

            // Ensure the profile file exists
            struct stat st;
            if (stat(activeProfilePath.c_str(), &st) != 0)
            {
                // Profile doesn't exist yet — create it
                activeProfilePath = ConfigSerializer::ensureGameProfile(detectedGameName);
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

        // Also clear any overrides on the base config to avoid stale values
        if (pBaseConfig)
            pBaseConfig->clearOverrides();

        // Re-initialize registry with new config
        effectRegistry.initialize(pConfig.get());
        cachedParams.dirty = true;

        Logger::info("switched to config: " + configPath);
    }

    // Helper function to get available effects separated by source (uses cache)
    void getAvailableEffects(Config* pConfig,
                             std::vector<std::string>& currentConfigEffects,
                             std::vector<std::string>& defaultConfigEffects,
                             std::map<std::string, std::string>& effectPaths)
    {
        // Use cache if available and config hasn't changed
        if (cachedEffects.initialized && cachedEffects.configPath == pConfig->getConfigFilePath())
        {
            currentConfigEffects = cachedEffects.currentConfigEffects;
            defaultConfigEffects = cachedEffects.defaultConfigEffects;
            effectPaths = cachedEffects.effectPaths;
            return;
        }

        currentConfigEffects.clear();
        defaultConfigEffects.clear();
        effectPaths.clear();

        // Collect all known effect names (to avoid duplicates)
        std::set<std::string> knownEffects;

        // Get effect definitions from current config
        auto configEffects = pConfig->getEffectDefinitions();
        for (const auto& [name, path] : configEffects)
        {
            currentConfigEffects.push_back(name);
            effectPaths[name] = path;
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
                    effectPaths[name] = path;
                    knownEffects.insert(name);
                }
            }
        }

        // Auto-discover .fx files in all shader manager discovered paths
        ShaderManagerConfig shaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();
        for (const auto& shaderPath : shaderMgrConfig.discoveredShaderPaths)
        {
            try
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         shaderPath, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file())
                        continue;

                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".fx")
                        continue;

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

        // Update cache
        cachedEffects.currentConfigEffects = currentConfigEffects;
        cachedEffects.defaultConfigEffects = defaultConfigEffects;
        cachedEffects.effectPaths = effectPaths;
        cachedEffects.configPath = pConfig->getConfigFilePath();
        cachedEffects.initialized = true;
    }

    // Helper function to create effects for a swapchain
    // This centralizes the effect creation logic used by both initial swapchain setup and hot-reload
    void createEffectsForSwapchain(
        LogicalSwapchain* pLogicalSwapchain,
        LogicalDevice* pLogicalDevice,
        Config* pConfig,
        const std::vector<std::string>& effectStrings,
        bool checkEnabledState = true)
    {
        const bool useMutableFormat = pLogicalSwapchain->useMutableFormat;

        if (pLogicalSwapchain->imageCount == 0)
        {
            Logger::err("Cannot create effects for swapchain with imageCount=0");
            return;
        }

        const size_t requiredSlots = effectStrings.empty()
            ? 1
            : effectStrings.size() + (useMutableFormat ? 0u : 1u);
        const size_t requiredFakeImages = static_cast<size_t>(pLogicalSwapchain->imageCount) * requiredSlots;
        if (pLogicalSwapchain->fakeImages.size() < requiredFakeImages)
        {
            Logger::err("Insufficient fake images for effect chain: have "
                        + std::to_string(pLogicalSwapchain->fakeImages.size()) + ", need "
                        + std::to_string(requiredFakeImages));
            return;
        }

        VkFormat unormFormat = convertToUNORM(pLogicalSwapchain->format);
        VkFormat srgbFormat = convertToSRGB(pLogicalSwapchain->format);

        // If no effects, add pass-through so rendering still works
        if (effectStrings.empty())
        {
            std::vector<VkImage> firstImages(pLogicalSwapchain->fakeImages.begin(),
                                             pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount);
            pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new TransferEffect(
                pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent,
                firstImages, pLogicalSwapchain->images, pConfig)));
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
                secondImages = useMutableFormat
                    ? pLogicalSwapchain->images
                    : std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount,
                                           pLogicalSwapchain->fakeImages.end());
            }
            else
            {
                secondImages = std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 1),
                                                    pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 2));
            }

            // Check if effect should be skipped (disabled or failed)
            bool effectFailed = effectRegistry.hasEffectFailed(effectStrings[i]);
            bool effectDisabled = checkEnabledState && !effectRegistry.isEffectEnabled(effectStrings[i]);

            if (effectFailed || effectDisabled)
            {
                Logger::debug("effect " + std::string(effectFailed ? "failed" : "disabled") + ", using pass-through: " + effectStrings[i]);
                pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                    new TransferEffect(pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig)));
                continue;
            }

            // Get effect type from registry (handles instance names like "cas.2")
            std::string effectType = effectRegistry.getEffectType(effectStrings[i]);
            if (effectType.empty())
                effectType = effectStrings[i];

            // Create the appropriate effect type
            const auto* def = BuiltInEffects::instance().getDef(effectType);
            if (def)
            {
                // Sync registry parameter values to pConfig overrides so built-in
                // effects (which read from pConfig) see the latest UI-modified values.
                for (auto* param : effectRegistry.getParametersForEffect(effectStrings[i]))
                {
                    auto serialized = param->serialize();
                    for (const auto& [suffix, value] : serialized)
                    {
                        std::string key = suffix.empty() ? param->name : (param->name + suffix);
                        pConfig->setOverride(key, value);
                    }
                }

                // Wrap built-in effect creation in try-catch to handle failures gracefully
                try
                {
                    VkFormat format = def->usesSrgbFormat ? srgbFormat : unormFormat;
                    pLogicalSwapchain->effects.push_back(
                        def->factory(pLogicalDevice, format, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig));
                }
                catch (const std::exception& e)
                {
                    Logger::err("Failed to create built-in effect " + effectStrings[i] + ": " + e.what());
                    effectRegistry.setEffectError(effectStrings[i], e.what());
                    pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                        new TransferEffect(pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig)));
                }
            }
            else
            {
                // ReShade effect - wrap in try-catch + signal handler to handle compilation failures gracefully
                // The embedded reshadefx compiler can trigger SIGFPE/SIGABRT in edge cases
                std::string effectPath = effectRegistry.getEffectFilePath(effectStrings[i]);
                auto customDefs = effectRegistry.getPreprocessorDefs(effectStrings[i]);

                installCrashHandlers();
                bool signalCrash = false;
                if (sigsetjmp(signalJmpBuf, 1) != 0)
                {
                    // Returned here from signal handler (SIGFPE/SIGABRT)
                    signalJmpActive = 0;
                    signalCrash = true;
                    std::string sigName = (caughtSignal == SIGFPE) ? "SIGFPE" : "SIGABRT";
                    Logger::err("Caught " + sigName + " creating ReshadeEffect " + effectStrings[i]);
                    effectRegistry.setEffectError(effectStrings[i], sigName + " during shader compilation");
                    pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                        new TransferEffect(pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig)));
                }

                if (!signalCrash)
                {
                    signalJmpActive = 1;
                    try
                    {
                        auto reshadeEffect = std::make_shared<ReshadeEffect>(
                            pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent,
                            firstImages, secondImages, &effectRegistry, effectStrings[i], effectPath, customDefs);
                        pLogicalSwapchain->effects.push_back(reshadeEffect);
                        if (reshadeEffect->getOutputWrites() == 0)
                        {
                            Logger::debug("deterministic forwarding for zero-output-write effect: " + effectStrings[i]);
                            pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                                new TransferEffect(pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent,
                                                   firstImages, secondImages, pConfig)));
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Logger::err("Failed to create ReshadeEffect " + effectStrings[i] + ": " + e.what());
                        effectRegistry.setEffectError(effectStrings[i], e.what());
                        pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                            new TransferEffect(pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig)));
                    }
                    signalJmpActive = 0;
                }
            }
        }

        // If device doesn't support mutable format, add final transfer to swapchain
        if (!useMutableFormat)
        {
            pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new TransferEffect(
                pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent,
                std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount, pLogicalSwapchain->fakeImages.end()),
                pLogicalSwapchain->images, pConfig)));
        }
    }

    // Helper function to reload effects for a swapchain (for hot-reload)
    void reloadEffectsForSwapchain(LogicalSwapchain* pLogicalSwapchain, Config* pConfig,
                                   const std::vector<std::string>& activeEffects = {})
    {
        LogicalDevice* pLogicalDevice = pLogicalSwapchain->pLogicalDevice;

        // Wait for GPU to finish
        pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);

        // Clear effects (command buffers will be freed by reallocateCommandBuffers)
        pLogicalSwapchain->effects.clear();
        pLogicalSwapchain->defaultTransfer.reset();

        // Use provided active effects list directly - no fallback to config
        // Registry is the single source of truth (initialized at first swapchain creation)
        std::vector<std::string> effectStrings = activeEffects;

        // Check if we have enough fake images for the effects
        // Fake images are allocated at swapchain creation based on maxEffectSlots
        if (effectStrings.size() > pLogicalSwapchain->maxEffectSlots)
        {
            Logger::warn("Cannot add more effects than maxEffectSlots (" +
                        std::to_string(effectStrings.size()) + " > " + std::to_string(pLogicalSwapchain->maxEffectSlots) +
                        "). Increase maxEffects in config.");
            effectStrings.resize(pLogicalSwapchain->maxEffectSlots);
        }

        Logger::info("reloading " + std::to_string(effectStrings.size()) + " effects");

        // Create effects using centralized helper
        createEffectsForSwapchain(pLogicalSwapchain, pLogicalDevice, pConfig, effectStrings, true);

        // Create default transfer effect (needed for no-effect command buffers)
        pLogicalSwapchain->defaultTransfer = std::shared_ptr<Effect>(new TransferEffect(
            pLogicalDevice,
            pLogicalSwapchain->format,
            pLogicalSwapchain->imageExtent,
            std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin(), pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount),
            pLogicalSwapchain->images,
            pConfig));

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

        getAvailableEffects(pConfig.get(), overlayState.currentConfigEffects,
                            overlayState.defaultConfigEffects, overlayState.effectPaths);
        overlayState.configPath = pConfig->getConfigFilePath();

        // Cache the filename extraction — config path rarely changes
        static std::string cachedConfigPath;
        static std::string cachedConfigName;
        if (overlayState.configPath != cachedConfigPath)
        {
            cachedConfigPath = overlayState.configPath;
            cachedConfigName = std::filesystem::path(cachedConfigPath).filename().string();
        }
        overlayState.configName = cachedConfigName;
        overlayState.effectsEnabled = effectsEnabled;

        // Ensure all selected effects are in the registry
        for (const auto& effectName : pLogicalDevice->imguiOverlay->getSelectedEffects())
        {
            if (effectRegistry.hasEffect(effectName))
                continue;
            auto pathIt = overlayState.effectPaths.find(effectName);
            std::string effectPath = (pathIt != overlayState.effectPaths.end()) ? pathIt->second : "";
            effectRegistry.ensureEffect(effectName, effectPath);
        }

        // Parameters now read directly from EffectRegistry, no need to pass via state
        pLogicalDevice->imguiOverlay->updateState(std::move(overlayState));
    }

    // Submit overlay command buffer if visible, returns semaphore to wait on
    VkResult submitOverlayFrame(LogicalDevice* pLogicalDevice, LogicalSwapchain* pSwapchain,
                                uint32_t index, VkSemaphore& outSemaphore)
    {
        outSemaphore = pSwapchain->semaphores[index];  // Default: wait on effects semaphore

        if (!pLogicalDevice->imguiOverlay)
            return VK_SUCCESS;

        VkCommandBuffer overlayCmd = pLogicalDevice->imguiOverlay->recordFrame(
            index, pSwapchain->imageViews[index],
            pSwapchain->imageExtent.width, pSwapchain->imageExtent.height);

        if (overlayCmd == VK_NULL_HANDLE)
            return VK_SUCCESS;

        VkPipelineStageFlags overlayWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo overlaySubmit = {};
        overlaySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        overlaySubmit.waitSemaphoreCount = 1;
        overlaySubmit.pWaitSemaphores = &pSwapchain->semaphores[index];
        overlaySubmit.pWaitDstStageMask = &overlayWaitStage;
        overlaySubmit.commandBufferCount = 1;
        overlaySubmit.pCommandBuffers = &overlayCmd;
        overlaySubmit.signalSemaphoreCount = 1;
        overlaySubmit.pSignalSemaphores = &pSwapchain->overlaySemaphores[index];

        // Use fence to track command buffer completion (prevents reuse while in flight)
        VkFence overlayFence = pLogicalDevice->imguiOverlay->getCommandBufferFence(index);
        VkResult vr = pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &overlaySubmit, overlayFence);
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

        const bool supportsMutableFormat = hasDeviceExtension("VK_KHR_swapchain_mutable_format");
        const bool supportsNvCheckpointExt = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        const bool supportsNvDiagnosticsConfigExt = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        const bool supportsDeviceFaultExt = hasDeviceExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        const bool gpuCrashDiagnosticsRequested = isGpuCrashDiagEnabled();
        if (supportsMutableFormat)
            Logger::debug("device supports VK_KHR_swapchain_mutable_format");

        bool hasMutableEnvOverride = false;
        bool mutableRequested = getMutableSwapchainEnvOverride(hasMutableEnvOverride, true);

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
            Logger::info(std::string("Mutable swapchain device extension set via VKSHADE_ENABLE_MUTABLE_SWAPCHAIN=") + (mutableRequested ? "1" : "0"));
        }
        if (deviceProps.apiVersion < VK_API_VERSION_1_2 || instanceVersionMap[GetKey(physicalDevice)] < VK_API_VERSION_1_2)
        {
            addUniqueCString(enabledExtensionNames, "VK_KHR_image_format_list");
        }

        bool enableNvCheckpoints = false;
        bool enableNvDiagnosticsConfig = false;
        bool enableDeviceFault = false;
        bool enableDeviceFaultFeature = false;
        VkPhysicalDeviceFaultFeaturesEXT faultFeatureEnable = {};
        VkDeviceDiagnosticsConfigCreateInfoNV diagnosticsConfigInfo = {};
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

        // Active needed Features
        VkPhysicalDeviceFeatures deviceFeatures = {};
        if (modifiedCreateInfo.pEnabledFeatures)
        {
            deviceFeatures = *(modifiedCreateInfo.pEnabledFeatures);
        }
        deviceFeatures.shaderImageGatherExtended = VK_TRUE;
        deviceFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
        deviceFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        modifiedCreateInfo.pEnabledFeatures      = &deviceFeatures;

        void* extendedDevicePNext = const_cast<void*>(modifiedCreateInfo.pNext);

        if (enableDeviceFault && !pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT))
        {
            VkPhysicalDeviceFaultFeaturesEXT faultFeatureQuery = {};
            faultFeatureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 featureQuery = {};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &faultFeatureQuery;
            if (instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceFeatures2)
            {
                instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceFeatures2(physicalDevice, &featureQuery);
                if (faultFeatureQuery.deviceFault == VK_TRUE)
                {
                    faultFeatureEnable = {};
                    faultFeatureEnable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
                    faultFeatureEnable.deviceFault = VK_TRUE;
                    faultFeatureEnable.pNext = extendedDevicePNext;
                    extendedDevicePNext = &faultFeatureEnable;
                    enableDeviceFaultFeature = true;
                }
                else
                {
                    Logger::warn("VK_EXT_device_fault is present but deviceFault feature is not supported");
                }
            }
            else
            {
                Logger::warn("Cannot query VkPhysicalDeviceFaultFeaturesEXT (vkGetPhysicalDeviceFeatures2 unavailable)");
            }
        }
        else if (enableDeviceFault)
        {
            enableDeviceFaultFeature = true;
        }

        if (enableNvDiagnosticsConfig && !pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV))
        {
            diagnosticsConfigInfo = {};
            diagnosticsConfigInfo.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
            diagnosticsConfigInfo.flags =
                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
                | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
            diagnosticsConfigInfo.pNext = extendedDevicePNext;
            extendedDevicePNext = &diagnosticsConfigInfo;
        }

        modifiedCreateInfo.pNext = extendedDevicePNext;

        VkResult ret = createFunc(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);

        if (ret != VK_SUCCESS)
            return ret;

        std::shared_ptr<LogicalDevice> pLogicalDevice(new LogicalDevice());
        pLogicalDevice->vki                   = instanceDispatchMap[GetKey(physicalDevice)];
        pLogicalDevice->device                = *pDevice;
        pLogicalDevice->physicalDevice        = physicalDevice;
        pLogicalDevice->instance              = instanceMap[GetKey(physicalDevice)];
        pLogicalDevice->queue                 = VK_NULL_HANDLE;
        pLogicalDevice->queueFamilyIndex      = 0;
        pLogicalDevice->commandPool           = VK_NULL_HANDLE;
        pLogicalDevice->supportsMutableFormat = supportsMutableFormat && mutableRequested;
        pLogicalDevice->isNvidiaGpu           = (deviceProps.vendorID == 0x10DE);
        pLogicalDevice->gpuCrashDiagnosticsEnabled = gpuCrashDiagnosticsRequested;
        pLogicalDevice->supportsNvDiagnosticCheckpoints = enableNvCheckpoints;
        pLogicalDevice->supportsNvDiagnosticsConfig = enableNvDiagnosticsConfig;
        pLogicalDevice->supportsDeviceFaultExt = enableDeviceFault && enableDeviceFaultFeature;

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

            Logger::info(
                std::string("GPU crash diagnostics enabled (NV checkpoints=")
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
                commandPoolCreateInfo.flags            = 0;
                commandPoolCreateInfo.queueFamilyIndex = queueInfo.queueFamilyIndex;

                Logger::debug("Found graphics capable queue");
                pLogicalDevice->vkd.CreateCommandPool(pLogicalDevice->device, &commandPoolCreateInfo, nullptr, &pLogicalDevice->commandPool);
                pLogicalDevice->queueFamilyIndex = queueInfo.queueFamilyIndex;

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

        // Clean up Wayland input resources (no-op if not initialized)
        cleanupWaylandKeyboard();
        cleanupWaylandMouse();

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

        VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;

        VkFormat format = modifiedCreateInfo.imageFormat;

        VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
        VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;
        Logger::debug(std::to_string(srgbFormat) + " " + std::to_string(unormFormat));

        VkFormat formats[] = {unormFormat, srgbFormat};

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        bool hasMutableEnvOverride = false;
        bool useMutableFormat = getMutableSwapchainEnvOverride(hasMutableEnvOverride, pLogicalDevice->supportsMutableFormat);
        if (useMutableFormat && !pLogicalDevice->supportsMutableFormat)
        {
            Logger::warn("Mutable swapchain forced on via VKSHADE_ENABLE_MUTABLE_SWAPCHAIN, but device does not support it. Falling back to disabled.");
            useMutableFormat = false;
        }
        else if (useMutableFormat &&
                 pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
        {
            Logger::warn("Application already provides VkImageFormatListCreateInfo in swapchain pNext; disabling mutable override for compatibility.");
            useMutableFormat = false;
        }
        else if (hasMutableEnvOverride)
        {
            Logger::info(std::string("Mutable swapchain explicitly set via VKSHADE_ENABLE_MUTABLE_SWAPCHAIN=") + (useMutableFormat ? "1" : "0"));
        }

        if (useMutableFormat)
        {
            // Keep application-requested usage bits and add the ones vkShade needs.
            modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                             ; // mutable path renders final pass directly into swapchain
            modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
            // TODO what if the application already uses multiple formats for the swapchain?

            imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
            imageFormatListCreateInfo.pNext           = modifiedCreateInfo.pNext;
            imageFormatListCreateInfo.viewFormatCount = (srgbFormat == unormFormat) ? 1 : 2;
            imageFormatListCreateInfo.pViewFormats    = formats;

            modifiedCreateInfo.pNext = &imageFormatListCreateInfo;
        }

        // Keep application-provided usage bits unchanged on the non-mutable path.
        // Some drivers/apps are sensitive to swapchain usage mutation.

        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "vkShade diag: CreateSwapchainKHR mutable=%d reqUsage=0x%x finalUsage=0x%x reqFlags=0x%x finalFlags=0x%x format=%u extent=%ux%u minImages=%u\n",
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
        pLogicalSwapchain->imageExtent         = modifiedCreateInfo.imageExtent;
        pLogicalSwapchain->format              = modifiedCreateInfo.imageFormat;
        pLogicalSwapchain->imageCount          = 0;
        pLogicalSwapchain->useMutableFormat    = useMutableFormat;

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
            return *pCount < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }

        const uint32_t requestedImageCapacity = *pCount;
        uint32_t realImageCount = 0;
        VkResult getCountResult = pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &realImageCount, nullptr);
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
        uint32_t fetchedImageCount = realImageCount;
        VkResult getImagesResult = pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &fetchedImageCount, realImages.data());
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
        pLogicalSwapchain->images = std::move(realImages);

        // Create image views for overlay rendering
        pLogicalSwapchain->imageViews.resize(pLogicalSwapchain->imageCount);
        for (uint32_t i = 0; i < pLogicalSwapchain->imageCount; i++)
        {
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = pLogicalSwapchain->images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = pLogicalSwapchain->format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            VkResult viewResult = pLogicalDevice->vkd.CreateImageView(pLogicalDevice->device, &viewInfo, nullptr, &pLogicalSwapchain->imageViews[i]);
            if (viewResult != VK_SUCCESS)
                Logger::err("Failed to create swapchain image view " + std::to_string(i) + ": " + std::to_string(viewResult));
        }

        // Initialize registry from config on first run (before calculating effect slots)
        bool isFirstRun = !effectRegistry.isInitializedFromConfig();
        if (isFirstRun)
            effectRegistry.initializeSelectedEffectsFromConfig();

        const auto& selectedEffects = effectRegistry.getSelectedEffects();

        // Allow dynamic effect loading by allocating for more effects than configured.
        // Clamp maxEffects to a safe range to avoid pathological allocations.
        int32_t maxEffects = std::clamp(settingsManager.getMaxEffects(), 1, 200);
        size_t effectSlots = std::max(selectedEffects.size(), static_cast<size_t>(maxEffects));
        pLogicalSwapchain->maxEffectSlots = effectSlots;

        // create 1 more set of images when we can't use the swapchain itself
        const uint64_t slotCount = static_cast<uint64_t>(effectSlots) + (pLogicalSwapchain->useMutableFormat ? 0u : 1u);
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

        if (!isFirstRun && !selectedEffects.empty())
        {
            // Resize with effects - use pass-through and debounce for smooth resize
            Logger::debug("using pass-through during resize, will restore effects after debounce");
            std::vector<VkImage> firstImages(pLogicalSwapchain->fakeImages.begin(),
                                             pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount);
            pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new TransferEffect(
                pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageExtent,
                firstImages, pLogicalSwapchain->images, pConfig.get())));

            resizeDebounce.pending = true;
            resizeDebounce.lastResizeTime = std::chrono::steady_clock::now();
        }
        else
        {
            // First run OR empty effects - create effects from registry
            createEffectsForSwapchain(pLogicalSwapchain, pLogicalDevice, pConfig.get(), selectedEffects, true);
        }

        DepthState depth = getDepthState(pLogicalDevice);

        Logger::debug("selected effect count: " + std::to_string(selectedEffects.size()));
        Logger::debug("effect count: " + std::to_string(pLogicalSwapchain->effects.size()));

        pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
        Logger::debug("allocated ComandBuffers " + std::to_string(pLogicalSwapchain->commandBuffersEffect.size()) + " for swapchain "
                      + convertToString(swapchain));

        ensureDepthResolveResources(pLogicalSwapchain, depth);
        writeCommandBuffers(pLogicalDevice,
                            pLogicalSwapchain,
                            pLogicalSwapchain->effects,
                            pLogicalSwapchain->commandBuffersEffect);
        Logger::debug("wrote CommandBuffers");

        pLogicalSwapchain->semaphores = createSemaphores(pLogicalDevice, pLogicalSwapchain->imageCount);
        pLogicalSwapchain->overlaySemaphores = createSemaphores(pLogicalDevice, pLogicalSwapchain->imageCount);
        Logger::debug("created semaphores");
        for (unsigned int i = 0; i < pLogicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(pLogicalSwapchain->commandBuffersEffect[i]));
        }
        Logger::trace("vkGetSwapchainImagesKHR");

        pLogicalSwapchain->defaultTransfer = std::shared_ptr<Effect>(new TransferEffect(
            pLogicalDevice,
            pLogicalSwapchain->format,
            pLogicalSwapchain->imageExtent,
            std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin(), pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount),
            pLogicalSwapchain->images,
            pConfig.get()));

        pLogicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);

        writeCommandBuffers(pLogicalDevice,
                            pLogicalSwapchain,
                            {pLogicalSwapchain->defaultTransfer},
                            pLogicalSwapchain->commandBuffersNoEffect);

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
                pLogicalDevice, pLogicalSwapchain->format, pLogicalSwapchain->imageCount,
                pLogicalDevice->overlayPersistentState.get());
            // Set the effect registry pointer (single source of truth for enabled states)
            pLogicalDevice->imguiOverlay->setEffectRegistry(&effectRegistry);

            // Set game/profile info for auto-save
            pLogicalDevice->imguiOverlay->setGameProfile(detectedGameName, activeProfileName, activeProfilePath);

            // Initialize input blocking (grabs all input when overlay is visible)
            static bool inputBlockerInited = false;
            if (!inputBlockerInited)
            {
                initInputBlocker(settingsManager.getOverlayBlockInput());
                if (pLogicalDevice->imguiOverlay)
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
        return requestedImageCapacity < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
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

        // Mark new input frame so dispatch deduplication resets
        beginWaylandInputFrame();
        beginKeyboardInputFrame();

        // Keybindings - read from settingsManager (can be updated when settings are saved)
        static uint32_t keySymbol = convertToKeySym(settingsManager.getToggleKey());
        static uint32_t reloadKeySymbol = convertToKeySym(settingsManager.getReloadKey());
        static uint32_t overlayKeySymbol = convertToKeySym(settingsManager.getOverlayKey());
        static bool initLogged = false;

        static bool pressed       = false;
        static bool presentEffect = settingsManager.getEnableOnLaunch();
        static bool reloadPressed = false;
        static bool overlayPressed = false;

        std::shared_ptr<LogicalDevice> pLogicalDeviceShared;
        bool presentEffectSnapshot = false;
        std::vector<std::shared_ptr<LogicalSwapchain>> presentSwapchains;
        std::vector<uint32_t> presentIndices;

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
                keySymbol = convertToKeySym(settingsManager.getToggleKey());
                reloadKeySymbol = convertToKeySym(settingsManager.getReloadKey());
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
                cachedEffects.initialized = false;  // Force re-scan of available effects
                pDeviceForSettings->imguiOverlay->clearShaderPathsChanged();
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
                    std::vector<std::string> newEffects = pConfig->getOption<std::vector<std::string>>("effects", {});
                    std::vector<std::string> disabledEffects = pConfig->getOption<std::vector<std::string>>("disabledEffects", {});
                    pLogicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                    pLogicalDevice->imguiOverlay->clearPendingConfig();
                    pLogicalDevice->imguiOverlay->markDirty();  // Defer reload via debounce
                }
                else
                {
                    pConfig->reload();
                    cachedEffects.initialized = false;
                    cachedParams.dirty = true;

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
                auto resizeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - resizeDebounce.lastResizeTime).count();

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
            updateOverlayState(pLogicalDevice, presentEffect);
            presentEffectSnapshot = presentEffect;
            pLogicalDeviceShared = devIt->second;

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
                uint32_t index = pPresentInfo->pImageIndices[i];
                if (index >= pLogicalSwapchain->imageCount
                    || index >= pLogicalSwapchain->semaphores.size()
                    || index >= pLogicalSwapchain->overlaySemaphores.size()
                    || index >= pLogicalSwapchain->imageViews.size())
                {
                    Logger::err("present image index out of bounds for swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                const auto& commandBuffers = presentEffectSnapshot
                    ? pLogicalSwapchain->commandBuffersEffect
                    : pLogicalSwapchain->commandBuffersNoEffect;
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
        static thread_local std::vector<VkSemaphore> presentSemaphores;
        static thread_local std::vector<VkPipelineStageFlags> waitStages;
        presentSemaphores.clear();
        presentSemaphores.reserve(pPresentInfo->swapchainCount);
        waitStages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
        {
            LogicalSwapchain* pLogicalSwapchain = presentSwapchains[i].get();
            uint32_t index = presentIndices[i];

            // Update effect uniforms only when effects are active (saves CPU+GPU when off)
            if (presentEffectSnapshot)
            {
                for (auto& effect : pLogicalSwapchain->effects)
                    effect->updateEffect();
            }

            const auto& commandBuffers = presentEffectSnapshot
                ? pLogicalSwapchain->commandBuffersEffect
                : pLogicalSwapchain->commandBuffersNoEffect;

            Logger::debug("present path: effects="
                          + std::string(presentEffectSnapshot ? "on" : "off")
                          + " swapchain=" + convertToString(pPresentInfo->pSwapchains[i])
                          + " imageIndex=" + std::to_string(index)
                          + " commandBuffer=" + convertToString(commandBuffers[index]));

            // Submit effect command buffer
            VkSubmitInfo submitInfo = {};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.waitSemaphoreCount = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
            submitInfo.pWaitSemaphores    = i == 0 ? pPresentInfo->pWaitSemaphores : nullptr;
            submitInfo.pWaitDstStageMask  = i == 0 ? waitStages.data() : nullptr;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &commandBuffers[index];
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores    = &pLogicalSwapchain->semaphores[index];

            VkResult vr = pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &submitInfo, VK_NULL_HANDLE);
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

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateRenderPass(VkDevice device,
                                                            const VkRenderPassCreateInfo* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator,
                                                            VkRenderPass* pRenderPass)
    {
        scoped_lock l(globalLock);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!vkShade::settingsManager.getDepthCapture() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
            return pLogicalDevice->vkd.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);

        std::vector<VkAttachmentDescription> attachments(
            pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
            return pLogicalDevice->vkd.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);

        VkRenderPassCreateInfo createInfo = *pCreateInfo;
        createInfo.pAttachments = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        return pLogicalDevice->vkd.CreateRenderPass(device, &createInfo, pAllocator, pRenderPass);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateRenderPass2(VkDevice device,
                                                             const VkRenderPassCreateInfo2* pCreateInfo,
                                                             const VkAllocationCallbacks* pAllocator,
                                                             VkRenderPass* pRenderPass)
    {
        scoped_lock l(globalLock);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!vkShade::settingsManager.getDepthCapture() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
            return pLogicalDevice->vkd.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);

        std::vector<VkAttachmentDescription2> attachments(
            pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
            return pLogicalDevice->vkd.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);

        VkRenderPassCreateInfo2 createInfo = *pCreateInfo;
        createInfo.pAttachments = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo2 with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        return pLogicalDevice->vkd.CreateRenderPass2(device, &createInfo, pAllocator, pRenderPass);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateRenderPass2KHR(VkDevice device,
                                                                const VkRenderPassCreateInfo2* pCreateInfo,
                                                                const VkAllocationCallbacks* pAllocator,
                                                                VkRenderPass* pRenderPass)
    {
        scoped_lock l(globalLock);
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (!vkShade::settingsManager.getDepthCapture() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
            return pLogicalDevice->vkd.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);

        std::vector<VkAttachmentDescription2> attachments(
            pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
            return pLogicalDevice->vkd.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);

        VkRenderPassCreateInfo2 createInfo = *pCreateInfo;
        createInfo.pAttachments = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo2KHR with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        return pLogicalDevice->vkd.CreateRenderPass2KHR(device, &createInfo, pAllocator, pRenderPass);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateImage(VkDevice                     device,
                                                       const VkImageCreateInfo*     pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkImage*                     pImage)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (isDepthFormat(pCreateInfo->format) && pCreateInfo->samples == VK_SAMPLE_COUNT_1_BIT
            && ((pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            DepthImageMetadata metadata = {
                pCreateInfo->usage,
                pCreateInfo->samples,
                pCreateInfo->tiling,
            };
            Logger::debug("detected depth image with format: " + convertToString(pCreateInfo->format));
            Logger::debug(std::to_string(pCreateInfo->extent.width) + "x" + std::to_string(pCreateInfo->extent.height));
            Logger::debug(
                std::to_string((pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));

            VkImageCreateInfo modifiedCreateInfo = *pCreateInfo;
            modifiedCreateInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VkResult result = pLogicalDevice->vkd.CreateImage(device, &modifiedCreateInfo, pAllocator, pImage);
            if (result != VK_SUCCESS)
                return result;

            pLogicalDevice->depthImages.push_back(*pImage);
            pLogicalDevice->depthFormats.push_back(pCreateInfo->format);
            pLogicalDevice->depthImageExtents[*pImage] = pCreateInfo->extent;
            pLogicalDevice->depthImageMetadata[*pImage] = metadata;
            Logger::debug("tracked depth image metadata: image=" + convertToString(*pImage)
                          + " usage=0x" + formatHexU64(static_cast<uint64_t>(metadata.usage))
                          + " samples=" + convertToString(metadata.samples)
                          + " tiling=" + convertToString(metadata.tiling)
                          + " transient=" + std::string((metadata.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));

            return result;
        }
        else
        {
            return pLogicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);
        }
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.BindImageMemory(device, image, memory, memoryOffset);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        VkResult result = pLogicalDevice->vkd.BindImageMemory(device, image, memory, memoryOffset);

        auto it = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), image);
        if (it == pLogicalDevice->depthImages.end())
            return result;

        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateImageView(VkDevice device,
                                                           const VkImageViewCreateInfo* pCreateInfo,
                                                           const VkAllocationCallbacks* pAllocator,
                                                           VkImageView* pView)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, pView);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        VkResult result = pLogicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, pView);
        if (result != VK_SUCCESS)
            return result;

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImage(pLogicalDevice, pCreateInfo->image);
        if (snapshotTarget.swapchain != VK_NULL_HANDLE)
        {
            Logger::debug("tracked snapshot target image view: appView=" + convertToString(*pView)
                          + " image=" + convertToString(pCreateInfo->image)
                          + " swapchain=" + convertToString(snapshotTarget.swapchain)
                          + " imageIndex=" + std::to_string(snapshotTarget.imageIndex));
            pLogicalDevice->snapshotTargetViewStates[*pView] = snapshotTarget;
        }

        if ((pCreateInfo->subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0)
            return result;

        auto imageIt = std::find(pLogicalDevice->depthImages.begin(), pLogicalDevice->depthImages.end(), pCreateInfo->image);
        if (imageIt == pLogicalDevice->depthImages.end())
            return result;

        size_t i = std::distance(pLogicalDevice->depthImages.begin(), imageIt);
        VkImageView sampledView = getOrCreateTrackedDepthSampleViewLocked(pLogicalDevice, pCreateInfo->image, pLogicalDevice->depthFormats[i]);
        Logger::debug("tracked depth image view created: appView=" + convertToString(*pView)
                      + " sampledView=" + convertToString(sampledView)
                      + " image=" + convertToString(pCreateInfo->image)
                      + " aspect=" + convertToString(pCreateInfo->subresourceRange.aspectMask));
        DepthState depth;
        depth.image = pCreateInfo->image;
        depth.imageView = sampledView;
        depth.format = pLogicalDevice->depthFormats[i];
        auto extentIt = pLogicalDevice->depthImageExtents.find(depth.image);
        if (extentIt != pLogicalDevice->depthImageExtents.end())
            depth.extent = extentIt->second;
        pLogicalDevice->depthViewStates[*pView] = depth;

        return result;
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_DestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator)
    {
        if (!imageView)
            return;

        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            pLogicalDevice->vkd.DestroyImageView(device, imageView, pAllocator);
            return;
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        pLogicalDevice->snapshotTargetViewStates.erase(imageView);
        pLogicalDevice->depthViewStates.erase(imageView);
        clearTrackedDepthScopesLocked(pLogicalDevice, [imageView](const DepthState& state) { return state.imageView == imageView; });

        for (auto it = pLogicalDevice->framebufferDepthStates.begin(); it != pLogicalDevice->framebufferDepthStates.end();)
        {
            if (it->second.imageView == imageView)
                it = pLogicalDevice->framebufferDepthStates.erase(it);
            else
                ++it;
        }

        if (pLogicalDevice->activeDepthState.imageView == imageView)
        {
            pLogicalDevice->activeDepthState = {};
            updateDeviceDepthStateLocked(pLogicalDevice, getDepthState(pLogicalDevice), "DestroyImageView");
        }

        pLogicalDevice->vkd.DestroyImageView(device, imageView, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateFramebuffer(VkDevice device,
                                                             const VkFramebufferCreateInfo* pCreateInfo,
                                                             const VkAllocationCallbacks* pAllocator,
                                                             VkFramebuffer* pFramebuffer)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            return pLogicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        VkResult result = pLogicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        if (result != VK_SUCCESS)
            return result;

        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++)
        {
            auto it = pLogicalDevice->depthViewStates.find(pCreateInfo->pAttachments[i]);
            if (it != pLogicalDevice->depthViewStates.end())
            {
                Logger::debug("tracked depth framebuffer attachment: framebuffer=" + convertToString(*pFramebuffer)
                              + " attachmentView=" + convertToString(pCreateInfo->pAttachments[i])
                              + " sampledView=" + convertToString(it->second.imageView)
                              + " image=" + convertToString(it->second.image));
                pLogicalDevice->framebufferDepthStates[*pFramebuffer] = it->second;
                break;
            }
        }

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImageViews(
            pLogicalDevice, pCreateInfo->pAttachments, pCreateInfo->attachmentCount);
        if (snapshotTarget.swapchain != VK_NULL_HANDLE)
        {
            Logger::debug("tracked framebuffer snapshot target: framebuffer=" + convertToString(*pFramebuffer)
                          + " swapchain=" + convertToString(snapshotTarget.swapchain)
                          + " imageIndex=" + std::to_string(snapshotTarget.imageIndex));
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

        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
            pLogicalDevice->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
            return;
        }

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        pLogicalDevice->framebufferDepthStates.erase(framebuffer);
        pLogicalDevice->framebufferSnapshotTargets.erase(framebuffer);
        pLogicalDevice->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                          const VkRenderPassBeginInfo* pRenderPassBegin,
                                                          VkSubpassContents contents)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                beginTrackedDepthScope(pLogicalDevice,
                                       commandBuffer,
                                       selectDepthStateFromRenderPassBegin(pLogicalDevice, pRenderPassBegin),
                                       selectDepthSnapshotTargetFromRenderPassBegin(pLogicalDevice, pRenderPassBegin));
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                                           const VkRenderPassBeginInfo* pRenderPassBegin,
                                                           const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                beginTrackedDepthScope(pLogicalDevice,
                                       commandBuffer,
                                       selectDepthStateFromRenderPassBegin(pLogicalDevice, pRenderPassBegin),
                                       selectDepthSnapshotTargetFromRenderPassBegin(pLogicalDevice, pRenderPassBegin));
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                                              const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                beginTrackedDepthScope(pLogicalDevice,
                                       commandBuffer,
                                       selectDepthStateFromRenderPassBegin(pLogicalDevice, pRenderPassBegin),
                                       selectDepthSnapshotTargetFromRenderPassBegin(pLogicalDevice, pRenderPassBegin));
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    void seedTrackedDepthScopeFromRenderingInfo(LogicalDevice* pLogicalDevice,
                                                VkCommandBuffer commandBuffer,
                                                const VkRenderingInfo* pRenderingInfo)
    {
        beginTrackedDepthScope(pLogicalDevice,
                               commandBuffer,
                               selectDepthStateFromRenderingInfo(pLogicalDevice, pRenderingInfo),
                               selectDepthSnapshotTargetFromRenderingInfo(pLogicalDevice, pRenderingInfo));
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRendering(commandBuffer, pRenderingInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
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
                VkRenderingInfo renderingInfo = *pRenderingInfo;
                VkRenderingAttachmentInfo depthAttachment = {};
                VkRenderingAttachmentInfo stencilAttachment = {};
                bool changed = false;

                if (pRenderingInfo->pDepthAttachment)
                {
                    depthAttachment = *pRenderingInfo->pDepthAttachment;
                    changed = forceDepthAttachmentStoreOp(depthAttachment, false) || changed;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                }

                if (pRenderingInfo->pStencilAttachment)
                {
                    stencilAttachment = *pRenderingInfo->pStencilAttachment;
                    changed = forceDepthAttachmentStoreOp(stencilAttachment, true) || changed;
                    renderingInfo.pStencilAttachment = &stencilAttachment;
                }

                if (changed)
                {
                    Logger::debug("forcing depth attachment storeOp=STORE for VkRenderingInfo on commandBuffer="
                                  + convertToString(commandBuffer));
                    pLogicalDevice->vkd.CmdBeginRendering(commandBuffer, &renderingInfo);
                    return;
                }
            }

            pLogicalDevice->vkd.CmdBeginRendering(commandBuffer, pRenderingInfo);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
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
                VkRenderingInfo renderingInfo = *pRenderingInfo;
                VkRenderingAttachmentInfo depthAttachment = {};
                VkRenderingAttachmentInfo stencilAttachment = {};
                bool changed = false;

                if (pRenderingInfo->pDepthAttachment)
                {
                    depthAttachment = *pRenderingInfo->pDepthAttachment;
                    changed = forceDepthAttachmentStoreOp(depthAttachment, false) || changed;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                }

                if (pRenderingInfo->pStencilAttachment)
                {
                    stencilAttachment = *pRenderingInfo->pStencilAttachment;
                    changed = forceDepthAttachmentStoreOp(stencilAttachment, true) || changed;
                    renderingInfo.pStencilAttachment = &stencilAttachment;
                }

                if (changed)
                {
                    Logger::debug("forcing depth attachment storeOp=STORE for VkRenderingInfoKHR on commandBuffer="
                                  + convertToString(commandBuffer));
                    pLogicalDevice->vkd.CmdBeginRenderingKHR(commandBuffer, &renderingInfo);
                    return;
                }
            }

            pLogicalDevice->vkd.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass(commandBuffer);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        DepthState promotedDepthState = {};
        DepthSnapshotTarget snapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice,
                                     commandBuffer,
                                     "CmdEndRenderPass",
                                     &promotedDepthState,
                                     &snapshotTarget);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);
            if (snapshotTarget.swapchain != VK_NULL_HANDLE && hasDepthState(promotedDepthState))
                recordDepthResolveSnapshotForCommandBuffer(pLogicalDevice, commandBuffer, promotedDepthState, &snapshotTarget);
            else if (hasDepthState(promotedDepthState) && sameDepthState(pLogicalDevice->activeDepthState, promotedDepthState))
                recordDepthResolveSnapshotForAllSwapchains(pLogicalDevice, commandBuffer, promotedDepthState);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        DepthState promotedDepthState = {};
        DepthSnapshotTarget snapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice,
                                     commandBuffer,
                                     "CmdEndRenderPass2",
                                     &promotedDepthState,
                                     &snapshotTarget);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
            if (snapshotTarget.swapchain != VK_NULL_HANDLE && hasDepthState(promotedDepthState))
                recordDepthResolveSnapshotForCommandBuffer(pLogicalDevice, commandBuffer, promotedDepthState, &snapshotTarget);
            else if (hasDepthState(promotedDepthState) && sameDepthState(pLogicalDevice->activeDepthState, promotedDepthState))
                recordDepthResolveSnapshotForAllSwapchains(pLogicalDevice, commandBuffer, promotedDepthState);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        DepthState promotedDepthState = {};
        DepthSnapshotTarget snapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice,
                                     commandBuffer,
                                     "CmdEndRenderPass2KHR",
                                     &promotedDepthState,
                                     &snapshotTarget);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
            if (snapshotTarget.swapchain != VK_NULL_HANDLE && hasDepthState(promotedDepthState))
                recordDepthResolveSnapshotForCommandBuffer(pLogicalDevice, commandBuffer, promotedDepthState, &snapshotTarget);
            else if (hasDepthState(promotedDepthState) && sameDepthState(pLogicalDevice->activeDepthState, promotedDepthState))
                recordDepthResolveSnapshotForAllSwapchains(pLogicalDevice, commandBuffer, promotedDepthState);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRendering(VkCommandBuffer commandBuffer)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRendering(commandBuffer);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        DepthState promotedDepthState = {};
        DepthSnapshotTarget snapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice,
                                     commandBuffer,
                                     "CmdEndRendering",
                                     &promotedDepthState,
                                     &snapshotTarget);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRendering(commandBuffer);
            if (snapshotTarget.swapchain != VK_NULL_HANDLE && hasDepthState(promotedDepthState))
                recordDepthResolveSnapshotForCommandBuffer(pLogicalDevice, commandBuffer, promotedDepthState, &snapshotTarget);
            else if (hasDepthState(promotedDepthState) && sameDepthState(pLogicalDevice->activeDepthState, promotedDepthState))
                recordDepthResolveSnapshotForAllSwapchains(pLogicalDevice, commandBuffer, promotedDepthState);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdEndRenderingKHR(VkCommandBuffer commandBuffer)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderingKHR(commandBuffer);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        DepthState promotedDepthState = {};
        DepthSnapshotTarget snapshotTarget = {};
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                endTrackedDepthScope(pLogicalDevice,
                                     commandBuffer,
                                     "CmdEndRenderingKHR",
                                     &promotedDepthState,
                                     &snapshotTarget);
            }
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdEndRenderingKHR(commandBuffer);
            if (snapshotTarget.swapchain != VK_NULL_HANDLE && hasDepthState(promotedDepthState))
                recordDepthResolveSnapshotForCommandBuffer(pLogicalDevice, commandBuffer, promotedDepthState, &snapshotTarget);
            else if (hasDepthState(promotedDepthState) && sameDepthState(pLogicalDevice->activeDepthState, promotedDepthState))
                recordDepthResolveSnapshotForAllSwapchains(pLogicalDevice, commandBuffer, promotedDepthState);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdCopyImage(VkCommandBuffer commandBuffer,
                                                    VkImage srcImage,
                                                    VkImageLayout srcImageLayout,
                                                    VkImage dstImage,
                                                    VkImageLayout dstImageLayout,
                                                    uint32_t regionCount,
                                                    const VkImageCopy* pRegions)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                pLogicalDevice = devIt->second.get();
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
            scoped_lock l(globalLock);
            tryActivatePendingTransferLinkedDepthScope(pLogicalDevice, commandBuffer, dstImage, "CmdCopyImage");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                                                          uint32_t commandBufferCount,
                                                          const VkCommandBuffer* pCommandBuffers)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                accumulateExecutedCommandBufferDraws(pLogicalDevice, commandBuffer, pCommandBuffers, commandBufferCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdBlitImage(VkCommandBuffer commandBuffer,
                                                    VkImage srcImage,
                                                    VkImageLayout srcImageLayout,
                                                    VkImage dstImage,
                                                    VkImageLayout dstImageLayout,
                                                    uint32_t regionCount,
                                                    const VkImageBlit* pRegions,
                                                    VkFilter filter)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                pLogicalDevice = devIt->second.get();
        }

        if (pLogicalDevice)
        {
            pLogicalDevice->vkd.CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
            scoped_lock l(globalLock);
            tryActivatePendingTransferLinkedDepthScope(pLogicalDevice, commandBuffer, dstImage, "CmdBlitImage");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDraw(VkCommandBuffer commandBuffer,
                                               uint32_t vertexCount,
                                               uint32_t instanceCount,
                                               uint32_t firstVertex,
                                               uint32_t firstInstance)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                                                      uint32_t indexCount,
                                                      uint32_t instanceCount,
                                                      uint32_t firstIndex,
                                                      int32_t vertexOffset,
                                                      uint32_t firstInstance)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                                                       VkBuffer buffer,
                                                       VkDeviceSize offset,
                                                       uint32_t drawCount,
                                                       uint32_t stride)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, drawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                                                              VkBuffer buffer,
                                                              VkDeviceSize offset,
                                                              uint32_t drawCount,
                                                              uint32_t stride)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, drawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndirectCount(VkCommandBuffer commandBuffer,
                                                            VkBuffer buffer,
                                                            VkDeviceSize offset,
                                                            VkBuffer countBuffer,
                                                            VkDeviceSize countBufferOffset,
                                                            uint32_t maxDrawCount,
                                                            uint32_t stride)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndirectCountKHR(VkCommandBuffer commandBuffer,
                                                               VkBuffer buffer,
                                                               VkDeviceSize offset,
                                                               VkBuffer countBuffer,
                                                               VkDeviceSize countBufferOffset,
                                                               uint32_t maxDrawCount,
                                                               uint32_t stride)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer,
                                                                   VkBuffer buffer,
                                                                   VkDeviceSize offset,
                                                                   VkBuffer countBuffer,
                                                                   VkDeviceSize countBufferOffset,
                                                                   uint32_t maxDrawCount,
                                                                   uint32_t stride)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_CmdDrawIndexedIndirectCountKHR(VkCommandBuffer commandBuffer,
                                                                      VkBuffer buffer,
                                                                      VkDeviceSize offset,
                                                                      VkBuffer countBuffer,
                                                                      VkDeviceSize countBufferOffset,
                                                                      uint32_t maxDrawCount,
                                                                      uint32_t stride)
    {
        if (!vkShade::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* pLogicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                pLogicalDevice = devIt->second.get();
                countTrackedDepthDraw(pLogicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (pLogicalDevice)
            pLogicalDevice->vkd.CmdDrawIndexedIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                              const VkCommandBufferBeginInfo* pBeginInfo)
    {
        scoped_lock l(globalLock);

        auto devIt = deviceMap.find(GetKey(commandBuffer));
        if (devIt == deviceMap.end())
            return VK_ERROR_DEVICE_LOST;

        LogicalDevice* pLogicalDevice = devIt->second.get();
        pLogicalDevice->commandBufferRecordedDrawCounts[commandBuffer] = 0;
        pLogicalDevice->commandBufferDepthStates.erase(commandBuffer);
        pLogicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);

        return pLogicalDevice->vkd.BeginCommandBuffer(commandBuffer, pBeginInfo);
    }

    VKAPI_ATTR void VKAPI_CALL vkShade_FreeCommandBuffers(VkDevice device,
                                                          VkCommandPool commandPool,
                                                          uint32_t commandBufferCount,
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
                pLogicalDevice->commandBufferRecordedDrawCounts.erase(pCommandBuffers[i]);
                pLogicalDevice->commandBufferDepthStates.erase(pCommandBuffers[i]);
                pLogicalDevice->pendingTransferLinkedDepthScopes.erase(pCommandBuffers[i]);
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
            size_t i = std::distance(pLogicalDevice->depthImages.begin(), it);

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
                pLogicalDevice->activeDepthState = {};

            // Update all swapchains with new depth state
            DepthState depth = getDepthState(pLogicalDevice);
            updateDeviceDepthStateLocked(pLogicalDevice, depth, "DestroyImage");
        }

        pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, image, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Wayland surface interception — capture wl_display for input

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateWaylandSurfaceKHR(
        VkInstance                              instance,
        const VkWaylandSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*            pAllocator,
        VkSurfaceKHR*                           pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateWaylandSurfaceKHR");

        // Capture the wl_display and wl_surface for Wayland input
        if (pCreateInfo && pCreateInfo->display)
            setWaylandDisplay(pCreateInfo->display);
        if (pCreateInfo && pCreateInfo->surface)
            setWaylandSurface(pCreateInfo->surface);

        // Forward to the real implementation via the next layer
        auto nextFunc = (PFN_vkCreateWaylandSurfaceKHR)
            instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(
                instanceMap[GetKey(instance)], "vkCreateWaylandSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateXlibSurfaceKHR(
        VkInstance                           instance,
        const VkXlibSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*         pAllocator,
        VkSurfaceKHR*                        pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateXlibSurfaceKHR");
        markNonWaylandSurface("vkCreateXlibSurfaceKHR");

        auto nextFunc = (PFN_vkCreateXlibSurfaceKHR)
            instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(
                instanceMap[GetKey(instance)], "vkCreateXlibSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkShade_CreateXcbSurfaceKHR(
        VkInstance                          instance,
        const VkXcbSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*        pAllocator,
        VkSurfaceKHR*                       pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateXcbSurfaceKHR");
        markNonWaylandSurface("vkCreateXcbSurfaceKHR");

        auto nextFunc = (PFN_vkCreateXcbSurfaceKHR)
            instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(
                instanceMap[GetKey(instance)], "vkCreateXcbSurfaceKHR");
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
        return (PFN_vkVoidFunction) &vkShade::vkShade_##func;
    /*
    Return our funktions for the funktions we want to intercept
    the macro takes the name and returns our vkShade_##func, if the name is equal
    */

    // vkGetDeviceProcAddr needs to behave like vkGetInstanceProcAddr thanks to some games
#define INTERCEPT_CALLS \
    /* instance chain functions we intercept */ \
    if (!std::strcmp(pName, "vkGetInstanceProcAddr")) \
        return (PFN_vkVoidFunction) &vkShade_GetInstanceProcAddr; \
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
        return (PFN_vkVoidFunction) &vkShade_GetDeviceProcAddr; \
    GETPROCADDR(EnumerateDeviceLayerProperties); \
    GETPROCADDR(EnumerateDeviceExtensionProperties); \
    GETPROCADDR(CreateDevice); \
    GETPROCADDR(DestroyDevice); \
    GETPROCADDR(CreateSwapchainKHR); \
    GETPROCADDR(GetSwapchainImagesKHR); \
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
    GETPROCADDR(CreateFramebuffer); \
    GETPROCADDR(DestroyFramebuffer); \
    GETPROCADDR(BeginCommandBuffer); \
    GETPROCADDR(FreeCommandBuffers); \
    GETPROCADDR(CmdBeginRenderPass); \
    GETPROCADDR(CmdBeginRenderPass2); \
    GETPROCADDR(CmdBeginRenderPass2KHR); \
    GETPROCADDR(CmdBeginRendering); \
    GETPROCADDR(CmdBeginRenderingKHR); \
    GETPROCADDR(CmdEndRenderPass); \
    GETPROCADDR(CmdEndRenderPass2); \
    GETPROCADDR(CmdEndRenderPass2KHR); \
    GETPROCADDR(CmdEndRendering); \
    GETPROCADDR(CmdEndRenderingKHR); \
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
    GETPROCADDR(CmdDrawIndexedIndirectCountKHR); \

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL vkShade_GetDeviceProcAddr(VkDevice device, const char* pName)
    {
        vkShade::initConfigs();

        INTERCEPT_CALLS

        if (device == VK_NULL_HANDLE)
        {
            PFN_vkGetDeviceProcAddr next = getNextDeviceProcAddr();
            return next ? next(device, pName) : nullptr;
        }

        {
            vkShade::scoped_lock l(vkShade::globalLock);
            auto it = vkShade::deviceMap.find(vkShade::GetKey(device));
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
            auto it = vkShade::instanceDispatchMap.find(vkShade::GetKey(instance));
            if (it != vkShade::instanceDispatchMap.end() && it->second.GetInstanceProcAddr)
                return it->second.GetInstanceProcAddr(instance, pName);
        }

        PFN_vkGetInstanceProcAddr next = getNextInstanceProcAddr();
        return next ? next(instance, pName) : nullptr;
    }

} // extern "C"
