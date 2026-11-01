// Wayland event interposition for input blocking.
//
// On Wayland, there is no XGrabPointer equivalent. The game and our overlay
// both receive pointer/keyboard events from the compositor for the same
// surface. This module interposes on wl_proxy_add_listener and
// wl_proxy_add_dispatcher (the underlying C functions that the generated
// protocol helpers use) and wraps the game's pointer/keyboard callbacks with
// callbacks that suppress events when the overlay has input blocked.
//
// Overlay-owned proxies are registered via registerOverlayProxy() and are
// always passed through unwrapped.

#include "wayland_interpose.hpp"
#include "input_blocker.hpp"
#include "wayland_display.hpp"
#include "wayland_pointer_geometry.hpp"
#include "logger.hpp"

// Forward-declare to avoid pulling in mouse_input_wayland.hpp (which
// transitively includes mouse_input.hpp → X11 headers in some targets).
namespace vkShade
{
    void mirrorButtonState(uint32_t button, bool pressed);
}

#include <wayland-client.h>
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-client-protocol.h"
#include "viewporter-client-protocol.h"
#include <dlfcn.h>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <vector>

namespace vkShade
{
    namespace
    {
        // Vulkan layers are typically loaded RTLD_LOCAL, which prevents global
        // symbol interposition in normal implicit-layer mode. Promote this
        // already-loaded object to RTLD_GLOBAL so wl_proxy/dlsym wrappers are
        // visible process-wide without requiring LD_PRELOAD.
        __attribute__((constructor)) void promoteSelfToGlobalScope()
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&promoteSelfToGlobalScope), &info) == 0 || !info.dli_fname)
                return;

            void* handle = dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_NOW | RTLD_GLOBAL);
            if (!handle)
                handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL);

            if (handle)
                Logger::debug("Wayland interpose: promoted vkShade library to RTLD_GLOBAL");
        }
    } // namespace

    static std::mutex                    proxyMutex;
    static std::unordered_set<wl_proxy*> overlayProxies;

    void registerOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.insert(proxy);
    }

    void unregisterOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.erase(proxy);
    }

    static bool isOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        return overlayProxies.count(proxy) > 0;
    }
} // namespace vkShade

// ── Game listener storage ────────────────────────────────────────────────────

// Per-proxy data for wrapped game listeners. We store the game's original
// listener and user_data so we can forward events when not blocking.
struct GamePointerData
{
    wl_pointer_listener original;
    void*               userData = nullptr;
};

struct GameKeyboardData
{
    wl_keyboard_listener original;
    void*                userData = nullptr;
};

static std::recursive_mutex                               gameDataMutex;
static std::unordered_map<wl_pointer*, GamePointerData>   gamePointers;
static std::unordered_map<wl_keyboard*, GameKeyboardData> gameKeyboards;

struct GamePointerDispatcherData
{
    wl_dispatcher_func_t original;
    const void*          dispatcherData = nullptr;
};

struct GameKeyboardDispatcherData
{
    wl_dispatcher_func_t original;
    const void*          dispatcherData = nullptr;
};

static std::unordered_map<wl_proxy*, GamePointerDispatcherData>  gamePointerDispatchers;
static std::unordered_map<wl_proxy*, GameKeyboardDispatcherData> gameKeyboardDispatchers;
static std::atomic<bool>                                         gameCursorHidden{false};

struct PointerFocusState
{
    wl_surface*                  surface          = nullptr;
    uint32_t                     lastSerial       = 0;
    uint32_t                     lastTime         = 0;
    uint32_t                     motionTime       = 0;
    wl_fixed_t                   x                = 0;
    wl_fixed_t                   y                = 0;
    bool                         delivered        = false;
    bool                         motionSuppressed = false;
    std::unordered_set<uint32_t> pressedButtons;
};

struct KeyboardFocusState
{
    wl_surface*                  surface    = nullptr;
    uint32_t                     lastSerial = 0;
    uint32_t                     lastTime   = 0;
    bool                         delivered  = false;
    std::unordered_set<uint32_t> pressedKeys;
};

static std::mutex                                        focusMutex;
static std::unordered_map<wl_proxy*, PointerFocusState>  pointerFocusStates;
static std::unordered_map<wl_proxy*, KeyboardFocusState> keyboardFocusStates;

static void recordPointerEnter(wl_proxy* proxy, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
    std::lock_guard lock(focusMutex);
    auto&           state  = pointerFocusStates[proxy];
    state.surface          = surface;
    state.lastSerial       = serial;
    state.delivered        = true;
    state.x                = x;
    state.y                = y;
    state.motionSuppressed = false;
}

static bool recordPointerLeave(wl_proxy* proxy)
{
    std::lock_guard lock(focusMutex);
    auto&           state   = pointerFocusStates[proxy];
    const bool      deliver = state.delivered;
    state.delivered         = false;
    state.motionSuppressed  = false;
    state.pressedButtons.clear();
    return deliver;
}

static bool recordPointerMotion(wl_proxy* proxy, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    std::lock_guard lock(focusMutex);
    auto&           state = pointerFocusStates[proxy];
    if (!state.delivered)
        return false;
    state.motionTime       = time;
    state.x                = x;
    state.y                = y;
    state.motionSuppressed = vkShade::isInputBlocked();
    return !state.motionSuppressed;
}

static bool recordPointerButton(wl_proxy* proxy, uint32_t serial, uint32_t time, uint32_t button, uint32_t buttonState)
{
    std::lock_guard lock(focusMutex);
    auto&           state = pointerFocusStates[proxy];
    state.lastSerial      = serial;
    state.lastTime        = time;
    if (vkShade::isInputBlocked() || !state.delivered)
        return false;
    if (buttonState == WL_POINTER_BUTTON_STATE_PRESSED)
        state.pressedButtons.insert(button);
    else if (state.pressedButtons.erase(button) == 0)
        return false;
    return true;
}

