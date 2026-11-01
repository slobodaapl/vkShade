#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vkShade
{
    enum class TechniqueOwner
    {
        BuiltIn, // Retained for profiles saved before built-in effects were removed.
        File,
        Instance,
    };

    struct TechniqueId
    {
        TechniqueOwner owner = TechniqueOwner::Instance;
        std::string    effect;
        std::string    technique;

        bool operator==(const TechniqueId&) const = default;
    };

    std::string                encodeTechniqueId(const TechniqueId& id);
    std::optional<TechniqueId> decodeTechniqueId(std::string_view encoded);
} // namespace vkShade
