#ifndef DEPTH_SELECTION_HPP_INCLUDED
#define DEPTH_SELECTION_HPP_INCLUDED

#include <algorithm>
#include <optional>
#include <unordered_map>
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
        return lhs.image != VK_NULL_HANDLE && lhs.image == rhs.image && lhs.sourceId == rhs.sourceId;
    }

    inline bool sameDepthSourceLayout(const DepthState& lhs, const DepthState& rhs)
    {
        return sameDepthSource(lhs, rhs) && lhs.imageView == rhs.imageView && lhs.observedLayout == rhs.observedLayout;
    }

    inline bool depthSourceRanksBefore(const std::vector<LogicalDevice::DepthSourceCandidate>& candidates, size_t lhs, size_t rhs)
    {
        if (candidates[lhs].stale != candidates[rhs].stale)
            return !candidates[lhs].stale;
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

    inline bool depthWorkloadBeats(const LogicalDevice::DrawStats& lhs, const LogicalDevice::DrawStats& rhs)
    {
        const uint64_t leftDraws  = static_cast<uint64_t>(lhs.drawcalls) + lhs.drawcallsIndirect;
        const uint64_t rightDraws = static_cast<uint64_t>(rhs.drawcalls) + rhs.drawcallsIndirect;
        if (lhs.drawcallsIndirect < leftDraws / 3 && lhs.vertices != rhs.vertices)
            return lhs.vertices > rhs.vertices;
        if (leftDraws != rightDraws)
            return leftDraws > rightDraws;
        return lhs.vertices > rhs.vertices;
    }

    inline bool acceptDepthFrame(const std::vector<LogicalDevice::DepthSourceCandidate>& submitted,
                                 const std::vector<LogicalDevice::DepthSourceCandidate>& previous,
                                 DepthFrameCadence&                                      cadence,
                                 std::chrono::steady_clock::time_point                   now)
    {
        const auto drawCount = [](const auto& source) { return static_cast<uint64_t>(source.stats.drawcalls) + source.stats.drawcallsIndirect; };
        const bool lowWork   = submitted.empty() || (submitted.size() == 1 && drawCount(submitted.front()) <= 8);
        const bool hadScene  = std::any_of(previous.begin(), previous.end(), [&](const auto& source) {
            return source.depthState.image != VK_NULL_HANDLE && !source.stale && drawCount(source) > 8;
        });
        if (!lowWork || !hadScene)
        {
            cadence.lowWorkPending = false;
            return true;
        }
        if (!cadence.lowWorkPending)
        {
            cadence.lowWorkPending = true;
            cadence.lowWorkSince   = now;
        }
        if (now - cadence.lowWorkSince < std::chrono::milliseconds(250))
            return false;
        cadence.lowWorkPending = false;
        return true;
    }

    inline bool automaticDepthWorkloadBeats(const DepthDrawStats& lhs, const DepthDrawStats& rhs, bool preferDraws)
    {
        const uint64_t leftDraws  = static_cast<uint64_t>(lhs.drawcalls) + lhs.drawcallsIndirect;
        const uint64_t rightDraws = static_cast<uint64_t>(rhs.drawcalls) + rhs.drawcallsIndirect;
        if (preferDraws && leftDraws != rightDraws)
            return leftDraws > rightDraws;
        if (lhs.vertices != rhs.vertices)
            return lhs.vertices > rhs.vertices;
        return leftDraws > rightDraws;
    }

    template<typename Candidate, typename Eligible, typename Identity>
    std::optional<size_t> selectStableDepthWorkload(const std::vector<Candidate>&         candidates,
                                                    AutomaticDepthSelection&              selection,
                                                    std::chrono::steady_clock::time_point now,
                                                    Eligible                              eligible,
                                                    Identity                              identity,
                                                    bool                                  preferLater)
    {
        uint64_t totalDraws    = 0;
        uint64_t indirectDraws = 0;
        for (const auto& candidate : candidates)
            if (eligible(candidate))
            {
                totalDraws += static_cast<uint64_t>(candidate.stats.drawcalls) + candidate.stats.drawcallsIndirect;
                indirectDraws += candidate.stats.drawcallsIndirect;
            }
        const bool            preferDraws = indirectDraws != 0 && indirectDraws >= (totalDraws + 2) / 3;
        std::optional<size_t> best;
        std::optional<size_t> current;
        std::optional<size_t> challenger;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (!eligible(candidates[i]))
                continue;
            const auto id = identity(candidates[i], i);
            if (id == selection.selectedId)
                current = i;
            if (id == selection.challengerId)
                challenger = i;
            if (!best || automaticDepthWorkloadBeats(candidates[i].stats, candidates[*best].stats, preferDraws)
                || (preferLater && !automaticDepthWorkloadBeats(candidates[*best].stats, candidates[i].stats, preferDraws)))
                best = i;
        }
        if (!current)
        {
            selection.selectedId   = best ? identity(candidates[*best], *best) : 0;
            selection.challengerId = 0;
            return best;
        }
        const auto betterThanCurrent = [&](size_t index) {
            return index != *current && automaticDepthWorkloadBeats(candidates[index].stats, candidates[*current].stats, preferDraws);
        };
        if (!challenger || !betterThanCurrent(*challenger))
        {
            challenger                = best && betterThanCurrent(*best) ? best : std::nullopt;
            selection.challengerId    = challenger ? identity(candidates[*challenger], *challenger) : 0;
            selection.challengerSince = now;
        }
        if (challenger && now - selection.challengerSince >= std::chrono::milliseconds(250))
        {
            selection.selectedId   = identity(candidates[*challenger], *challenger);
            selection.challengerId = 0;
            return challenger;
        }
        return current;
    }

    template<typename Eligible>
    std::optional<size_t> selectStableAutomaticDepthSource(const std::vector<LogicalDevice::DepthSourceCandidate>& candidates,
                                                           AutomaticDepthSelection&                                selection,
                                                           std::chrono::steady_clock::time_point                   now,
                                                           Eligible                                                eligible)
    {
        return selectStableDepthWorkload(
            candidates, selection, now, eligible, [](const auto& candidate, size_t) { return candidate.depthState.sourceId; }, false);
    }

    inline std::optional<size_t> selectStableAutomaticDepthClear(const std::vector<DepthClearCandidate>& clears,
                                                                 AutomaticDepthSelection&                selection,
                                                                 std::chrono::steady_clock::time_point   now)
    {
        return selectStableDepthWorkload(
            clears,
            selection,
            now,
            [](const auto& clear) { return clear.copyPossible; },
            [](const auto&, size_t index) { return static_cast<uint64_t>(index) + 1; },
            true);
    }

    inline std::optional<size_t> selectDepthClear(const std::vector<DepthClearCandidate>& clears, int selection)
    {
        if (selection != 0)
        {
            const int64_t index = selection > 0 ? static_cast<int64_t>(selection) - 1 : static_cast<int64_t>(clears.size()) + selection;
            if (index < 0 || static_cast<uint64_t>(index) >= clears.size() || !clears[index].copyPossible)
                return std::nullopt;
            return static_cast<size_t>(index);
        }
        std::optional<size_t> best;
        for (size_t i = 0; i < clears.size(); ++i)
            if (clears[i].copyPossible && (!best || !depthWorkloadBeats(clears[*best].stats, clears[i].stats)))
                best = i;
        return best;
    }

    inline size_t depthSourceDrawRank(const std::vector<LogicalDevice::DepthSourceCandidate>& candidates, size_t index)
    {
        size_t rank = 0;
        for (size_t i = 0; i < candidates.size(); ++i)
            if (i != index && candidates[i].depthState.image != VK_NULL_HANDLE && depthSourceRanksBefore(candidates, i, index))
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
            return index < candidates.size() && candidates[index].depthState.image != VK_NULL_HANDLE ? std::optional<size_t>(index) : std::nullopt;
        }

        const size_t        rank = static_cast<size_t>(DEPTH_SOURCE_DRAW_RANK_BASE - selection);
        std::vector<size_t> order;
        order.reserve(candidates.size());
        for (size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i].depthState.image != VK_NULL_HANDLE)
                order.push_back(i);
        if (rank >= order.size())
            return std::nullopt;
        std::sort(order.begin(), order.end(), [&candidates](size_t lhs, size_t rhs) { return depthSourceRanksBefore(candidates, lhs, rhs); });
        return order[rank];
    }

    inline std::optional<size_t> resolveActiveDepthSource(const std::vector<LogicalDevice::DepthSourceCandidate>& candidates,
                                                          int                                                     selection,
                                                          const DepthState&                                       pinnedSource          = {},
                                                          bool                                                    pinnedSourceDestroyed = false)
    {
        auto index = resolveDepthSource(candidates, selection, pinnedSource, pinnedSourceDestroyed);
        return index && !candidates[*index].stale ? index : std::nullopt;
    }

    inline void markDepthSourceStale(LogicalDevice::DepthSourceCandidate& candidate)
    {
        candidate.stale = true;
        for (auto& clear : candidate.clears)
        {
            clear.commandBuffer = VK_NULL_HANDLE;
            clear.copied        = false;
            clear.backup.reset();
        }
    }

    inline std::vector<LogicalDevice::DepthSourceCandidate> retainDepthSourceHistory(std::vector<LogicalDevice::DepthSourceCandidate> submitted,
                                                                                     std::vector<LogicalDevice::DepthSourceCandidate> previous)
    {
        std::unordered_map<uint64_t, size_t> submittedBySource;
        submittedBySource.reserve(submitted.size());
        for (size_t i = 0; i < submitted.size(); ++i)
            submittedBySource.emplace(submitted[i].depthState.sourceId, i);
        std::vector<bool> consumed(submitted.size(), false);
        previous.reserve(previous.size() + submitted.size());
        for (auto& candidate : previous)
        {
            if (candidate.depthState.image == VK_NULL_HANDLE)
                continue;
            const auto current = submittedBySource.find(candidate.depthState.sourceId);
            if (current != submittedBySource.end())
            {
                candidate                 = std::move(submitted[current->second]);
                consumed[current->second] = true;
                continue;
            }
            markDepthSourceStale(candidate);
        }
        size_t freeSlot = 0;
        for (size_t i = 0; i < submitted.size(); ++i)
        {
            if (consumed[i])
                continue;
            while (freeSlot < previous.size() && previous[freeSlot].depthState.image != VK_NULL_HANDLE)
                ++freeSlot;
            if (freeSlot == previous.size())
                previous.push_back(std::move(submitted[i]));
            else
                previous[freeSlot] = std::move(submitted[i]);
        }
        return previous;
    }
} // namespace vkShade

#endif // DEPTH_SELECTION_HPP_INCLUDED
