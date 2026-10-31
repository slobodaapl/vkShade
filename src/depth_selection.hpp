#ifndef DEPTH_SELECTION_HPP_INCLUDED
#define DEPTH_SELECTION_HPP_INCLUDED

#include <algorithm>
#include <numeric>
#include <optional>
#include <vector>

#include "logical_device.hpp"

namespace vkShade
{
    constexpr int DEPTH_SOURCE_AUTO           = -1;
    constexpr int DEPTH_SOURCE_DRAW_RANK_BASE = -2;

    inline int resolveDepthSourcePreference(std::optional<int> selection, int configSelection)
    {
        return selection.value_or(configSelection);
    }

    inline bool sameDepthSource(const DepthState& lhs, const DepthState& rhs)
    {
        return lhs.image != VK_NULL_HANDLE && lhs.image == rhs.image && lhs.imageView == rhs.imageView && lhs.sourceId == rhs.sourceId;
    }

    inline bool sameDepthSourceLayout(const DepthState& lhs, const DepthState& rhs)
    {
        return sameDepthSource(lhs, rhs) && lhs.observedLayout == rhs.observedLayout;
    }

    inline bool depthSourceRanksBefore(const std::vector<LogicalDevice::DepthSourceCandidate>& candidates, size_t lhs, size_t rhs)
    {
        const auto&    left       = candidates[lhs].stats;
        const auto&    right      = candidates[rhs].stats;
        const uint64_t leftDraws  = static_cast<uint64_t>(left.drawcalls) + left.drawcallsIndirect;
        const uint64_t rightDraws = static_cast<uint64_t>(right.drawcalls) + right.drawcallsIndirect;
        if (leftDraws != rightDraws)
            return leftDraws > rightDraws;
        if (left.vertices != right.vertices)
            return left.vertices > right.vertices;
        return lhs < rhs;
    }

    inline size_t depthSourceDrawRank(const std::vector<LogicalDevice::DepthSourceCandidate>& candidates, size_t index)
    {
        size_t rank = 0;
        for (size_t i = 0; i < candidates.size(); ++i)
            if (i != index && depthSourceRanksBefore(candidates, i, index))
                ++rank;
        return rank;
    }

    inline std::optional<size_t> resolveDepthSource(const std::vector<LogicalDevice::DepthSourceCandidate>& candidates,
                                                    int                                                     selection,
                                                    const DepthState&                                       pinnedSource          = {},
                                                    bool                                                    pinnedSourceDestroyed = false)
    {
        if (selection == DEPTH_SOURCE_AUTO)
            return std::nullopt;

        if (selection >= 0)
        {
            if (pinnedSourceDestroyed)
                return std::nullopt;
            if (pinnedSource.image != VK_NULL_HANDLE)
            {
                std::optional<size_t> match;
                bool                  ambiguous = false;
                for (size_t i = 0; i < candidates.size(); ++i)
                {
                    if (sameDepthSource(candidates[i].depthState, pinnedSource))
                    {
                        if (candidates[i].depthState.observedLayout == pinnedSource.observedLayout)
                            return i;
                        ambiguous = match.has_value();
                        match     = i;
                    }
                }
                return ambiguous ? std::nullopt : match;
            }
            const size_t index = static_cast<size_t>(selection);
            return index < candidates.size() ? std::optional<size_t>(index) : std::nullopt;
        }

        const size_t rank = static_cast<size_t>(DEPTH_SOURCE_DRAW_RANK_BASE - selection);
        if (rank >= candidates.size())
            return std::nullopt;
        std::vector<size_t> order(candidates.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::sort(order.begin(), order.end(), [&candidates](size_t lhs, size_t rhs) { return depthSourceRanksBefore(candidates, lhs, rhs); });
        return order[rank];
    }
} // namespace vkShade

#endif // DEPTH_SELECTION_HPP_INCLUDED