static bool recordKeyboardEnter(wl_proxy* proxy, uint32_t serial, wl_surface* surface, const wl_array* keys)
{
    std::lock_guard lock(focusMutex);
    auto&           state = keyboardFocusStates[proxy];
    state.surface         = surface;
    state.lastSerial      = serial;
    state.delivered       = true;
    state.pressedKeys.clear();
    if (vkShade::isInputBlocked())
        return true;
    const auto* values = static_cast<const uint32_t*>(keys->data);
    for (size_t i = 0; i < keys->size / sizeof(uint32_t); ++i)
        state.pressedKeys.insert(values[i]);
    return false;
}

static bool recordKeyboardLeave(wl_proxy* proxy)
{
    std::lock_guard lock(focusMutex);
    auto&           state   = keyboardFocusStates[proxy];
    const bool      deliver = state.delivered;
    state.delivered         = false;
    state.pressedKeys.clear();
    return deliver;
}

static bool recordKeyboardKey(wl_proxy* proxy, uint32_t serial, uint32_t time, uint32_t key, uint32_t keyState)
{
    std::lock_guard lock(focusMutex);
    auto&           state = keyboardFocusStates[proxy];
    state.lastSerial      = serial;
    state.lastTime        = time;
    if (vkShade::isInputBlocked() || !state.delivered)
        return false;
    if (keyState == WL_KEYBOARD_KEY_STATE_PRESSED)
        state.pressedKeys.insert(key);
    else if (state.pressedKeys.erase(key) == 0)
        return false;
    return true;
}

static void forgetGameFocus(wl_proxy* proxy)
{
    std::lock_guard gameLock(gameDataMutex);
    std::lock_guard focusLock(focusMutex);
    pointerFocusStates.erase(proxy);
    keyboardFocusStates.erase(proxy);
    gamePointers.erase(reinterpret_cast<wl_pointer*>(proxy));
    gameKeyboards.erase(reinterpret_cast<wl_keyboard*>(proxy));
    gamePointerDispatchers.erase(proxy);
    gameKeyboardDispatchers.erase(proxy);
}

static bool hasDeliveredPointerFocus(wl_proxy* proxy)
{
    std::lock_guard lock(focusMutex);
    auto            it = pointerFocusStates.find(proxy);
    return it != pointerFocusStates.end() && it->second.delivered;
}

static bool hasDeliveredKeyboardFocus(wl_proxy* proxy)
{
    std::lock_guard lock(focusMutex);
    auto            it = keyboardFocusStates.find(proxy);
    return it != keyboardFocusStates.end() && it->second.delivered;
}

static void forgetSurfaceFocus(wl_surface* surface)
{
    std::lock_guard lock(focusMutex);
    for (auto& [proxy, state] : pointerFocusStates)
    {
        if (state.surface == surface)
        {
            state.surface          = nullptr;
            state.delivered        = false;
            state.motionSuppressed = false;
            state.pressedButtons.clear();
        }
    }
    for (auto& [proxy, state] : keyboardFocusStates)
    {
        if (state.surface == surface)
        {
            state.surface   = nullptr;
            state.delivered = false;
            state.pressedKeys.clear();
        }
    }
}

struct GameRelativePointerData
{
    zwp_relative_pointer_v1_listener original{};
    void*                            userData = nullptr;
};

struct GameLockedPointerData
{
    zwp_locked_pointer_v1_listener original{};
    void*                          userData = nullptr;
};

struct ExtensionDispatcherData
{
    wl_dispatcher_func_t original;
    const void*          dispatcherData = nullptr;
};

static std::mutex                                             extensionMutex;
static std::unordered_map<wl_proxy*, GameRelativePointerData> gameRelativePointers;
static std::unordered_map<wl_proxy*, GameLockedPointerData>   gameLockedPointers;
static std::unordered_map<wl_proxy*, ExtensionDispatcherData> relativePointerDispatchers;
static std::unordered_map<wl_proxy*, ExtensionDispatcherData> lockedPointerDispatchers;
static std::unordered_set<wl_proxy*>                          activeLockedPointers;
static std::unordered_set<wl_proxy*>                          pointerCursorDevices;
static std::atomic<bool>                                      gamePointerLocked{false};

static bool isTrackedExtension(const char* cls)
{
    return cls
           && (std::strcmp(cls, "zwp_relative_pointer_v1") == 0 || std::strcmp(cls, "zwp_locked_pointer_v1") == 0
               || std::strcmp(cls, "wp_cursor_shape_device_v1") == 0);
}

static void setGamePointerLocked(wl_proxy* proxy, bool locked)
{
    if (locked)
        activeLockedPointers.insert(proxy);
    else
        activeLockedPointers.erase(proxy);
    gamePointerLocked.store(!activeLockedPointers.empty(), std::memory_order_release);
}

static void forgetExtensionProxy(wl_proxy* proxy)
{
    std::lock_guard lock(extensionMutex);
    gameRelativePointers.erase(proxy);
    gameLockedPointers.erase(proxy);
    relativePointerDispatchers.erase(proxy);
    lockedPointerDispatchers.erase(proxy);
    pointerCursorDevices.erase(proxy);
    setGamePointerLocked(proxy, false);
}

static void relativeMotion(void*,
                           zwp_relative_pointer_v1* pointer,
                           uint32_t                 hi,
                           uint32_t                 lo,
                           wl_fixed_t               dx,
                           wl_fixed_t               dy,
                           wl_fixed_t               dxUnaccelerated,
                           wl_fixed_t               dyUnaccelerated)
{
    if (vkShade::isInputBlocked())
        return;
    GameRelativePointerData listener{};
    {
        std::lock_guard lock(extensionMutex);
        auto            it = gameRelativePointers.find(reinterpret_cast<wl_proxy*>(pointer));
        if (it == gameRelativePointers.end())
            return;
        listener = it->second;
    }
    if (listener.original.relative_motion)
        listener.original.relative_motion(listener.userData, pointer, hi, lo, dx, dy, dxUnaccelerated, dyUnaccelerated);
}

static zwp_relative_pointer_v1_listener wrapperRelativePointerListener = {
    .relative_motion = relativeMotion,
};

