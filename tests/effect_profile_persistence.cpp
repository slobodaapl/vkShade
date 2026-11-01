#include "config.hpp"
#include "config_serializer.hpp"
#include "technique_id.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;

    const std::string                        prefix         = std::string(argv[1]) + "/effect-profile-" + std::to_string(getpid());
    const std::string                        basePath       = prefix + "-base.conf";
    const std::string                        profilePath    = prefix + "-profile.conf";
    const std::string                        unresolvedPath = prefix + "-unresolved.conf";
    const std::string                        copiedPath     = prefix + "-copy.conf";
    const std::string                        depthPath      = prefix + "-depth.conf";
    const std::string                        conflictPath   = prefix + "-depth-conflict.conf";
    const std::string                        inheritedPath  = prefix + "-depth-inherited.conf";
    const std::vector<std::string>           effects        = {"cas", "Curves", "cas.2"};
    const std::vector<std::string>           disabled       = {"Curves"};
    const std::string                        specialPath    = "/shader folder/a #1 \\\"curve\\\".fx";
    const std::map<std::string, std::string> paths          = {{"Curves", specialPath}, {"cas.2", "cas"}};
    bool   ok = check(ConfigSerializer::saveToPath(basePath, effects, disabled, {}, paths), "Could not save effect profile");
    Config base(basePath);
    ok &= check(base.getOption<std::vector<std::string>>("effects") == effects, "Effect order must survive a profile round trip");
    ok &= check(base.getOption<std::vector<std::string>>("disabledEffects") == disabled, "Disabled effects must survive a profile round trip");
    ok &= check(base.getOption<std::string>("Curves") == specialPath && base.getOption<std::string>("cas.2") == "cas",
                "Effect paths and distinct instance names must survive a profile round trip");

    {
        std::ofstream append(basePath, std::ios::app);
        append << "Unused = \"/shader folder/Other.fx\"\nUnused@FEATURE = 1\nUnused.radius = 0.75\n";
    }
    ok &= check(ConfigSerializer::saveToPath(basePath, effects, disabled, {}, paths), "Could not save profile with an available alias");
    Config preserved(basePath);
    ok &= check(preserved.getOption<std::string>("Unused") == "/shader folder/Other.fx" && preserved.getOption<std::string>("Unused@FEATURE") == "1"
                    && preserved.getOption<std::string>("Unused.radius") == "0.75",
                "Saving must preserve configured but unselected shader aliases and their settings");
    ok &= check(ConfigSerializer::saveToPath(copiedPath, effects, disabled, {}, paths, {}, {}, {}, basePath), "Could not copy profile");
    Config copied(copiedPath);
    ok &= check(copied.getOption<std::string>("Unused@FEATURE") == "1", "Saving to a new profile must retain available shader settings");

    {
        std::ofstream file(unresolvedPath);
        file << "effects = Missing\nMissing = \"/missing shader.fx\"\nMissing@FEATURE = 1\nMissing.radius = 0.25\n";
    }
    ok &= check(ConfigSerializer::saveToPath(unresolvedPath, {"Missing"}, {}, {}, {{"Missing", "/missing shader.fx"}}, {}, {}, {"Missing"}),
                "Could not save a profile with a missing shader");
    Config unresolved(unresolvedPath);
    ok &= check(unresolved.getOption<std::string>("Missing") == "/missing shader.fx" && unresolved.getOption<std::string>("Missing@FEATURE") == "1"
                    && unresolved.getOption<std::string>("Missing.radius") == "0.25",
                "Saving must preserve the settings of a selected missing shader");

    const std::vector<std::string> reordered = {"cas.2", "Curves", "cas"};
    ProfileSettings                settings;
    settings.hasTechniqueOrder    = true;
    settings.hasEnabledTechniques = true;
    settings.techniqueOrder       = {encodeTechniqueId({TechniqueOwner::Instance, "Curves", "First"}),
                                     encodeTechniqueId({TechniqueOwner::File, "/shaders/other:name.fx", "Second"}),
                                     encodeTechniqueId({TechniqueOwner::BuiltIn, "cas", ""})};
    settings.enabledTechniques    = {settings.techniqueOrder[0], settings.techniqueOrder[2]};
    ok &= check(ConfigSerializer::saveToPath(profilePath, reordered, {}, {}, paths, {}, settings), "Could not save reordered profile");
    Config profile(profilePath);
    profile.setFallback(&base);
    ok &= check(profile.getOption<std::vector<std::string>>("effects") == reordered, "A profile must preserve its own effect order");
    ok &= check(profile.getOption<std::vector<std::string>>("disabledEffects").empty(),
                "A profile with no disabled effects must override disabled effects in the base config");
    auto loaded = ConfigSerializer::loadProfileSettings(profilePath);
    ok &= check(loaded.hasTechniqueOrder && loaded.techniqueOrder == settings.techniqueOrder && loaded.hasEnabledTechniques
                    && loaded.enabledTechniques == settings.enabledTechniques,
                "Technique order and enabled state must survive a profile round trip");
    loaded = ConfigSerializer::loadProfileSettings(basePath);
    ok &= check(!loaded.hasTechniqueOrder && !loaded.hasEnabledTechniques, "Legacy profiles must retain their implicit technique state");

    settings.techniqueOrder.clear();
    settings.enabledTechniques.clear();
    ok &= check(ConfigSerializer::saveToPath(profilePath, {}, {}, {}, {}, {}, settings), "Could not save empty profile");
    Config emptyProfile(profilePath);
    ok &= check(emptyProfile.getOption<std::string>("Curves").empty(), "Removing a selected instance must remove its profile definition");
    emptyProfile.setFallback(&base);
    ok &= check(emptyProfile.getOption<std::vector<std::string>>("effects").empty(), "An empty profile must not inherit base effects");
    ok &=
        check(emptyProfile.getOption<std::vector<std::string>>("disabledEffects").empty(), "An empty profile must not inherit base disabled effects");
    loaded = ConfigSerializer::loadProfileSettings(profilePath);
    ok &= check(loaded.hasTechniqueOrder && loaded.techniqueOrder.empty() && loaded.hasEnabledTechniques && loaded.enabledTechniques.empty(),
                "Explicitly empty technique state must remain distinct from a legacy profile");

    {
        std::ofstream legacy(depthPath);
        legacy << "A@RESHADE_DEPTH_INPUT_IS_REVERSED = 1\nB@DEPTH_INPUT_IS_REVERSED = 1\n"
                  "A@RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN = 1\n";
    }
    auto depth = ConfigSerializer::loadProfileSettings(depthPath);
    ok &= check(depth.hasDepthDefinitions && depth.conflictingDepthDefinitions.empty() && depth.depthDefinitions["DEPTH_INPUT_IS_REVERSED"] == "1"
                    && depth.depthDefinitions["DEPTH_INPUT_IS_UPSIDE_DOWN"] == "1",
                "Matching per-effect depth definitions must migrate to shared values");
    ok &= check(ConfigSerializer::saveToPath(depthPath, {"A", "B"}, {}, {}, {}, {}, depth), "Could not save shared depth definitions");
    Config savedDepth(depthPath);
    ok &= check(savedDepth.getOption<std::string>("DEPTH_INPUT_IS_REVERSED") == "1"
                    && savedDepth.getOption<std::string>("DEPTH_INPUT_IS_UPSIDE_DOWN") == "1"
                    && savedDepth.getOption<std::string>("A@RESHADE_DEPTH_INPUT_IS_REVERSED").empty()
                    && savedDepth.getOption<std::string>("B@DEPTH_INPUT_IS_REVERSED").empty(),
                "A migrated profile must store depth definitions once for all effects");
    ok &= check(ConfigSerializer::saveToPath(depthPath, {"A", "B"}, {}, {}), "Saving unrelated settings must preserve shared depth definitions");
    Config retainedDepth(depthPath);
    ok &= check(retainedDepth.getOption<std::string>("DEPTH_INPUT_IS_REVERSED") == "1", "Shared values must survive unrelated saves");

    {
        std::ofstream conflict(conflictPath);
        conflict << "A@RESHADE_DEPTH_INPUT_IS_REVERSED = 0\nB@RESHADE_DEPTH_INPUT_IS_REVERSED = 1\n";
    }
    auto conflicting = ConfigSerializer::loadProfileSettings(conflictPath);
    ok &= check(conflicting.conflictingDepthDefinitions.count("DEPTH_INPUT_IS_REVERSED") == 1, "Conflicting legacy depth values must be reported");
    ok &= check(ConfigSerializer::saveToPath(conflictPath, {"A", "B"}, {}, {}, {}, {}, conflicting), "Could not preserve conflicting depth values");
    Config savedConflict(conflictPath);
    ok &= check(savedConflict.getOption<std::string>("A@RESHADE_DEPTH_INPUT_IS_REVERSED") == "0"
                    && savedConflict.getOption<std::string>("B@RESHADE_DEPTH_INPUT_IS_REVERSED") == "1",
                "Unresolved conflicting values must survive a profile save");
    std::ofstream(inheritedPath) << "DEPTH_INPUT_IS_UPSIDE_DOWN = 1\n";
    Config conflictFallback(conflictPath);
    Config inheritsConflict(inheritedPath);
    inheritsConflict.setFallback(&conflictFallback);
    auto inheritedConflict = ConfigSerializer::loadEffectiveDepthDefinitions(&inheritsConflict);
    ok &= check(inheritedConflict.conflicts.count("DEPTH_INPUT_IS_REVERSED") == 1, "Inherited conflicts must remain visible to the game profile");
    std::ofstream(inheritedPath, std::ios::app) << "DEPTH_INPUT_IS_REVERSED = 0\n";
    inheritedConflict = ConfigSerializer::loadEffectiveDepthDefinitions(&inheritsConflict);
    ok &= check(inheritedConflict.conflicts.empty() && inheritedConflict.values["DEPTH_INPUT_IS_REVERSED"] == "0",
                "A game-specific value must resolve an inherited conflict");
    conflicting.depthDefinitions["DEPTH_INPUT_IS_REVERSED"] = "1";
    conflicting.conflictingDepthDefinitions.clear();
    ok &= check(ConfigSerializer::saveToPath(conflictPath, {"A", "B"}, {}, {}, {}, {}, conflicting), "Could not resolve conflicting depth values");
    Config resolvedConflict(conflictPath);
    ok &= check(resolvedConflict.getOption<std::string>("DEPTH_INPUT_IS_REVERSED") == "1"
                    && resolvedConflict.getOption<std::string>("A@RESHADE_DEPTH_INPUT_IS_REVERSED").empty()
                    && resolvedConflict.getOption<std::string>("B@RESHADE_DEPTH_INPUT_IS_REVERSED").empty(),
                "Resolving a conflict must replace its legacy values with one shared value");

    std::ofstream(inheritedPath) << "DEPTH_INPUT_IS_UPSIDE_DOWN = 1\n";
    Config inherited(inheritedPath);
    inherited.setFallback(&savedDepth);
    auto effective = ConfigSerializer::loadDepthDefinitions(&inherited);
    ok &= check(effective["DEPTH_INPUT_IS_REVERSED"] == "1" && effective["DEPTH_INPUT_IS_UPSIDE_DOWN"] == "1",
                "Game depth definitions must inherit unspecified shared defaults");
    std::ofstream(inheritedPath, std::ios::app) << "DEPTH_INPUT_IS_REVERSED = 0\n";
    inherited.reload();
    effective = ConfigSerializer::loadDepthDefinitions(&inherited);
    ok &= check(effective["DEPTH_INPUT_IS_REVERSED"] == "0", "A game depth definition must override its inherited default");

    std::remove(basePath.c_str());
    std::remove(profilePath.c_str());
    std::remove(unresolvedPath.c_str());
    std::remove(copiedPath.c_str());
    std::remove(depthPath.c_str());
    std::remove(conflictPath.c_str());
    std::remove(inheritedPath.c_str());
    return ok ? 0 : 1;
}
