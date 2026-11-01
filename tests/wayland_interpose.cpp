#include "wayland_interpose.hpp"
#include "wayland_pointer_geometry.hpp"
#include "input_blocker.hpp"

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-client-protocol.h"
#include "viewporter-client-protocol.h"

#include <wayland-client.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <initializer_list>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace vkShade
{
    bool isWayland()
    {
        return true;
    }
    bool isNonWaylandSurface()
    {
        return false;
    }
    void mirrorButtonState(uint32_t, bool)
    {
    }
} // namespace vkShade

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

static bool event(int peer, wl_display* display, wl_proxy* proxy, uint32_t opcode, std::initializer_list<uint32_t> arguments = {})
{
    std::vector<uint32_t> message{wl_proxy_get_id(proxy), static_cast<uint32_t>(((arguments.size() + 2) * sizeof(uint32_t)) << 16) | opcode};
    message.insert(message.end(), arguments.begin(), arguments.end());
    const size_t bytes = message.size() * sizeof(uint32_t);
    return write(peer, message.data(), bytes) == static_cast<ssize_t>(bytes) && wl_display_dispatch(display) >= 0;
}

struct ListenerState
{
    int        locked   = 0;
    int        unlocked = 0;
    int        relative = 0;
    wl_fixed_t lastDx   = 0;
};

struct FocusState
{
    int         keyboardEnters  = 0;
    int         keyboardLeaves  = 0;
    int         keyPresses      = 0;
    int         keyReleases     = 0;
    int         pointerEnters   = 0;
    int         pointerLeaves   = 0;
    int         pointerFrames   = 0;
    int         pointerMotions  = 0;
    int         buttonPresses   = 0;
    int         buttonReleases  = 0;
    size_t      keyboardKeys    = 0;
    wl_surface* keyboardSurface = nullptr;
    wl_surface* pointerSurface  = nullptr;
    uint32_t    keyboardSerial  = 0;
    uint32_t    pointerSerial   = 0;
    wl_fixed_t  pointerX        = 0;
    wl_fixed_t  pointerY        = 0;
};

static void keyboardEnter(void* data, wl_keyboard*, uint32_t serial, wl_surface* surface, wl_array* keys)
{
    auto* state = static_cast<FocusState*>(data);
    ++state->keyboardEnters;
    state->keyboardSerial  = serial;
    state->keyboardSurface = surface;
    state->keyboardKeys    = keys->size / sizeof(uint32_t);
}

static void keyboardLeave(void* data, wl_keyboard*, uint32_t, wl_surface*)
{
    ++static_cast<FocusState*>(data)->keyboardLeaves;
}
static void keyboardKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t keyState)
{
    auto* state = static_cast<FocusState*>(data);
    if (keyState == WL_KEYBOARD_KEY_STATE_PRESSED)
        ++state->keyPresses;
    else
        ++state->keyReleases;
}

static void pointerEnter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
    auto* state = static_cast<FocusState*>(data);
    ++state->pointerEnters;
    state->pointerSerial  = serial;
    state->pointerSurface = surface;
    state->pointerX       = x;
    state->pointerY       = y;
}

static void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*)
{
    ++static_cast<FocusState*>(data)->pointerLeaves;
}
static void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    auto* state = static_cast<FocusState*>(data);
    ++state->pointerMotions;
    state->pointerX = x;
    state->pointerY = y;
}
static void pointerFrame(void* data, wl_pointer*)
{
    ++static_cast<FocusState*>(data)->pointerFrames;
}
static void pointerButton(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t buttonState)
{
    auto* state = static_cast<FocusState*>(data);
    if (buttonState == WL_POINTER_BUTTON_STATE_PRESSED)
        ++state->buttonPresses;
    else
        ++state->buttonReleases;
}

struct FocusDispatcherContext
{
    FocusState* state;
    bool        pointer;
};