static void lockedPointerEvent(void*, zwp_locked_pointer_v1* pointer, bool locked)
{
    GameLockedPointerData listener{};
    {
        std::lock_guard lock(extensionMutex);
        auto*           proxy = reinterpret_cast<wl_proxy*>(pointer);
        setGamePointerLocked(proxy, locked);
        auto it = gameLockedPointers.find(proxy);
        if (it == gameLockedPointers.end())
            return;
        listener = it->second;
    }
    if (locked && listener.original.locked)
        listener.original.locked(listener.userData, pointer);
    else if (!locked && listener.original.unlocked)
        listener.original.unlocked(listener.userData, pointer);
}

static void lockedPointerLocked(void* data, zwp_locked_pointer_v1* pointer)
{
    lockedPointerEvent(data, pointer, true);
}

static void lockedPointerUnlocked(void* data, zwp_locked_pointer_v1* pointer)
{
    lockedPointerEvent(data, pointer, false);
}

static zwp_locked_pointer_v1_listener wrapperLockedPointerListener = {
    .locked   = lockedPointerLocked,
    .unlocked = lockedPointerUnlocked,
};

// ── Wrapper pointer listener ─────────────────────────────────────────────────

static void wp_enter(void* data, wl_pointer* p, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    std::lock_guard lock(gameDataMutex);
    recordPointerEnter(reinterpret_cast<wl_proxy*>(p), serial, s, x, y);
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.enter)
        it->second.original.enter(it->second.userData, p, serial, s, x, y);
}

static void wp_leave(void* data, wl_pointer* p, uint32_t serial, wl_surface* s)
{
    std::lock_guard lock(gameDataMutex);
    if (!recordPointerLeave(reinterpret_cast<wl_proxy*>(p)))
        return;
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.leave)
        it->second.original.leave(it->second.userData, p, serial, s);
}

static void wp_motion(void* data, wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    std::lock_guard lock(gameDataMutex);
    if (!recordPointerMotion(reinterpret_cast<wl_proxy*>(p), time, x, y))
        return;
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.motion)
        it->second.original.motion(it->second.userData, p, time, x, y);
}

static void wp_button(void* data, wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    // Always mirror button state to overlay — the game's pointer receives
    // releases via Wayland's implicit grab that our overlay pointer never sees.
    if (vkShade::isWayland())
        vkShade::mirrorButtonState(button, state == WL_POINTER_BUTTON_STATE_PRESSED);

    std::lock_guard lock(gameDataMutex);
    if (!recordPointerButton(reinterpret_cast<wl_proxy*>(p), serial, time, button, state))
        return;
    auto it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.button)
        it->second.original.button(it->second.userData, p, serial, time, button, state);
}

static void wp_axis(void* data, wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (vkShade::isInputBlocked())
        return;
    std::lock_guard lock(gameDataMutex);
    auto            it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis)
        it->second.original.axis(it->second.userData, p, time, axis, value);
}

static void wp_frame(void* data, wl_pointer* p)
{
    std::lock_guard lock(gameDataMutex);
    auto            it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.frame)
        it->second.original.frame(it->second.userData, p);
}

static void wp_axis_source(void* data, wl_pointer* p, uint32_t source)
{
    if (vkShade::isInputBlocked())
        return;
    std::lock_guard lock(gameDataMutex);
    auto            it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_source)
        it->second.original.axis_source(it->second.userData, p, source);
}

static void wp_axis_stop(void* data, wl_pointer* p, uint32_t time, uint32_t axis)
{
    if (vkShade::isInputBlocked())
        return;
    std::lock_guard lock(gameDataMutex);
    auto            it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_stop)
        it->second.original.axis_stop(it->second.userData, p, time, axis);
}

static void wp_axis_discrete(void* data, wl_pointer* p, uint32_t axis, int32_t discrete)
{
    if (vkShade::isInputBlocked())
        return;
    std::lock_guard lock(gameDataMutex);
    auto            it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_discrete)
        it->second.original.axis_discrete(it->second.userData, p, axis, discrete);
}

static void wp_axis_value120(void* data, wl_pointer* p, uint32_t axis, int32_t value120)
{
    if (vkShade::isInputBlocked())
        return;
    std::lock_guard lock(gameDataMutex);
    auto            it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_value120)
        it->second.original.axis_value120(it->second.userData, p, axis, value120);
}

static void wp_axis_relative_direction(void* data, wl_pointer* p, uint32_t axis, uint32_t direction)
{
    if (vkShade::isInputBlocked())
        return;
    std::lock_guard lock(gameDataMutex);
    auto            it = gamePointers.find(p);
    if (it != gamePointers.end() && it->second.original.axis_relative_direction)
        it->second.original.axis_relative_direction(it->second.userData, p, axis, direction);
}

static const wl_pointer_listener wrapperPointerListener = {
    .enter                   = wp_enter,
    .leave                   = wp_leave,
    .motion                  = wp_motion,
    .button                  = wp_button,
    .axis                    = wp_axis,
    .frame                   = wp_frame,
    .axis_source             = wp_axis_source,
    .axis_stop               = wp_axis_stop,
    .axis_discrete           = wp_axis_discrete,
    .axis_value120           = wp_axis_value120,
    .axis_relative_direction = wp_axis_relative_direction,
};

// ── Wrapper keyboard listener ────────────────────────────────────────────────

static void wk_keymap(void* data, wl_keyboard* kb, uint32_t format, int32_t fd, uint32_t size)
{
    // Always forward keymap — needed for initialization
    std::lock_guard lock(gameDataMutex);
    auto            it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.keymap)
        it->second.original.keymap(it->second.userData, kb, format, fd, size);
}

static void wk_enter(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s, wl_array* keys)
{
    std::lock_guard lock(gameDataMutex);
    const bool      hideKeys = recordKeyboardEnter(reinterpret_cast<wl_proxy*>(kb), serial, s, keys);
    auto            it       = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.enter)
    {
        wl_array emptyKeys{};
        it->second.original.enter(it->second.userData, kb, serial, s, hideKeys ? &emptyKeys : keys);
    }
}

static void wk_leave(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s)
{
    std::lock_guard lock(gameDataMutex);
    if (!recordKeyboardLeave(reinterpret_cast<wl_proxy*>(kb)))
        return;
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.leave)
        it->second.original.leave(it->second.userData, kb, serial, s);
}

