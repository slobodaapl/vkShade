#include "logical_device.hpp"

#include <algorithm>
#include <unordered_set>

namespace vkShade
{
    namespace
    {
        void add(DepthDrawStats& destination, const DepthDrawStats& source)
        {
            destination.vertices += source.vertices;
            destination.drawcalls += source.drawcalls;
            destination.drawcallsIndirect += source.drawcallsIndirect;
        }
    } // namespace

    void DepthFrameTracker::begin(VkCommandBuffer commandBuffer)
    {
        recorded[commandBuffer].clear();
        clearOrdinals[commandBuffer].clear();
    }

    void DepthFrameTracker::forget(VkCommandBuffer commandBuffer)
    {
        recorded.erase(commandBuffer);
        clearOrdinals.erase(commandBuffer);
    }

    void DepthFrameTracker::reset()
    {
        recorded.clear();
        clearOrdinals.clear();
        frame.clear();
        frameIndex.clear();
    }

    void DepthFrameTracker::forgetSource(uint64_t sourceId)
    {
        for (auto& [_, events] : recorded)
            events.erase(std::remove_if(events.begin(), events.end(), [sourceId](const Event& event) { return event.depth.sourceId == sourceId; }),
                         events.end());
        for (auto& [_, ordinals] : clearOrdinals)
            ordinals.erase(sourceId);
        for (auto& source : frame)
            if (source.candidate.depthState.sourceId == sourceId)
                source.candidate.depthState = {};
        frameIndex.erase(sourceId);
    }

    void DepthFrameTracker::forgetView(VkImageView imageView)
    {
        for (auto& [_, events] : recorded)
            events.erase(std::remove_if(events.begin(), events.end(), [imageView](const Event& event) { return event.depth.imageView == imageView; }),
                         events.end());
        for (auto& source : frame)
            if (source.candidate.depthState.imageView == imageView)
            {
                frameIndex.erase(source.candidate.depthState.sourceId);
                source.candidate.depthState = {};
            }
    }

    void DepthFrameTracker::draw(VkCommandBuffer commandBuffer, const DepthState& depth, const DepthDrawStats& stats)
    {
        auto& events = recorded[commandBuffer];
        if (!events.empty() && !events.back().isClear && !events.back().isLayout && events.back().depth.image == depth.image
            && events.back().depth.sourceId == depth.sourceId && events.back().depth.imageView == depth.imageView
            && events.back().depth.observedLayout == depth.observedLayout)
            add(events.back().stats, stats);
        else
            events.push_back({depth, stats, {}, false});
    }

    void DepthFrameTracker::layout(VkCommandBuffer commandBuffer, const DepthState& depth)
    {
        Event event;
        event.depth    = depth;
        event.isLayout = true;
        recorded[commandBuffer].push_back(std::move(event));
    }

    uint32_t DepthFrameTracker::nextClearOrdinal(VkCommandBuffer commandBuffer, uint64_t sourceId) const
    {
        const auto it = clearOrdinals.find(commandBuffer);
        if (it == clearOrdinals.end())
            return 0;
        const auto source = it->second.find(sourceId);
        return source == it->second.end() ? 0 : source->second;
    }

    void DepthFrameTracker::clear(VkCommandBuffer              commandBuffer,
                                  const DepthState&            depth,
                                  DepthClearKind               kind,
                                  bool                         copyPossible,
                                  std::shared_ptr<DepthBackup> backup,
                                  bool                         requiresPresentQueue)
    {
        Event event;
        event.depth                      = depth;
        event.isClear                    = true;
        event.clear.commandBuffer        = commandBuffer;
        event.clear.ordinal              = clearOrdinals[commandBuffer][depth.sourceId]++;
        event.clear.kind                 = kind;
        event.clear.copyPossible         = copyPossible;
        event.clear.requiresPresentQueue = requiresPresentQueue;
        event.clear.copied               = static_cast<bool>(backup);
        event.clear.backup               = std::move(backup);
        recorded[commandBuffer].push_back(std::move(event));
    }

    void DepthFrameTracker::execute(VkCommandBuffer primary, VkCommandBuffer secondary, const DepthState* inheritedDepth)
    {
        const auto found = recorded.find(secondary);
        if (found == recorded.end())
            return;
        const auto& source = found->second;
        auto&       target = recorded[primary];
        for (const auto& original : source)
        {
            auto event = original;
            if (event.depth.image == VK_NULL_HANDLE && inheritedDepth)
                event.depth = *inheritedDepth;
            target.push_back(std::move(event));
        }
    }

    void DepthFrameTracker::submit(VkCommandBuffer commandBuffer)
    {
        const auto found = recorded.find(commandBuffer);
        if (found == recorded.end())
            return;
        for (const auto& event : found->second)
        {
            if (event.depth.image == VK_NULL_HANDLE || event.depth.sourceId == 0)
                continue;
            if (event.isLayout)
            {
                const auto source = frameIndex.find(event.depth.sourceId);
                if (source != frameIndex.end())
                    frame[source->second].candidate.depthState.observedLayout = event.depth.observedLayout;
                continue;
            }
            auto [it, inserted] = frameIndex.emplace(event.depth.sourceId, frame.size());
            if (inserted)
                frame.push_back({});
            auto& source = frame[it->second];
            if (source.candidate.depthState.image == VK_NULL_HANDLE
                || (event.depth.imageView != VK_NULL_HANDLE && (event.depth.capturable || !source.candidate.depthState.capturable)))
                source.candidate.depthState = event.depth;
            if (event.isClear)
            {
                auto clear  = event.clear;
                clear.stats = source.sinceClear;
                source.candidate.clears.push_back(clear);
                source.sinceClear = {};
            }
            else
            {
                add(source.candidate.stats, event.stats);
                add(source.sinceClear, event.stats);
            }
        }
    }

    std::vector<DepthSourceCandidate> DepthFrameTracker::finishFrame()
    {
        std::vector<DepthSourceCandidate> result;
        result.reserve(frame.size());
        for (auto& source : frame)
            if (source.candidate.depthState.image != VK_NULL_HANDLE)
            {
                std::unordered_set<const DepthBackup*> seen;
                for (auto clear = source.candidate.clears.rbegin(); clear != source.candidate.clears.rend(); ++clear)
                    if (clear->backup && !seen.insert(clear->backup.get()).second)
                    {
                        clear->copyPossible = false;
                        clear->copied       = false;
                        clear->backup.reset();
                    }
                result.push_back(std::move(source.candidate));
            }
        frame.clear();
        frameIndex.clear();
        return result;
    }
} // namespace vkShade