static int focusDispatcher(const void* data, void* target, uint32_t opcode, const wl_message*, wl_argument* args)
{
    const auto& context = *static_cast<const FocusDispatcherContext*>(data);
    if (context.pointer)
    {
        auto* pointer = static_cast<wl_pointer*>(target);
        if (opcode == 0)
            pointerEnter(context.state, pointer, args[0].u, reinterpret_cast<wl_surface*>(args[1].o), args[2].f, args[3].f);
        else if (opcode == 1)
            pointerLeave(context.state, pointer, args[0].u, reinterpret_cast<wl_surface*>(args[1].o));
        else if (opcode == 2)
            pointerMotion(context.state, pointer, args[0].u, args[1].f, args[2].f);
        else if (opcode == 3)
            pointerButton(context.state, pointer, args[0].u, args[1].u, args[2].u, args[3].u);
        else if (opcode == 5)
            pointerFrame(context.state, pointer);
    }
    else
    {
        auto* keyboard = static_cast<wl_keyboard*>(target);
        if (opcode == 1)
            keyboardEnter(context.state, keyboard, args[0].u, reinterpret_cast<wl_surface*>(args[1].o), args[2].a);
        else if (opcode == 2)
            keyboardLeave(context.state, keyboard, args[0].u, reinterpret_cast<wl_surface*>(args[1].o));
        else if (opcode == 3)
            keyboardKey(context.state, keyboard, args[0].u, args[1].u, args[2].u, args[3].u);
    }
    return 0;
}

static void ignoreKeyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*)
{
}
static void destroyKeyboardOnRelease(void* data, wl_keyboard* keyboard, uint32_t, uint32_t, uint32_t, uint32_t keyState)
{
    if (keyState == WL_KEYBOARD_KEY_STATE_RELEASED)
    {
        ++*static_cast<int*>(data);
        wl_proxy_destroy(reinterpret_cast<wl_proxy*>(keyboard));
    }
}

static void locked(void* data, zwp_locked_pointer_v1*)
{
    ++static_cast<ListenerState*>(data)->locked;
}
static void unlocked(void* data, zwp_locked_pointer_v1*)
{
    ++static_cast<ListenerState*>(data)->unlocked;
}
static void relative(void* data, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t, wl_fixed_t, wl_fixed_t)
{
    auto* state = static_cast<ListenerState*>(data);
    ++state->relative;
    state->lastDx = dx;
}

static int dispatcher(const void* data, void*, uint32_t, const wl_message*, wl_argument*)
{
    ++*static_cast<int*>(const_cast<void*>(data));
    return 0;
}