static void wk_key(void* data, wl_keyboard* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    std::lock_guard lock(gameDataMutex);
    if (!recordKeyboardKey(reinterpret_cast<wl_proxy*>(kb), serial, time, key, state))
        return;
    auto it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.key)
        it->second.original.key(it->second.userData, kb, serial, time, key, state);
}

static void wk_modifiers(void* data, wl_keyboard* kb, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    // Always forward modifiers — games need modifier state for keybinds
    std::lock_guard lock(gameDataMutex);
    auto            it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.modifiers)
        it->second.original.modifiers(it->second.userData, kb, serial, depressed, latched, locked, group);
}

static void wk_repeat_info(void* data, wl_keyboard* kb, int32_t rate, int32_t delay)
{
    // Always forward repeat info
    std::lock_guard lock(gameDataMutex);
    auto            it = gameKeyboards.find(kb);
    if (it != gameKeyboards.end() && it->second.original.repeat_info)
        it->second.original.repeat_info(it->second.userData, kb, rate, delay);
}

static const wl_keyboard_listener wrapperKeyboardListener = {
    .keymap      = wk_keymap,
    .enter       = wk_enter,
    .leave       = wk_leave,
    .key         = wk_key,
    .modifiers   = wk_modifiers,
    .repeat_info = wk_repeat_info,
};

// ── Wrapper dispatcher callbacks ────────────────────────────────────────────

static int wd_pointer(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto*           proxy = static_cast<wl_proxy*>(target);
    std::lock_guard lock(gameDataMutex);

    switch (opcode)
    {
        case 0: recordPointerEnter(proxy, args[0].u, reinterpret_cast<wl_surface*>(args[1].o), args[2].f, args[3].f); break;
        case 1:
            if (!recordPointerLeave(proxy))
                return 0;
            break;
        case 2:
            if (!recordPointerMotion(proxy, args[0].u, args[1].f, args[2].f))
                return 0;
            break;
        case 3:
            if (vkShade::isWayland())
                vkShade::mirrorButtonState(args[2].u, args[3].u == WL_POINTER_BUTTON_STATE_PRESSED);
            if (!recordPointerButton(proxy, args[0].u, args[1].u, args[2].u, args[3].u))
                return 0;
            break;
        case 4:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
            if (vkShade::isInputBlocked())
                return 0;
            break;
        default: break;
    }

    auto it = gamePointerDispatchers.find(proxy);
    if (it != gamePointerDispatchers.end() && it->second.original)
        return it->second.original(it->second.dispatcherData, target, opcode, msg, args);
    return 0;
}

static int wd_keyboard(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto*           proxy = static_cast<wl_proxy*>(target);
    std::lock_guard lock(gameDataMutex);
    wl_array        emptyKeys{};
    wl_argument     enterArgs[3]{};
    wl_argument*    forwardedArgs = args;

    switch (opcode)
    {
        case 1:
            if (recordKeyboardEnter(proxy, args[0].u, reinterpret_cast<wl_surface*>(args[1].o), args[2].a))
            {
                enterArgs[0]   = args[0];
                enterArgs[1]   = args[1];
                enterArgs[2].a = &emptyKeys;
                forwardedArgs  = enterArgs;
            }
            break;
        case 2:
            if (!recordKeyboardLeave(proxy))
                return 0;
            break;
        case 3:
            if (!recordKeyboardKey(proxy, args[0].u, args[1].u, args[2].u, args[3].u))
                return 0;
            break;
        default: break;
    }

    auto it = gameKeyboardDispatchers.find(proxy);
    if (it != gameKeyboardDispatchers.end() && it->second.original)
        return it->second.original(it->second.dispatcherData, target, opcode, msg, forwardedArgs);
    return 0;
}

static int wd_relative_pointer(const void*, void* target, uint32_t opcode, const wl_message* msg, wl_argument* args)
{
    if (opcode == 0 && vkShade::isInputBlocked())
        return 0;

    ExtensionDispatcherData dispatcher{};
    {
        std::lock_guard lock(extensionMutex);
        auto            it = relativePointerDispatchers.find(static_cast<wl_proxy*>(target));
        if (it == relativePointerDispatchers.end())
            return 0;
        dispatcher = it->second;
    }
    return dispatcher.original ? dispatcher.original(dispatcher.dispatcherData, target, opcode, msg, args) : 0;
}

static int wd_locked_pointer(const void*, void* target, uint32_t opcode, const wl_message* msg, wl_argument* args)
{
    ExtensionDispatcherData dispatcher{};
    {
        std::lock_guard lock(extensionMutex);
        auto*           proxy = static_cast<wl_proxy*>(target);
        if (opcode == 0 || opcode == 1)
            setGamePointerLocked(proxy, opcode == 0);
        auto it = lockedPointerDispatchers.find(proxy);
        if (it == lockedPointerDispatchers.end())
            return 0;
        dispatcher = it->second;
    }
    return dispatcher.original ? dispatcher.original(dispatcher.dispatcherData, target, opcode, msg, args) : 0;
}

// ── Symbol interposition ─────────────────────────────────────────────────────
//
// wl_pointer_add_listener is a static inline in <wayland-client-protocol.h>
// that calls wl_proxy_add_listener. We interpose on the underlying C function
// so that even inlined calls are caught.

using AddListenerFn = int (*)(struct wl_proxy*, void (**)(void), void*);
using DlsymFn       = void* (*)(void*, const char*);

static DlsymFn getRealDlsym()
{
    static DlsymFn fn = nullptr;
    if (fn)
        return fn;

    // Bootstrap with a versioned lookup, then include unversioned interposers after us.
    auto libcDlsym = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    if (!libcDlsym)
        libcDlsym = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"));
    if (libcDlsym)
        fn = reinterpret_cast<DlsymFn>(libcDlsym(RTLD_NEXT, "dlsym"));
    return fn;
}

