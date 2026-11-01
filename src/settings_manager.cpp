#include "settings_manager.hpp"
#include "logger.hpp"

namespace vkShade
{
    // Global instance
    SettingsManager settingsManager;

    void SettingsManager::initialize()
    {
        if (initialized)
            return;

        settings = ConfigSerializer::loadSettings();
        activeDepthCapture.store(settings.depthCapture && !safeAntiCheat, std::memory_order_release);
        initialized = true;
        Logger::info("SettingsManager initialized");
    }

    bool SettingsManager::save()
    {
        bool success = ConfigSerializer::saveSettings(settings);
        if (success)
            Logger::debug("Settings saved to config");
        else
            Logger::err("Failed to save settings");
        return success;
    }

} // namespace vkShade
