#include "technique_plan.hpp"

#include "technique_id.hpp"

#include <unordered_set>

namespace vkShade
{
    std::vector<TechniqueStage> buildTechniquePlan(const std::vector<std::string>& effects, const ProfileSettings& settings)
    {
        std::vector<TechniqueStage> result;
        if (!settings.hasEnabledTechniques)
        {
            for (const auto& effect : effects)
                result.push_back({effect, std::nullopt});
            return result;
        }

        const std::unordered_set<std::string> availableEffects(effects.begin(), effects.end());
        std::unordered_set<std::string>       enabled;
        std::unordered_set<std::string>       instancesWithWholeEffect;
        for (const auto& encoded : settings.enabledTechniques)
            if (auto id = decodeTechniqueId(encoded))
            {
                enabled.insert(encodeTechniqueId(*id));
                if (id->owner == TechniqueOwner::Instance && id->technique.empty())
                    instancesWithWholeEffect.insert(id->effect);
            }

        std::unordered_set<std::string> seen;
        auto                            append = [&](const std::vector<std::string>& entries) {
            for (const auto& encoded : entries)
            {
                auto id = decodeTechniqueId(encoded);
                if (!id || id->owner != TechniqueOwner::Instance || !availableEffects.count(id->effect))
                    continue;
                const auto canonical = encodeTechniqueId(*id);
                if (!enabled.count(canonical))
                    continue;
                if (id->owner == TechniqueOwner::Instance && instancesWithWholeEffect.count(id->effect))
                    id->technique.clear();
                const auto stageKey = encodeTechniqueId({TechniqueOwner::Instance, id->effect, id->technique});
                if (!seen.insert(stageKey).second)
                    continue;
                result.push_back({id->effect, id->technique.empty() ? std::nullopt : std::optional<std::string>(id->technique)});
            }
        };
        if (settings.hasTechniqueOrder)
            append(settings.techniqueOrder);
        append(settings.enabledTechniques);
        return result;
    }
} // namespace vkShade