template<typename FnType>
static FnType resolveWaylandSymbol(const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    // Preferred path for normal implicit-layer usage.
    void* sym = realDlsym(RTLD_NEXT, symbol);
    if (sym)
        return reinterpret_cast<FnType>(sym);

    // Fallback for preload / early-load scenarios where RTLD_NEXT may not
    // reach libwayland-client yet.
    static void* waylandHandle = nullptr;
    if (!waylandHandle)
    {
        waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOLOAD | RTLD_NOW);
        if (!waylandHandle)
            waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    }

    if (waylandHandle)
    {
        sym = realDlsym(waylandHandle, symbol);
        if (sym)
            return reinterpret_cast<FnType>(sym);
    }

    return nullptr;
}

static AddListenerFn getRealAddListener()
{
    static AddListenerFn fn = resolveWaylandSymbol<AddListenerFn>("wl_proxy_add_listener");
    return fn;
}

using AddDispatcherFn     = int (*)(struct wl_proxy*, wl_dispatcher_func_t, const void*, void*);
using MarshalArrayFlagsFn = wl_proxy* (*)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, union wl_argument*);
using DestroyProxyFn      = void (*)(struct wl_proxy*);
using CreateWrapperFn     = void* (*)(void*);
using DestroyWrapperFn    = void (*)(void*);

static AddDispatcherFn getRealAddDispatcher()
{
    static AddDispatcherFn fn = resolveWaylandSymbol<AddDispatcherFn>("wl_proxy_add_dispatcher");
    return fn;
}

static MarshalArrayFlagsFn getRealMarshalArrayFlags()
{
    static MarshalArrayFlagsFn fn = resolveWaylandSymbol<MarshalArrayFlagsFn>("wl_proxy_marshal_array_flags");
    return fn;
}

static DestroyProxyFn getRealDestroyProxy()
{
    static DestroyProxyFn fn = resolveWaylandSymbol<DestroyProxyFn>("wl_proxy_destroy");
    return fn;
}

static CreateWrapperFn getRealCreateWrapper()
{
    static CreateWrapperFn fn = resolveWaylandSymbol<CreateWrapperFn>("wl_proxy_create_wrapper");
    return fn;
}

static DestroyWrapperFn getRealDestroyWrapper()
{
    static DestroyWrapperFn fn = resolveWaylandSymbol<DestroyWrapperFn>("wl_proxy_wrapper_destroy");
    return fn;
}

extern "C" __attribute__((visibility("default"))) void* wl_proxy_create_wrapper(void* proxy)
{
    CreateWrapperFn real = getRealCreateWrapper();
    if (!real)
        return nullptr;
    void* wrapper = real(proxy);
    if (wrapper)
    {
        const char* cls = wl_proxy_get_class(static_cast<wl_proxy*>(proxy));
        if (cls && (std::strcmp(cls, "wl_surface") == 0 || std::strcmp(cls, "wp_viewport") == 0))
            vkShade::waylandSurfaceGeometryTracker().associateProxyWrapper(wrapper, proxy);
    }
    return wrapper;
}

extern "C" __attribute__((visibility("default"))) void wl_proxy_wrapper_destroy(void* wrapper)
{
    DestroyWrapperFn real = getRealDestroyWrapper();
    if (!real)
        return;
    vkShade::waylandSurfaceGeometryTracker().forgetProxyWrapper(wrapper);
    real(wrapper);
}

extern "C" __attribute__((visibility("default"))) int wl_proxy_add_listener(struct wl_proxy* proxy, void (**implementation)(void), void* data)
{
    AddListenerFn real = getRealAddListener();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, implementation, data);

    // Skip overlay-owned proxies — they use their own listeners
    if (vkShade::isOverlayProxy(proxy))
        return real(proxy, implementation, data);

    if (std::strcmp(cls, "zwp_relative_pointer_v1") == 0)
    {
        {
            std::lock_guard lock(extensionMutex);
            gameRelativePointers[proxy] = {*reinterpret_cast<const zwp_relative_pointer_v1_listener*>(implementation), data};
        }
        return real(proxy, reinterpret_cast<void (**)(void)>(&wrapperRelativePointerListener), nullptr);
    }

    if (std::strcmp(cls, "zwp_locked_pointer_v1") == 0)
    {
        {
            std::lock_guard lock(extensionMutex);
            gameLockedPointers[proxy] = {*reinterpret_cast<const zwp_locked_pointer_v1_listener*>(implementation), data};
        }
        return real(proxy, reinterpret_cast<void (**)(void)>(&wrapperLockedPointerListener), nullptr);
    }

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto* gameListener = (const wl_pointer_listener*) implementation;
        auto* ptr          = reinterpret_cast<wl_pointer*>(proxy);

        {
            std::lock_guard lock(gameDataMutex);
            GamePointerData gpd;
            gpd.original      = *gameListener;
            gpd.userData      = data;
            gamePointers[ptr] = gpd;
            gamePointerDispatchers.erase(proxy);
        }

        vkShade::Logger::debug("Wayland interpose: wrapped game pointer listener");
        return real(proxy, (void (**)(void)) &wrapperPointerListener, nullptr);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto* gameListener = (const wl_keyboard_listener*) implementation;
        auto* kb           = reinterpret_cast<wl_keyboard*>(proxy);

        {
            std::lock_guard  lock(gameDataMutex);
            GameKeyboardData gkd;
            gkd.original      = *gameListener;
            gkd.userData      = data;
            gameKeyboards[kb] = gkd;
            gameKeyboardDispatchers.erase(proxy);
        }

        vkShade::Logger::debug("Wayland interpose: wrapped game keyboard listener");
        return real(proxy, (void (**)(void)) &wrapperKeyboardListener, nullptr);
    }

    return real(proxy, implementation, data);
}

extern "C" __attribute__((visibility("default"))) int
wl_proxy_add_dispatcher(struct wl_proxy* proxy, wl_dispatcher_func_t dispatcher_func, const void* dispatcher_data, void* data);

extern "C" __attribute__((visibility("default"))) wl_proxy* wl_proxy_marshal_array_flags(
    struct wl_proxy* proxy, uint32_t opcode, const struct wl_interface* iface, uint32_t version, uint32_t flags, union wl_argument* args)
{
    MarshalArrayFlagsFn real = getRealMarshalArrayFlags();
    if (!real)
        return nullptr;

