#pragma once

#include "mouse_input.hpp"

namespace vkShade
{
    MouseState getMouseStateWayland(uint32_t framebufferWidth, uint32_t framebufferHeight);

    // Initialize Wayland mouse from a captured wl_display
    bool initWaylandMouse();

    void setWaylandOverlayMouseActive(bool active);

    void resetWaylandMouseButtons();

    // Cleanup
    void cleanupWaylandMouse();

    // Mirror a button event from the game's pointer into overlay state.
    // The game's wl_pointer receives releases via implicit grab that our
    // overlay's private pointer never sees. Called from wayland_interpose.cpp.
    void mirrorButtonState(uint32_t button, bool pressed);
} // namespace vkShade