int main()
{
    vkShade::initInputBlocker(true);
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    wl_display* display = wl_display_connect_to_fd(sockets[0]);
    if (!display)
        return 1;

    bool         ok           = true;
    auto*        displayProxy = reinterpret_cast<wl_proxy*>(display);
    auto*        surface      = reinterpret_cast<wl_surface*>(wl_proxy_create(displayProxy, &wl_surface_interface));
    auto*        viewporter   = reinterpret_cast<wp_viewporter*>(wl_proxy_create(displayProxy, &wp_viewporter_interface));
    wp_viewport* viewport     = wp_viewporter_get_viewport(viewporter, surface);
    wp_viewport_set_destination(viewport, 2560, 1440);
    wl_surface_commit(surface);
    auto geometry = vkShade::waylandSurfaceGeometryTracker().geometry(surface);
    auto position = vkShade::mapWaylandPointer(geometry, 3840, 2160, 1280, 720);
    ok &= check(std::abs(position.x - 1920) < 1e-9 && std::abs(position.y - 1080) < 1e-9,
                "Wayland viewport requests must align logical and framebuffer coordinates");
    wp_viewport_set_destination(viewport, 1920, 1080);
    auto* surfaceWrapper = reinterpret_cast<wl_surface*>(wl_proxy_create_wrapper(surface));
    wl_surface_commit(surfaceWrapper);
    geometry = vkShade::waylandSurfaceGeometryTracker().geometry(surface);
    position = vkShade::mapWaylandPointer(geometry, 3840, 2160, 960, 540);
    ok &= check(std::abs(position.x - 1920) < 1e-9 && std::abs(position.y - 1080) < 1e-9,
                "A wrapped surface commit must apply viewport changes to the original surface");
    wl_proxy_wrapper_destroy(surfaceWrapper);
    if (setenv("ENABLE_VKSHADE", "1", 1) != 0)
        return 1;
    void* waylandLibrary = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!waylandLibrary)
        return 1;
    using MarshalArray = wl_proxy* (*)(wl_proxy*, uint32_t, const wl_interface*, uint32_t, uint32_t, wl_argument*);
    auto marshalArray  = reinterpret_cast<MarshalArray>(dlsym(waylandLibrary, "wl_proxy_marshal_array_flags"));
    if (!marshalArray)
        return 1;
    wl_argument destination[2]{};
    destination[0].i = 1920;
    destination[1].i = 1080;
    marshalArray(reinterpret_cast<wl_proxy*>(viewport),
                 WP_VIEWPORT_SET_DESTINATION,
                 nullptr,
                 wl_proxy_get_version(reinterpret_cast<wl_proxy*>(viewport)),
                 0,
                 destination);
    wl_surface_commit(surface);
    geometry = vkShade::waylandSurfaceGeometryTracker().geometry(surface);
    position = vkShade::mapWaylandPointer(geometry, 3840, 2160, 960, 540);
    ok &= check(std::abs(position.x - 1920) < 1e-9 && std::abs(position.y - 1080) < 1e-9,
                "Dynamically resolved Wayland requests must retain viewport tracking");
    using CreateWrapper  = void* (*)(void*);
    using DestroyWrapper = void (*)(void*);
    auto createWrapper   = reinterpret_cast<CreateWrapper>(dlsym(waylandLibrary, "wl_proxy_create_wrapper"));
    auto destroyWrapper  = reinterpret_cast<DestroyWrapper>(dlsym(waylandLibrary, "wl_proxy_wrapper_destroy"));
    if (!createWrapper || !destroyWrapper)
        return 1;
    wp_viewport_set_destination(viewport, 2560, 1440);
    auto* dynamicWrapper = reinterpret_cast<wl_surface*>(createWrapper(surface));
    if (!dynamicWrapper)
        return 1;
    wl_surface_commit(dynamicWrapper);
    geometry = vkShade::waylandSurfaceGeometryTracker().geometry(surface);
    position = vkShade::mapWaylandPointer(geometry, 3840, 2160, 1280, 720);
    ok &= check(std::abs(position.x - 1920) < 1e-9 && std::abs(position.y - 1080) < 1e-9,
                "Dynamically resolved surface wrappers must apply viewport changes");
    destroyWrapper(dynamicWrapper);
    wp_viewport_destroy(viewport);
    wl_surface_commit(surface);
    geometry = vkShade::waylandSurfaceGeometryTracker().geometry(surface);
    ok &= check(geometry.destinationWidth == -1 && geometry.destinationHeight == -1, "Viewport destruction must clear scaling after commit");

    ListenerState state;
    auto*         lockedPointer = reinterpret_cast<zwp_locked_pointer_v1*>(wl_proxy_create(displayProxy, &zwp_locked_pointer_v1_interface));
    const zwp_locked_pointer_v1_listener lockedListener = {.locked = locked, .unlocked = unlocked};
    ok &= check(zwp_locked_pointer_v1_add_listener(lockedPointer, &lockedListener, &state) == 0, "Lock listener registration failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(lockedPointer), 0), "Locked event dispatch failed");
    ok &= check(vkShade::isWaylandGamePointerLocked() && state.locked == 1, "Pointer lock must be observed and forwarded");

    auto* relativePointer = reinterpret_cast<zwp_relative_pointer_v1*>(wl_proxy_create(displayProxy, &zwp_relative_pointer_v1_interface));
    const zwp_relative_pointer_v1_listener relativeListener = {.relative_motion = relative};
    ok &= check(zwp_relative_pointer_v1_add_listener(relativePointer, &relativeListener, &state) == 0, "Relative listener registration failed");
    const auto dx = static_cast<uint32_t>(wl_fixed_from_double(1.5));
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(relativePointer), 0, {0, 1, dx, 0, dx, 0}), "Relative event dispatch failed");
    ok &= check(state.relative == 1 && state.lastDx == wl_fixed_from_double(1.5), "Unblocked relative motion must reach the game");
    vkShade::setInputBlocked(true);
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(relativePointer), 0, {0, 2, dx, 0, dx, 0}),
                "Blocked relative event dispatch failed");
    ok &= check(state.relative == 1, "Blocked relative motion must not reach the game");
    vkShade::setInputBlocked(false);
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(lockedPointer), 1), "Unlocked event dispatch failed");
    ok &= check(!vkShade::isWaylandGamePointerLocked() && state.unlocked == 1, "Pointer unlock must be observed and forwarded");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(lockedPointer), 0), "Second locked event dispatch failed");
    zwp_locked_pointer_v1_destroy(lockedPointer);
    ok &= check(!vkShade::isWaylandGamePointerLocked(), "Destroying a locked pointer must clear its state");
    zwp_relative_pointer_v1_destroy(relativePointer);

    int   dispatchCount     = 0;
    auto* dispatcherPointer = reinterpret_cast<wl_proxy*>(wl_proxy_create(displayProxy, &zwp_locked_pointer_v1_interface));
    ok &= check(wl_proxy_add_dispatcher(dispatcherPointer, dispatcher, &dispatchCount, nullptr) == 0, "Lock dispatcher registration failed");
    ok &= check(event(sockets[1], display, dispatcherPointer, 0), "Dispatcher locked event failed");
    ok &= check(vkShade::isWaylandGamePointerLocked() && dispatchCount == 1, "Dispatcher lock must be observed and forwarded");
    wl_proxy_destroy(dispatcherPointer);
    ok &= check(!vkShade::isWaylandGamePointerLocked(), "Destroying a dispatcher proxy must clear its lock");

    using AddListener = int (*)(wl_proxy*, void (**)(void), void*);
    auto addListener  = reinterpret_cast<AddListener>(dlsym(waylandLibrary, "wl_proxy_add_listener"));
    if (!addListener)
        return 1;
    auto* dynamicPointer = reinterpret_cast<zwp_locked_pointer_v1*>(wl_proxy_create(displayProxy, &zwp_locked_pointer_v1_interface));
    ok &= check(addListener(reinterpret_cast<wl_proxy*>(dynamicPointer),
                            reinterpret_cast<void (**)(void)>(const_cast<zwp_locked_pointer_v1_listener*>(&lockedListener)),
                            &state)
                    == 0,
                "Dynamic lock listener registration failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(dynamicPointer), 0), "Dynamic locked event dispatch failed");
    ok &= check(vkShade::isWaylandGamePointerLocked(), "Dynamically resolved listeners must retain lock tracking");
    zwp_locked_pointer_v1_destroy(dynamicPointer);
    dlclose(waylandLibrary);

    FocusState                 focus;
    auto*                      keyboard         = reinterpret_cast<wl_keyboard*>(wl_proxy_create(displayProxy, &wl_keyboard_interface));
    auto*                      pointer          = reinterpret_cast<wl_pointer*>(wl_proxy_create(displayProxy, &wl_pointer_interface));
    const wl_keyboard_listener keyboardListener = {.enter = keyboardEnter, .leave = keyboardLeave, .key = keyboardKey};
    const wl_pointer_listener  pointerListener  = {
        .enter = pointerEnter, .leave = pointerLeave, .motion = pointerMotion, .button = pointerButton, .frame = pointerFrame};
    ok &= check(wl_keyboard_add_listener(keyboard, &keyboardListener, &focus) == 0, "Keyboard listener registration failed");
    ok &= check(wl_pointer_add_listener(pointer, &pointerListener, &focus) == 0, "Pointer listener registration failed");
    const uint32_t surfaceId = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(surface));
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(keyboard), 1, {100, surfaceId, 4, 25}), "Keyboard enter failed");
    ok &= check(focus.keyboardKeys == 1, "Unblocked keyboard focus must preserve held keys");
    ok &= check(event(sockets[1],
                      display,
                      reinterpret_cast<wl_proxy*>(pointer),
                      0,
                      {110, surfaceId, static_cast<uint32_t>(wl_fixed_from_int(20)), static_cast<uint32_t>(wl_fixed_from_int(30))}),
                "Pointer enter failed");
    ok &=
        check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(keyboard), 3, {101, 20, 17, WL_KEYBOARD_KEY_STATE_PRESSED}), "Key press failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(pointer), 3, {111, 20, 0x110, WL_POINTER_BUTTON_STATE_PRESSED}),
                "Button press failed");
    vkShade::setInputBlocked(true);
    ok &= check(focus.keyboardLeaves == 0 && focus.keyReleases == 2 && focus.buttonReleases == 1,
                "Opening the overlay must release held input without falsifying focus");
    const int pointerFramesBeforeFocusChange = focus.pointerFrames;
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(keyboard), 3, {102, 21, 42, WL_KEYBOARD_KEY_STATE_PRESSED}),
                "Blocked key press failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(pointer), 3, {112, 21, 0x111, WL_POINTER_BUTTON_STATE_PRESSED}),
                "Blocked button press failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(keyboard), 2, {102, surfaceId}), "Keyboard leave failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(pointer), 1, {112, surfaceId}), "Pointer leave failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(pointer), 5), "Pointer leave frame failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(keyboard), 1, {103, surfaceId, 4, 42}), "Blocked keyboard enter failed");
    ok &= check(event(sockets[1],
                      display,
                      reinterpret_cast<wl_proxy*>(pointer),
                      0,
                      {113, surfaceId, static_cast<uint32_t>(wl_fixed_from_int(40)), static_cast<uint32_t>(wl_fixed_from_int(50))}),
                "Blocked pointer enter failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(pointer), 5), "Pointer enter frame failed");
    ok &= check(event(sockets[1],
                      display,
                      reinterpret_cast<wl_proxy*>(pointer),
                      2,
                      {29, static_cast<uint32_t>(wl_fixed_from_int(55)), static_cast<uint32_t>(wl_fixed_from_int(65))}),
                "First blocked pointer motion failed");
    ok &= check(event(sockets[1],
                      display,
                      reinterpret_cast<wl_proxy*>(pointer),
                      2,
                      {30, static_cast<uint32_t>(wl_fixed_from_int(60)), static_cast<uint32_t>(wl_fixed_from_int(70))}),
                "Blocked pointer motion failed");
    ok &= check(focus.keyboardEnters == 2 && focus.keyboardSurface == surface && focus.keyboardSerial == 103 && focus.keyboardKeys == 0,
                "Keyboard focus must reach the game without overlay-held keys");
    ok &= check(focus.pointerEnters == 2 && focus.pointerSurface == surface && focus.pointerSerial == 113 && focus.pointerMotions == 0
                    && focus.pointerX == wl_fixed_from_int(40) && focus.pointerY == wl_fixed_from_int(50),
                "Pointer focus must reach the game while overlay motion stays blocked");
    ok &= check(focus.pointerFrames == pointerFramesBeforeFocusChange + 2, "Pointer focus frames must reach the game while input is blocked");
    vkShade::setInputBlocked(false);
    ok &= check(focus.keyboardEnters == 2 && focus.keyboardSurface == surface && focus.keyboardSerial == 103,
                "Closing the overlay must not duplicate keyboard focus");
    ok &= check(focus.pointerEnters == 2 && focus.pointerSurface == surface && focus.pointerSerial == 113 && focus.pointerMotions == 1
                    && focus.pointerX == wl_fixed_from_int(60) && focus.pointerY == wl_fixed_from_int(70),
                "Closing the overlay must restore the current pointer position without duplicate focus");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(keyboard), 3, {105, 31, 42, WL_KEYBOARD_KEY_STATE_RELEASED}),
                "Post-overlay key release failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(pointer), 3, {115, 31, 0x111, WL_POINTER_BUTTON_STATE_RELEASED}),
                "Post-overlay button release failed");
    ok &= check(focus.keyReleases == 2 && focus.buttonReleases == 1, "Input pressed inside the overlay must not leak releases to the game");

    vkShade::setInputBlocked(true);
    ok &= check(event(sockets[1],
                      display,
                      reinterpret_cast<wl_proxy*>(pointer),
                      2,
                      {32, static_cast<uint32_t>(wl_fixed_from_int(80)), static_cast<uint32_t>(wl_fixed_from_int(90))}),
                "Blocked pointer motion without a focus change failed");
    vkShade::setInputBlocked(false);
    ok &= check(focus.pointerEnters == 2 && focus.pointerMotions == 2 && focus.pointerX == wl_fixed_from_int(80)
                    && focus.pointerY == wl_fixed_from_int(90),
                "Closing the overlay must restore the pointer position even when focus never changed");

    vkShade::setInputBlocked(true);
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(keyboard), 2, {104, surfaceId}), "Second keyboard leave failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(pointer), 1, {114, surfaceId}), "Second pointer leave failed");
    vkShade::setInputBlocked(false);
    ok &= check(focus.keyboardEnters == 2 && focus.pointerEnters == 2, "Closing an unfocused overlay must not invent focus");
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(keyboard));
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(pointer));

    FocusState             dispatcherFocus;
    FocusDispatcherContext keyboardContext{&dispatcherFocus, false};
    FocusDispatcherContext pointerContext{&dispatcherFocus, true};
    auto*                  dispatcherKeyboard = wl_proxy_create(displayProxy, &wl_keyboard_interface);
    auto*                  dispatcherMouse    = wl_proxy_create(displayProxy, &wl_pointer_interface);
    ok &= check(wl_proxy_add_dispatcher(dispatcherKeyboard, focusDispatcher, &keyboardContext, nullptr) == 0,
                "Keyboard dispatcher registration failed");
    ok &= check(wl_proxy_add_dispatcher(dispatcherMouse, focusDispatcher, &pointerContext, nullptr) == 0, "Pointer dispatcher registration failed");
    ok &= check(event(sockets[1], display, dispatcherKeyboard, 1, {200, surfaceId, 0}), "Dispatcher keyboard enter failed");
    ok &= check(event(sockets[1],
                      display,
                      dispatcherMouse,
                      0,
                      {210, surfaceId, static_cast<uint32_t>(wl_fixed_from_int(20)), static_cast<uint32_t>(wl_fixed_from_int(30))}),
                "Dispatcher pointer enter failed");
    ok &= check(event(sockets[1], display, dispatcherKeyboard, 3, {201, 20, 17, WL_KEYBOARD_KEY_STATE_PRESSED}), "Dispatcher key press failed");
    ok &= check(event(sockets[1], display, dispatcherMouse, 3, {211, 20, 0x110, WL_POINTER_BUTTON_STATE_PRESSED}), "Dispatcher button press failed");
    vkShade::setInputBlocked(true);
    ok &= check(dispatcherFocus.keyboardLeaves == 0 && dispatcherFocus.keyReleases == 1 && dispatcherFocus.buttonReleases == 1,
                "Overlay opening must release dispatcher input without falsifying focus");
    const int dispatcherFramesBeforeFocusChange = dispatcherFocus.pointerFrames;
    ok &= check(event(sockets[1], display, dispatcherKeyboard, 2, {202, surfaceId}), "Dispatcher keyboard leave failed");
    ok &= check(event(sockets[1], display, dispatcherMouse, 1, {212, surfaceId}), "Dispatcher pointer leave failed");
    ok &= check(event(sockets[1], display, dispatcherMouse, 5), "Dispatcher pointer leave frame failed");
    ok &= check(event(sockets[1], display, dispatcherKeyboard, 1, {203, surfaceId, 4, 42}), "Blocked dispatcher keyboard enter failed");
    ok &= check(event(sockets[1],
                      display,
                      dispatcherMouse,
                      0,
                      {213, surfaceId, static_cast<uint32_t>(wl_fixed_from_int(40)), static_cast<uint32_t>(wl_fixed_from_int(50))}),
                "Blocked dispatcher pointer enter failed");
    ok &= check(event(sockets[1], display, dispatcherMouse, 5), "Dispatcher pointer enter frame failed");
    ok &= check(event(sockets[1],
                      display,
                      dispatcherMouse,
                      2,
                      {230, static_cast<uint32_t>(wl_fixed_from_int(60)), static_cast<uint32_t>(wl_fixed_from_int(70))}),
                "Blocked dispatcher pointer motion failed");
    ok &= check(dispatcherFocus.keyboardEnters == 2 && dispatcherFocus.keyboardSurface == surface && dispatcherFocus.keyboardSerial == 203
                    && dispatcherFocus.keyboardKeys == 0,
                "Dispatcher keyboard focus must reach the game without overlay-held keys");
    ok &= check(dispatcherFocus.pointerEnters == 2 && dispatcherFocus.pointerSurface == surface && dispatcherFocus.pointerSerial == 213
                    && dispatcherFocus.pointerMotions == 0,
                "Dispatcher pointer focus must reach the game while input is blocked");
    ok &= check(dispatcherFocus.pointerFrames == dispatcherFramesBeforeFocusChange + 2, "Dispatcher pointer focus frames must reach the game");
    vkShade::setInputBlocked(false);
    ok &= check(dispatcherFocus.keyboardEnters == 2 && dispatcherFocus.keyboardSurface == surface && dispatcherFocus.keyboardSerial == 203,
                "Closing the overlay must not duplicate dispatcher keyboard focus");
    ok &= check(dispatcherFocus.pointerEnters == 2 && dispatcherFocus.pointerSurface == surface && dispatcherFocus.pointerSerial == 213
                    && dispatcherFocus.pointerMotions == 1 && dispatcherFocus.pointerX == wl_fixed_from_int(60)
                    && dispatcherFocus.pointerY == wl_fixed_from_int(70),
                "Closing the overlay must restore dispatcher pointer position without duplicate focus");
    wl_proxy_destroy(dispatcherKeyboard);
    wl_proxy_destroy(dispatcherMouse);

    int                        destroyedReleases  = 0;
    auto*                      shortLivedKeyboard = reinterpret_cast<wl_keyboard*>(wl_proxy_create(displayProxy, &wl_keyboard_interface));
    const wl_keyboard_listener shortLivedListener = {.enter = ignoreKeyboardEnter, .key = destroyKeyboardOnRelease};
    ok &=
        check(wl_keyboard_add_listener(shortLivedKeyboard, &shortLivedListener, &destroyedReleases) == 0, "Short-lived keyboard registration failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(shortLivedKeyboard), 1, {300, surfaceId, 0}),
                "Short-lived keyboard enter failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(shortLivedKeyboard), 3, {301, 20, 17, WL_KEYBOARD_KEY_STATE_PRESSED}),
                "First short-lived key press failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(shortLivedKeyboard), 3, {302, 21, 18, WL_KEYBOARD_KEY_STATE_PRESSED}),
                "Second short-lived key press failed");
    vkShade::setInputBlocked(true);
    ok &= check(destroyedReleases == 1, "Destroying a keyboard during release must stop further callbacks");
    vkShade::setInputBlocked(false);

    FocusState destroyedSurfaceFocus;
    auto*      transientSurface  = reinterpret_cast<wl_surface*>(wl_proxy_create(displayProxy, &wl_surface_interface));
    auto*      transientKeyboard = reinterpret_cast<wl_keyboard*>(wl_proxy_create(displayProxy, &wl_keyboard_interface));
    ok &=
        check(wl_keyboard_add_listener(transientKeyboard, &keyboardListener, &destroyedSurfaceFocus) == 0, "Transient keyboard registration failed");
    const uint32_t transientSurfaceId = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(transientSurface));
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(transientKeyboard), 1, {400, transientSurfaceId, 0}),
                "Transient keyboard enter failed");
    vkShade::setInputBlocked(true);
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(transientKeyboard), 2, {401, transientSurfaceId}),
                "Transient keyboard leave failed");
    ok &= check(event(sockets[1], display, reinterpret_cast<wl_proxy*>(transientKeyboard), 1, {402, transientSurfaceId, 0}),
                "Transient blocked keyboard enter failed");
    wl_surface_destroy(transientSurface);
    vkShade::setInputBlocked(false);
    ok &= check(destroyedSurfaceFocus.keyboardEnters == 2, "Destroyed surfaces must not receive duplicate focus");
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(transientKeyboard));

    wl_surface_destroy(surface);
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(viewporter));
    wl_display_disconnect(display);
    close(sockets[1]);
    return ok ? 0 : 1;
}
