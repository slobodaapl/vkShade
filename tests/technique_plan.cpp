#include "technique_id.hpp"
#include "technique_plan.hpp"

#include <iostream>
#include <string>
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
    const std::vector<std::string> effects = {"A", "B", "cas"};
    ProfileSettings                settings;
    bool                           ok =
        check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"A", std::nullopt}, {"B", std::nullopt}, {"cas", std::nullopt}}),
              "Legacy profiles must execute complete effects in their saved order");

    const auto a1                 = encodeTechniqueId({TechniqueOwner::Instance, "A", "First"});
    const auto a2                 = encodeTechniqueId({TechniqueOwner::Instance, "A", "Second"});
    const auto b1                 = encodeTechniqueId({TechniqueOwner::Instance, "B", "Only"});
    const auto builtin            = encodeTechniqueId({TechniqueOwner::BuiltIn, "cas", ""});
    const auto missing            = encodeTechniqueId({TechniqueOwner::Instance, "Missing", "Only"});
    const auto unselected         = encodeTechniqueId({TechniqueOwner::File, "/available.fx", "Only"});
    settings.hasTechniqueOrder    = true;
    settings.hasEnabledTechniques = true;
    settings.techniqueOrder       = {a1, b1, a2, builtin, missing, unselected, a1};
    settings.enabledTechniques    = {a1, b1, a2, builtin, missing, unselected};
    ok &= check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"A", "First"}, {"B", "Only"}, {"A", "Second"}}),
                "Enabled FX techniques must execute in order without running removed built-in entries");
    const auto casFx           = encodeTechniqueId({TechniqueOwner::Instance, "cas", "CAS"});
    settings.techniqueOrder    = {builtin, casFx};
    settings.enabledTechniques = {builtin, casFx};
    ok &= check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"cas", "CAS"}}),
                "A user-provided shader with the same name must run only when its own technique is enabled");

    settings.techniqueOrder    = {a1, b1, a2, builtin, missing, unselected, a1};
    settings.enabledTechniques = {b1, a2};
    ok &= check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"B", "Only"}, {"A", "Second"}}),
                "Disabled techniques must not execute or disturb the remaining order");
    settings.hasTechniqueOrder = false;
    settings.enabledTechniques = {a2, b1};
    ok &= check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"A", "Second"}, {"B", "Only"}}),
                "Enabled technique order must be used when no separate order is saved");
    settings.enabledTechniques.clear();
    ok &= check(buildTechniquePlan(effects, settings).empty(), "An explicitly empty technique selection must execute nothing");
    const auto wholeA          = encodeTechniqueId({TechniqueOwner::Instance, "A", ""});
    settings.hasTechniqueOrder = true;
    settings.techniqueOrder    = {a1, b1, wholeA};
    settings.enabledTechniques = {a1, b1, wholeA};
    ok &= check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"A", std::nullopt}, {"B", "Only"}}),
                "A whole-effect entry must run all techniques once, at the first enabled position");
    settings.techniqueOrder    = {a1, b1, wholeA};
    settings.enabledTechniques = {b1, wholeA};
    ok &= check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"B", "Only"}, {"A", std::nullopt}}),
                "A disabled named technique must not change the whole-effect stage position");
    settings.enabledTechniques = {wholeA};
    ok &= check(buildTechniquePlan(effects, settings) == std::vector<TechniqueStage>({{"A", std::nullopt}}),
                "A legacy whole-effect entry must still run when no named technique is enabled");
    return ok ? 0 : 1;
}