    // Block compositor window move/resize requests while overlay input is blocked.
    // xdg_toplevel requests:
    //   4: show_window_menu
    //   5: move
    //   6: resize
    const char* cls = wl_proxy_get_class(proxy);
    if ((flags & WL_MARSHAL_FLAG_DESTROY) && isTrackedExtension(cls))
        forgetExtensionProxy(proxy);
    if ((flags & WL_MARSHAL_FLAG_DESTROY) && cls && (std::strcmp(cls, "wl_pointer") == 0 || std::strcmp(cls, "wl_keyboard") == 0))
        forgetGameFocus(proxy);

    if (cls && std::strcmp(cls, "wp_viewporter") == 0 && opcode == WP_VIEWPORTER_GET_VIEWPORT)
    {
        wl_proxy* viewport = real(proxy, opcode, iface, version, flags, args);
        if (viewport)
            vkShade::waylandSurfaceGeometryTracker().associateViewport(viewport, args[1].o);
        return viewport;
    }

    // wp_cursor_shape_manager_v1.get_pointer creates a cursor device for a mouse.
    if (cls && std::strcmp(cls, "wp_cursor_shape_manager_v1") == 0 && opcode == 1)
    {
        wl_proxy* device = real(proxy, opcode, iface, version, flags, args);
        if (device)
        {
            std::lock_guard lock(extensionMutex);
            pointerCursorDevices.insert(device);
        }
        return device;
    }

    if (cls && std::strcmp(cls, "wp_cursor_shape_device_v1") == 0 && opcode == 1)
    {
        std::lock_guard lock(extensionMutex);
        if (pointerCursorDevices.count(proxy))
            gameCursorHidden.store(false, std::memory_order_release);
    }

    if (cls && std::strcmp(cls, "wp_viewport") == 0)
    {
        auto& tracker = vkShade::waylandSurfaceGeometryTracker();
        if (opcode == WP_VIEWPORT_SET_SOURCE)
            tracker.setViewportSource(
                proxy, wl_fixed_to_double(args[0].f), wl_fixed_to_double(args[1].f), wl_fixed_to_double(args[2].f), wl_fixed_to_double(args[3].f));
        else if (opcode == WP_VIEWPORT_SET_DESTINATION)
            tracker.setViewportDestination(proxy, args[0].i, args[1].i);
        else if (opcode == WP_VIEWPORT_DESTROY)
            tracker.destroyViewport(proxy);
    }
    else if (cls && std::strcmp(cls, "wl_surface") == 0)
    {
        auto& tracker = vkShade::waylandSurfaceGeometryTracker();
        if (opcode == WL_SURFACE_SET_BUFFER_SCALE)
            tracker.setBufferScale(proxy, args[0].i);
        else if (opcode == WL_SURFACE_SET_BUFFER_TRANSFORM)
            tracker.setBufferTransform(proxy, args[0].i);
        else if (opcode == WL_SURFACE_COMMIT)
            tracker.commit(proxy);
        else if (opcode == WL_SURFACE_DESTROY)
        {
            auto* surface = reinterpret_cast<wl_surface*>(const_cast<void*>(tracker.canonicalProxy(proxy)));
            tracker.destroySurface(proxy);
            forgetSurfaceFocus(surface);
        }
    }

    if (cls && std::strcmp(cls, "wl_pointer") == 0 && opcode == 0 && !vkShade::isOverlayProxy(proxy))
    {
        gameCursorHidden.store(args[1].o == nullptr, std::memory_order_release);
    }

    if (vkShade::isInputBlocked() && cls && std::strcmp(cls, "xdg_toplevel") == 0 && (opcode == 4 || opcode == 5 || opcode == 6))
    {
        return nullptr;
    }

    return real(proxy, opcode, iface, version, flags, args);
}

extern "C" __attribute__((visibility("default"))) void wl_proxy_destroy(struct wl_proxy* proxy)
{
    DestroyProxyFn real = getRealDestroyProxy();
    if (!real)
        return;
    const char* cls = wl_proxy_get_class(proxy);
    if (isTrackedExtension(cls))
        forgetExtensionProxy(proxy);
    else if (cls && std::strcmp(cls, "wp_viewport") == 0)
        vkShade::waylandSurfaceGeometryTracker().forgetViewport(proxy);
    else if (cls && std::strcmp(cls, "wl_surface") == 0)
    {
        auto& tracker = vkShade::waylandSurfaceGeometryTracker();
        auto* surface = reinterpret_cast<wl_surface*>(const_cast<void*>(tracker.canonicalProxy(proxy)));
        tracker.destroySurface(proxy);
        forgetSurfaceFocus(surface);
    }
    else if (cls && (std::strcmp(cls, "wl_pointer") == 0 || std::strcmp(cls, "wl_keyboard") == 0))
        forgetGameFocus(proxy);
    real(proxy);
}

