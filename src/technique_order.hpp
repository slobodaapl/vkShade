#pragma once

#include "technique_id.hpp"

#include <vector>

namespace vkShade
{
    std::vector<TechniqueId> mergeTechniqueOrder(const std::vector<TechniqueId>& available, const std::vector<TechniqueId>& saved);
    std::vector<TechniqueId> mergeProfileTechniqueOrder(const std::vector<TechniqueId>& available,
                                                        const std::vector<TechniqueId>& saved,
                                                        const std::vector<TechniqueId>& enabled);
    bool                     moveTechnique(std::vector<TechniqueId>& order, const TechniqueId& source, const TechniqueId& target);
} // namespace vkShade
