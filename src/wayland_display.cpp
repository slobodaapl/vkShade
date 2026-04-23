#include "wayland_display.hpp"

#include "logger.hpp"

#include <cstdlib>
#include <string>

namespace vkShade
{
    static wl_display* waylandDisplay = nullptr;
    static wl_surface* waylandSurface = nullptr;
    static int waylandChecked = -1; // -1 = unchecked, 0 = no, 1 = yes
    static bool nonWaylandSurface = false;

    void setWaylandDisplay(wl_display* display)
    {
        if (!display)
            return;

        if (nonWaylandSurface)
            return;

        waylandDisplay = display;
        waylandChecked = 1;
        Logger::info("captured Wayland display from vkCreateWaylandSurfaceKHR");
    }

    wl_display* getWaylandDisplay()
    {
        return waylandDisplay;
    }

    void setWaylandSurface(wl_surface* surface)
    {
        if (!surface)
            return;

        if (nonWaylandSurface)
            return;

        waylandSurface = surface;
        Logger::info("captured Wayland surface from vkCreateWaylandSurfaceKHR");
    }

    wl_surface* getWaylandSurface()
    {
        return waylandSurface;
    }

    bool isWayland()
    {
        if (nonWaylandSurface)
            return false;

        if (waylandChecked >= 0)
            return waylandChecked == 1;

        // Only enable the Wayland input backend after we actually intercepted
        // vkCreateWaylandSurfaceKHR and captured the game's wl_display/surface.
        // This avoids false positives on Xwayland apps where WAYLAND_DISPLAY
        // exists in the session but Vulkan uses Xlib/Xcb surfaces.
        if (waylandDisplay != nullptr || waylandSurface != nullptr)
        {
            waylandChecked = 1;
            return true;
        }

        waylandChecked = 0;
        return false;
    }

    bool isNonWaylandSurface()
    {
        return nonWaylandSurface;
    }

    void markNonWaylandSurface(const char* source)
    {
        if (nonWaylandSurface)
            return;

        nonWaylandSurface = true;
        waylandDisplay = nullptr;
        waylandSurface = nullptr;
        waylandChecked = 0;
        if (source && *source)
            Logger::warn(std::string("unsupported non-Wayland Vulkan surface via ") + source + "; vkShade will pass through only");
        else
            Logger::warn("unsupported non-Wayland Vulkan surface detected; vkShade will pass through only");
    }
} // namespace vkShade
