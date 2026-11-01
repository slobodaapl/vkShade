#include "config.hpp"
#include "effects/effect_registry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 1;

    const auto work        = std::filesystem::path(argv[1]) / ("effect-registry-" + std::to_string(getpid()));
    const auto previousDir = std::filesystem::current_path();
    std::filesystem::create_directories(work / "shaders");
    std::filesystem::create_directories(work / "config");
    std::filesystem::copy_file(argv[2], work / "shaders" / "CAS.fx");
    std::filesystem::copy_file(argv[2], work / "shaders" / "DLS.fx");
    setenv("XDG_CONFIG_HOME", (work / "config").c_str(), 1);
    std::filesystem::current_path(work);

    Config         emptyConfig((work / "empty.conf").string());
    EffectRegistry empty;
    empty.initialize(&emptyConfig);
    empty.ensureEffect("cas");
    bool ok = check(empty.getAllEffects().empty(), "An empty shader search path must not provide bundled effects");

    const auto profile = work / "profile.conf";
    {
        std::ofstream file(profile);
        file << "effects = cas:dls:missing\n"
             << "disabledEffects = dls\n"
             << "reshadeIncludePath = \"" << (work / "shaders").string() << "\"\n";
    }
    Config         config(profile.string());
    EffectRegistry registry;
    registry.initialize(&config);
    registry.initializeSelectedEffectsFromConfig();
    ok &= check(registry.hasEffect("cas") && registry.getEffectFilePath("cas") == (work / "shaders" / "CAS.fx").string(),
                "A user shader with a former built-in name must be found case-insensitively");
    ok &= check(registry.hasEffect("dls") && !registry.isEffectEnabled("dls"), "The profile must preserve disabled user shaders");
    ok &= check(!registry.hasEffect("missing"), "A missing shader must remain unresolved");
    registry.ensureEffect("cas.2", (work / "shaders" / "CAS.fx").string());
    ok &= check(registry.hasEffect("cas.2") && registry.getEffectFilePath("cas.2") == registry.getEffectFilePath("cas"),
                "A user shader must support multiple independently named instances");
    registry.setEffectEnabled("cas.2", false);
    ok &= check(registry.isEffectEnabled("cas") && !registry.isEffectEnabled("cas.2"), "Instance enable states must be independent");

    std::filesystem::current_path(previousDir);
    std::filesystem::remove_all(work);
    return ok ? 0 : 1;
}
