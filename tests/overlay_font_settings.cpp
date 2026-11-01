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
    std::string       name = std::string(argv[1]) + "/overlay-font-settings-XXXXXX";
    std::vector<char> path(name.begin(), name.end());
    path.push_back('\0');
    const char* directory = mkdtemp(path.data());
    if (!directory || setenv("XDG_CONFIG_HOME", directory, 1) != 0)
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
    std::filesystem::create_directories(root / "vkShade");
    const auto config = root / "vkShade" / "vkShade.conf";
    std::ofstream(config) << "overlayBlockInput = false\n";

    SettingsManager defaults;
    defaults.initialize();
    bool ok = check(defaults.getOverlayFontSize() == AUTO_OVERLAY_FONT_SIZE, "Font size must default to automatic");
    defaults.setOverlayFontSize(22);
    ok &= check(defaults.save(), "Manual font size could not be saved");
    SettingsManager restored;
    restored.initialize();
    ok &=
        check(restored.getOverlayFontSize() == 22 && !restored.getOverlayBlockInput(), "Manual font size and other settings must survive a restart");
    restored.setOverlayFontSize(AUTO_OVERLAY_FONT_SIZE);
    ok &= check(restored.save(), "Automatic font size could not be saved");
    SettingsManager automatic;
    automatic.initialize();
    ok &= check(automatic.getOverlayFontSize() == AUTO_OVERLAY_FONT_SIZE, "Automatic mode must survive a restart");

    std::ofstream(config) << "overlayFontSize = 1\n";
    SettingsManager tooSmall;
    tooSmall.initialize();
    ok &= check(tooSmall.getOverlayFontSize() == MIN_OVERLAY_FONT_SIZE, "Small font sizes must clamp to the minimum");
    std::ofstream(config) << "overlayFontSize = 900\n";
    SettingsManager tooLarge;
    tooLarge.initialize();
    ok &= check(tooLarge.getOverlayFontSize() == MAX_OVERLAY_FONT_SIZE, "Large font sizes must clamp to the maximum");
    std::ofstream(config) << "overlayFontSize = -1\n";
    SettingsManager invalid;
    invalid.initialize();
    ok &= check(invalid.getOverlayFontSize() == AUTO_OVERLAY_FONT_SIZE, "Invalid font sizes must retain the automatic default");
    return ok ? 0 : 1;
}