extern "C" __attribute__((visibility("hidden"), used)) void* vkshadeDlsym(void* handle, const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    void* resolved = realDlsym(handle, symbol);

    // Keep behavior unchanged unless vkShade is explicitly enabled.
    const char* enabled = std::getenv("ENABLE_VKSHADE");
    if (!enabled || std::strcmp(enabled, "1") != 0 || !resolved)
        return resolved;

    bool wantsListener          = (std::strcmp(symbol, "wl_proxy_add_listener") == 0);
    bool wantsDispatcher        = (std::strcmp(symbol, "wl_proxy_add_dispatcher") == 0);
    bool wantsMarshalArrayFlags = (std::strcmp(symbol, "wl_proxy_marshal_array_flags") == 0);
    bool wantsDestroy           = (std::strcmp(symbol, "wl_proxy_destroy") == 0);
    bool wantsCreateWrapper     = (std::strcmp(symbol, "wl_proxy_create_wrapper") == 0);
    bool wantsDestroyWrapper    = (std::strcmp(symbol, "wl_proxy_wrapper_destroy") == 0);
    if (!wantsListener && !wantsDispatcher && !wantsMarshalArrayFlags && !wantsDestroy && !wantsCreateWrapper && !wantsDestroyWrapper)
        return resolved;

    Dl_info info{};
    if (dladdr(resolved, &info) == 0 || !info.dli_fname)
        return resolved;

    std::string module(info.dli_fname);
    std::transform(module.begin(), module.end(), module.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Only redirect lookups that currently resolve to libwayland-client.
    if (module.find("libwayland-client") == std::string::npos)
    {
        return resolved;
    }

    if (wantsListener)
        return reinterpret_cast<void*>(&wl_proxy_add_listener);
    if (wantsDispatcher)
        return reinterpret_cast<void*>(&wl_proxy_add_dispatcher);
    if (wantsDestroy)
        return reinterpret_cast<void*>(&wl_proxy_destroy);
    if (wantsCreateWrapper)
        return reinterpret_cast<void*>(&wl_proxy_create_wrapper);
    if (wantsDestroyWrapper)
        return reinterpret_cast<void*>(&wl_proxy_wrapper_destroy);
    return reinterpret_cast<void*>(&wl_proxy_marshal_array_flags);
}

// RTLD_NEXT is relative to the caller of dlsym. Forward it with a tail call so
// the dynamic linker sees the original caller instead of this interposer.
#ifndef __has_cpp_attribute
#define __has_cpp_attribute(attribute) 0
#endif
#if defined(__x86_64__) || defined(__aarch64__) || defined(__i386__)
extern "C" __attribute__((visibility("hidden"), used)) DlsymFn vkshadeRealDlsym()
{
    return getRealDlsym();
}

extern "C" __attribute__((visibility("default"), naked)) void* dlsym(void*, const char*)
{
#if defined(__x86_64__)
    __asm__ volatile("cmpq $-1, %rdi\n"
                     "jne vkshadeDlsym\n"
                     "pushq %rdi\n"
                     "pushq %rsi\n"
                     "subq $8, %rsp\n"
                     "call vkshadeRealDlsym\n"
                     "addq $8, %rsp\n"
                     "popq %rsi\n"
                     "popq %rdi\n"
                     "testq %rax, %rax\n"
                     "jz 1f\n"
                     "jmp *%rax\n"
                     "1: ret\n");
#elif defined(__aarch64__)
    __asm__ volatile("cmn x0, #1\n"
                     "b.ne vkshadeDlsym\n"
                     "stp x0, x1, [sp, #-32]!\n"
                     "str x30, [sp, #16]\n"
                     "bl vkshadeRealDlsym\n"
                     "mov x16, x0\n"
                     "ldp x0, x1, [sp]\n"
                     "ldr x30, [sp, #16]\n"
                     "add sp, sp, #32\n"
                     "cbz x16, 1f\n"
                     "br x16\n"
                     "1: mov x0, xzr\n"
                     "ret\n");
#else
    __asm__ volatile("cmpl $-1, 4(%esp)\n"
                     "jne vkshadeDlsym\n"
                     "subl $12, %esp\n"
                     "call vkshadeRealDlsym\n"
                     "addl $12, %esp\n"
                     "testl %eax, %eax\n"
                     "jz 1f\n"
                     "jmp *%eax\n"
                     "1: ret\n");
#endif
}
#elif __has_cpp_attribute(gnu::musttail)
extern "C" __attribute__((visibility("default"))) void* dlsym(void* handle, const char* symbol)
{
    if (handle == RTLD_NEXT)
    {
        DlsymFn real = getRealDlsym();
        if (!real)
            return nullptr;
        [[gnu::musttail]] return real(handle, symbol);
    }
    return vkshadeDlsym(handle, symbol);
}
#elif __has_cpp_attribute(clang::musttail)
extern "C" __attribute__((visibility("default"))) void* dlsym(void* handle, const char* symbol)
{
    if (handle == RTLD_NEXT)
    {
        DlsymFn real = getRealDlsym();
        if (!real)
            return nullptr;
        [[clang::musttail]] return real(handle, symbol);
    }
    return vkshadeDlsym(handle, symbol);
}
#else
static_assert(false, "Correct RTLD_NEXT interposition requires tail-call support on this architecture");
#endif

extern "C" __attribute__((visibility("default"))) int
wl_proxy_add_dispatcher(struct wl_proxy* proxy, wl_dispatcher_func_t dispatcher_func, const void* dispatcher_data, void* data)
{
    AddDispatcherFn real = getRealAddDispatcher();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, dispatcher_func, dispatcher_data, data);

    // Skip overlay-owned proxies — they use their own dispatchers
    if (vkShade::isOverlayProxy(proxy))
        return real(proxy, dispatcher_func, dispatcher_data, data);

    if (std::strcmp(cls, "zwp_relative_pointer_v1") == 0)
    {
        {
            std::lock_guard lock(extensionMutex);
            relativePointerDispatchers[proxy] = {dispatcher_func, dispatcher_data};
        }
        return real(proxy, wd_relative_pointer, nullptr, data);
    }

    if (std::strcmp(cls, "zwp_locked_pointer_v1") == 0)
    {
        {
            std::lock_guard lock(extensionMutex);
            lockedPointerDispatchers[proxy] = {dispatcher_func, dispatcher_data};
        }
        return real(proxy, wd_locked_pointer, nullptr, data);
    }

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto* ptr = static_cast<wl_proxy*>(proxy);

        {
            std::lock_guard           lock(gameDataMutex);
            GamePointerDispatcherData gpd;
            gpd.original                = dispatcher_func;
            gpd.dispatcherData          = dispatcher_data;
            gamePointerDispatchers[ptr] = gpd;
            gamePointers.erase(reinterpret_cast<wl_pointer*>(proxy));
        }

        vkShade::Logger::debug("Wayland interpose: wrapped game pointer dispatcher");
        return real(proxy, wd_pointer, nullptr, data);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto* kb = static_cast<wl_proxy*>(proxy);

        {
            std::lock_guard            lock(gameDataMutex);
            GameKeyboardDispatcherData gkd;
            gkd.original                = dispatcher_func;
            gkd.dispatcherData          = dispatcher_data;
            gameKeyboardDispatchers[kb] = gkd;
            gameKeyboards.erase(reinterpret_cast<wl_keyboard*>(proxy));
        }

        vkShade::Logger::debug("Wayland interpose: wrapped game keyboard dispatcher");
        return real(proxy, wd_keyboard, nullptr, data);
    }

    return real(proxy, dispatcher_func, dispatcher_data, data);
}

