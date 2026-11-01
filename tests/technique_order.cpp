#include "technique_order.hpp"

#include <iostream>
#include <vector>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    const TechniqueId first{TechniqueOwner::Instance, "A", "First"};
    const TechniqueId second{TechniqueOwner::Instance, "A", "Second"};
    const TechniqueId other{TechniqueOwner::Instance, "B", "Only"};
    const TechniqueId missing{TechniqueOwner::File, "/missing.fx", "Gone"};
    const TechniqueId added{TechniqueOwner::File, "/new.fx", "New"};

    bool ok    = true;
    auto order = mergeTechniqueOrder({first, second, other, added}, {other, missing, first, first});
    ok &= check(order == std::vector<TechniqueId>({other, missing, first, second, added}),
                "Saved order and missing techniques must be retained; new techniques must be appended once");
    ok &= check(mergeProfileTechniqueOrder({first, second, other, added}, {first}, {other, second})
                    == std::vector<TechniqueId>({first, other, second, added}),
                "Enabled entries absent from a saved order must retain their execution order");
    ok &= check(mergeProfileTechniqueOrder({first, second, other, added}, {}, {other, second})
                    == std::vector<TechniqueId>({other, second, first, added}),
                "Enabled order must remain intact when a profile has no separate ordering");

    ok &= check(moveTechnique(order, added, other), "A technique must be movable before another effect");
    ok &= check(order == std::vector<TechniqueId>({added, other, missing, first, second}),
                "Moving upward must preserve the order of the remaining techniques");
    ok &= check(moveTechnique(order, added, second), "A technique must be movable after another effect");
    ok &= check(order == std::vector<TechniqueId>({other, missing, first, second, added}),
                "Moving downward must preserve the order of the remaining techniques");
    ok &= check(!moveTechnique(order, first, first) && !moveTechnique(order, first, {TechniqueOwner::BuiltIn, "unknown", ""}),
                "No-op and unknown moves must leave the order unchanged");
    ok &= check(order == std::vector<TechniqueId>({other, missing, first, second, added}), "Rejected moves must preserve the order");
    return ok ? 0 : 1;
}
