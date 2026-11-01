#pragma once

struct wl_proxy;

namespace vkShade
{
    // Register overlay-owned proxies so the interposition layer skips them.
    // Call BEFORE wl_pointer_add_listener / wl_keyboard_add_listener.
    void registerOverlayProxy(wl_proxy* proxy);
    void unregisterOverlayProxy(wl_proxy* proxy);

    void notifyGameInputBlocked(bool blocked);

    bool isWaylandGameCursorHidden();

    bool isWaylandGamePointerLocked();
} // namespace vkShade
