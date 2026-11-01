#pragma once

#include "config_serializer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace vkShade
{
    struct TechniqueStage
    {
        std::string                effectName;
        std::optional<std::string> technique;

        bool operator==(const TechniqueStage&) const = default;
    };

    std::vector<TechniqueStage> buildTechniquePlan(const std::vector<std::string>& effects, const ProfileSettings& settings);
} // namespace vkShade
