#include "settings_manager.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;

    std::string       tempTemplate = std::string(argv[1]) + "/depth-capture-settings-XXXXXX";
    std::vector<char> tempPath(tempTemplate.begin(), tempTemplate.end());
    tempPath.push_back('\0');
    char* directory = mkdtemp(tempPath.data());
    if (!directory)
        return 1;

    const std::filesystem::path root(directory);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};

    if (setenv("XDG_CONFIG_HOME", directory, 1) != 0)
        return 1;
    std::filesystem::create_directories(root / "vkShade");
    const auto config = root / "vkShade" / "vkShade.conf";
    std::ofstream(config).close();

    bool            ok = true;
    SettingsManager defaults;
    defaults.initialize();
    ok &= check(defaults.getDepthCapture() && defaults.isDepthCaptureActive(), "Depth capture must default on");
    defaults.setDepthCapture(false);
    ok &= check(!defaults.getDepthCapture() && !defaults.isDepthCaptureActive(), "Disabling capture must stop it immediately");
    defaults.setDepthCapture(true);
    ok &= check(defaults.getDepthCapture() && !defaults.isDepthCaptureActive(), "Re-enabling capture must wait for restart");
    ok &= check(defaults.save(), "The requested capture setting must be saved");

    SettingsManager restarted;
    restarted.initialize();
    ok &= check(restarted.getDepthCapture() && restarted.isDepthCaptureActive(), "A restart must activate the saved setting");
    restarted.setSafeAntiCheat(true);
    ok &= check(!restarted.isDepthCaptureActive(), "Safe Anti-Cheat must stop capture immediately");
    restarted.setSafeAntiCheat(false);
    ok &= check(!restarted.isDepthCaptureActive(), "Leaving Safe Anti-Cheat must not reactivate capture mid-session");

    std::ofstream(config) << "depthCapture = off\n";
    SettingsManager disabled;
    disabled.initialize();
    ok &= check(!disabled.getDepthCapture() && !disabled.isDepthCaptureActive(), "An explicit off setting must be respected");
    disabled.setDepthCapture(true);
    ok &= check(disabled.getDepthCapture() && !disabled.isDepthCaptureActive(), "Enabling capture from off must wait for restart");

    return ok ? 0 : 1;
}