static void dispatchPointerEvent(wl_proxy* proxy, uint32_t opcode, wl_argument* args)
{
    GamePointerData           listener{};
    GamePointerDispatcherData dispatcher{};
    bool                      hasListener   = false;
    bool                      hasDispatcher = false;
    {
        std::lock_guard lock(gameDataMutex);
        auto            listenerIt = gamePointers.find(reinterpret_cast<wl_pointer*>(proxy));
        if (listenerIt != gamePointers.end())
        {
            listener    = listenerIt->second;
            hasListener = true;
        }
        auto dispatcherIt = gamePointerDispatchers.find(proxy);
        if (dispatcherIt != gamePointerDispatchers.end())
        {
            dispatcher    = dispatcherIt->second;
            hasDispatcher = true;
        }
    }

    auto* pointer = reinterpret_cast<wl_pointer*>(proxy);
    if (hasListener)
    {
        if (opcode == 2 && listener.original.motion)
            listener.original.motion(listener.userData, pointer, args[0].u, args[1].f, args[2].f);
        else if (opcode == 3 && listener.original.button)
            listener.original.button(listener.userData, pointer, args[0].u, args[1].u, args[2].u, args[3].u);
        else if (opcode == 5 && listener.original.frame)
            listener.original.frame(listener.userData, pointer);
    }
    else if (hasDispatcher && dispatcher.original)
        dispatcher.original(dispatcher.dispatcherData, proxy, opcode, &wl_pointer_interface.events[opcode], args);
}

static void dispatchKeyboardEvent(wl_proxy* proxy, uint32_t opcode, wl_argument* args)
{
    GameKeyboardData           listener{};
    GameKeyboardDispatcherData dispatcher{};
    bool                       hasListener   = false;
    bool                       hasDispatcher = false;
    {
        std::lock_guard lock(gameDataMutex);
        auto            listenerIt = gameKeyboards.find(reinterpret_cast<wl_keyboard*>(proxy));
        if (listenerIt != gameKeyboards.end())
        {
            listener    = listenerIt->second;
            hasListener = true;
        }
        auto dispatcherIt = gameKeyboardDispatchers.find(proxy);
        if (dispatcherIt != gameKeyboardDispatchers.end())
        {
            dispatcher    = dispatcherIt->second;
            hasDispatcher = true;
        }
    }

    auto* keyboard = reinterpret_cast<wl_keyboard*>(proxy);
    if (hasListener)
    {
        if (opcode == 3 && listener.original.key)
            listener.original.key(listener.userData, keyboard, args[0].u, args[1].u, args[2].u, args[3].u);
    }
    else if (hasDispatcher && dispatcher.original)
        dispatcher.original(dispatcher.dispatcherData, proxy, opcode, &wl_keyboard_interface.events[opcode], args);
}

namespace vkShade
{
    bool isWaylandGameCursorHidden()
    {
        return gameCursorHidden.load(std::memory_order_acquire);
    }

    bool isWaylandGamePointerLocked()
    {
        return gamePointerLocked.load(std::memory_order_acquire);
    }

    void notifyGameInputBlocked(bool blocked)
    {
        if (!blocked)
        {
            std::vector<wl_proxy*> pointers;
            {
                std::lock_guard lock(focusMutex);
                for (const auto& [proxy, state] : pointerFocusStates)
                    if (state.delivered && state.motionSuppressed)
                        pointers.push_back(proxy);
            }
            for (auto* proxy : pointers)
            {
                std::lock_guard gameLock(gameDataMutex);
                wl_argument     args[3]{};
                {
                    std::lock_guard focusLock(focusMutex);
                    auto            it = pointerFocusStates.find(proxy);
                    if (it == pointerFocusStates.end() || !it->second.delivered || !it->second.motionSuppressed || isInputBlocked())
                        continue;
                    auto& state            = it->second;
                    args[0].u              = state.motionTime;
                    args[1].f              = state.x;
                    args[2].f              = state.y;
                    state.motionSuppressed = false;
                }
                dispatchPointerEvent(proxy, 2, args);
                if (hasDeliveredPointerFocus(proxy) && wl_proxy_get_version(proxy) >= 5)
                    dispatchPointerEvent(proxy, 5, nullptr);
            }
            return;
        }
        struct Release
        {
            wl_proxy* proxy;
            uint32_t  serial;
            uint32_t  time;
            uint32_t  code;
        };
        std::vector<Release> buttons;
        std::vector<Release> keys;
        {
            std::lock_guard lock(focusMutex);
            for (auto& [proxy, state] : pointerFocusStates)
            {
                for (uint32_t button : state.pressedButtons)
                    buttons.push_back({proxy, state.lastSerial, state.lastTime, button});
                state.pressedButtons.clear();
            }
            for (auto& [proxy, state] : keyboardFocusStates)
            {
                for (uint32_t key : state.pressedKeys)
                    keys.push_back({proxy, state.lastSerial, state.lastTime, key});
                state.pressedKeys.clear();
            }
        }

        for (const auto& release : buttons)
        {
            std::lock_guard gameLock(gameDataMutex);
            if (!hasDeliveredPointerFocus(release.proxy))
                continue;
            wl_argument args[4]{};
            args[0].u = release.serial;
            args[1].u = release.time;
            args[2].u = release.code;
            args[3].u = WL_POINTER_BUTTON_STATE_RELEASED;
            dispatchPointerEvent(release.proxy, 3, args);
            if (hasDeliveredPointerFocus(release.proxy) && wl_proxy_get_version(release.proxy) >= 5)
                dispatchPointerEvent(release.proxy, 5, nullptr);
        }
        for (const auto& release : keys)
        {
            std::lock_guard gameLock(gameDataMutex);
            if (!hasDeliveredKeyboardFocus(release.proxy))
                continue;
            wl_argument args[4]{};
            args[0].u = release.serial;
            args[1].u = release.time;
            args[2].u = release.code;
            args[3].u = WL_KEYBOARD_KEY_STATE_RELEASED;
            dispatchKeyboardEvent(release.proxy, 3, args);
        }
        Logger::debug("Wayland input released game input");
    }
} // namespace vkShade
