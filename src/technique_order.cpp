#include "technique_order.hpp"

#include <algorithm>
#include <unordered_set>

namespace vkShade
{
    std::vector<TechniqueId> mergeTechniqueOrder(const std::vector<TechniqueId>& available, const std::vector<TechniqueId>& saved)
    {
        std::vector<TechniqueId>        result;
        std::unordered_set<std::string> seen;
        result.reserve(saved.size() + available.size());
        auto append = [&](const std::vector<TechniqueId>& items) {
            for (const auto& id : items)
                if (seen.insert(encodeTechniqueId(id)).second)
                    result.push_back(id);
        };
        append(saved);
        append(available);
        return result;
    }

    std::vector<TechniqueId> mergeProfileTechniqueOrder(const std::vector<TechniqueId>& available,
                                                        const std::vector<TechniqueId>& saved,
                                                        const std::vector<TechniqueId>& enabled)
    {
        auto ordered = saved;
        ordered.insert(ordered.end(), enabled.begin(), enabled.end());
        return mergeTechniqueOrder(available, ordered);
    }

    bool moveTechnique(std::vector<TechniqueId>& order, const TechniqueId& source, const TechniqueId& target)
    {
        auto sourceIt = std::find(order.begin(), order.end(), source);
        auto targetIt = std::find(order.begin(), order.end(), target);
        if (sourceIt == order.end() || targetIt == order.end() || sourceIt == targetIt)
            return false;
        if (sourceIt < targetIt)
            std::rotate(sourceIt, sourceIt + 1, targetIt + 1);
        else
            std::rotate(targetIt, sourceIt, sourceIt + 1);
        return true;
    }
} // namespace vkShade
