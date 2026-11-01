#include "mouse_input_wayland.hpp"
#include "keyboard_input_wayland.hpp"
#include "wayland_input_common.hpp"
#include "wayland_pointer_geometry.hpp"

#include "relative-pointer-server-protocol.h"

#include <wayland-client.h>
#include <wayland-server.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static wl_display*       clientDisplay = nullptr;
static wl_surface*       clientSurface = nullptr;
static std::atomic<bool> pointerLocked{false};
static std::atomic<bool> cursorHidden{false};
static bool              queueWarning = false;

namespace vkShade
{
    wl_display* getWaylandDisplay()
    {
        return clientDisplay;
    }
    wl_surface* getWaylandSurface()
    {
        return clientSurface;
    }
    bool isWaylandGamePointerLocked()
    {
        return pointerLocked.load();
    }
    bool isWaylandGameCursorHidden()
    {
        return cursorHidden.load();
    }
    void registerOverlayProxy(wl_proxy*)
    {
    }
    void unregisterOverlayProxy(wl_proxy*)
    {
    }
} // namespace vkShade

static void clientLog(const char* format, va_list args)
{
    char message[1024];
    vsnprintf(message, sizeof(message), format, args);
    if (std::strstr(message, "destroyed while proxies still attached"))
        queueWarning = true;
}

struct Server
{
    wl_display*  display          = nullptr;
    wl_resource* surface          = nullptr;
    wl_resource* pointer          = nullptr;
    wl_resource* keyboard         = nullptr;
    wl_resource* relative         = nullptr;
    int          commands         = -1;
    int          acknowledgements = -1;
};

static void destroyResource(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void setCursor(wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t)
{
}
static const struct wl_pointer_interface pointerImplementation = {
    .set_cursor = setCursor,
    .release    = destroyResource,
};

static const struct wl_keyboard_interface keyboardImplementation = {.release = destroyResource};

static void getPointer(wl_client* client, wl_resource* seat, uint32_t id)
{
    auto* server    = static_cast<Server*>(wl_resource_get_user_data(seat));
    server->pointer = wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(seat), id);
    wl_resource_set_implementation(server->pointer, &pointerImplementation, server, nullptr);
}

static void getKeyboard(wl_client* client, wl_resource* seat, uint32_t id)
{
    auto* server     = static_cast<Server*>(wl_resource_get_user_data(seat));
    server->keyboard = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(seat), id);
    wl_resource_set_implementation(server->keyboard, &keyboardImplementation, server, nullptr);
}

static const struct wl_seat_interface seatImplementation = {
    .get_pointer  = getPointer,
    .get_keyboard = getKeyboard,
    .get_touch    = nullptr,
    .release      = destroyResource,
};

static void bindSeat(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    wl_resource* seat = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(seat, &seatImplementation, data, nullptr);
    wl_seat_send_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

static const struct zwp_relative_pointer_v1_interface relativeImplementation = {.destroy = destroyResource};

static void getRelativePointer(wl_client* client, wl_resource* manager, uint32_t id, wl_resource*)
{
    auto* server     = static_cast<Server*>(wl_resource_get_user_data(manager));
    server->relative = wl_resource_create(client, &zwp_relative_pointer_v1_interface, 1, id);
    wl_resource_set_implementation(server->relative, &relativeImplementation, server, nullptr);
}

static const struct zwp_relative_pointer_manager_v1_interface managerImplementation = {
    .destroy              = destroyResource,
    .get_relative_pointer = getRelativePointer,
};

static void bindManager(wl_client* client, void* data, uint32_t, uint32_t id)
{
    wl_resource* manager = wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, 1, id);
    wl_resource_set_implementation(manager, &managerImplementation, data, nullptr);
}

static int handleCommand(int fd, uint32_t, void* data)
{
    auto* server  = static_cast<Server*>(data);
    char  command = 0;
    if (read(fd, &command, 1) != 1)
        return 0;
    if (command == 'e' && server->pointer)
        wl_pointer_send_enter(server->pointer, 1, server->surface, wl_fixed_from_double(40), wl_fixed_from_double(30));
    else if (command == 'm' && server->pointer)
        wl_pointer_send_motion(server->pointer, 2, wl_fixed_from_double(20), wl_fixed_from_double(10));
    else if (command == 'r' && server->relative)
        zwp_relative_pointer_v1_send_relative_motion(
            server->relative, 0, 3, wl_fixed_from_double(10.5), wl_fixed_from_double(-4.25), wl_fixed_from_double(10.5), wl_fixed_from_double(-4.25));
    else if (command == 'b' && server->pointer)
        wl_pointer_send_button(server->pointer, 4, 4, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    else if (command == 'u' && server->pointer)
        wl_pointer_send_button(server->pointer, 5, 5, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    else if (command == 'p' && server->pointer)
        wl_pointer_send_leave(server->pointer, 6, server->surface);
    else if (command == 'k' && server->keyboard)
    {
        wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(server->keyboard, 6, server->surface, &keys);
        wl_array_release(&keys);
    }
    else if (command == 'l' && server->keyboard)
        wl_keyboard_send_leave(server->keyboard, 7, server->surface);
    else if (command == 'q')
        wl_display_terminate(server->display);
    wl_display_flush_clients(server->display);
    return write(server->acknowledgements, &command, 1) == 1 ? 0 : -1;
}

static bool sendCommand(int commands, int acknowledgements, char command)
{
    char acknowledged = 0;
    return write(commands, &command, 1) == 1 && read(acknowledgements, &acknowledged, 1) == 1 && acknowledged == command;
}

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    int sockets[2];
    int commands[2];
    int acknowledgements[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 || pipe(commands) != 0 || pipe(acknowledgements) != 0)
        return 1;

    Server server;
    server.display          = wl_display_create();
    server.commands         = commands[0];
    server.acknowledgements = acknowledgements[1];
    if (!server.display)
        return 1;
    wl_client* serverClient = wl_client_create(server.display, sockets[0]);
    clientDisplay           = wl_display_connect_to_fd(sockets[1]);
    if (!serverClient || !clientDisplay)
        return 1;

    clientSurface  = reinterpret_cast<wl_surface*>(wl_proxy_create(reinterpret_cast<wl_proxy*>(clientDisplay), &wl_surface_interface));
    server.surface = wl_resource_create(serverClient, &wl_surface_interface, 1, wl_proxy_get_id(reinterpret_cast<wl_proxy*>(clientSurface)));
    wl_global_create(server.display, &wl_seat_interface, 5, &server, bindSeat);
    wl_global_create(server.display, &zwp_relative_pointer_manager_v1_interface, 1, &server, bindManager);
    wl_event_loop_add_fd(wl_display_get_event_loop(server.display), server.commands, WL_EVENT_READABLE, handleCommand, &server);
    wl_log_set_handler_client(clientLog);
    std::thread serverThread([&server] { wl_display_run(server.display); });

    bool ok = check(vkShade::initWaylandMouse(), "Wayland mouse initialization failed");
    ok &= check(vkShade::initWaylandKeyboard(), "Wayland keyboard initialization failed");
    ok &= check(wl_display_roundtrip(clientDisplay) >= 0, "Wayland pointer setup failed");
    auto poll = [] {
        vkShade::beginWaylandInputFrame();
        return vkShade::getMouseStateWayland(400, 200);
    };

    ok &= check(sendCommand(commands[1], acknowledgements[0], 'e'), "Pointer enter could not be sent");
    auto mouse = poll();
    ok &= check(mouse.x == 40 && mouse.y == 30, "Absolute Wayland motion must reach the overlay");
    vkShade::setWaylandOverlayMouseActive(true);
    pointerLocked.store(true);
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'r'), "Relative motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 50.5 && mouse.y == 25.75, "Locked pointer motion must move the overlay cursor");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'b'), "Pointer press could not be sent");
    ok &= check(poll().leftButton, "Pointer buttons must work while locked");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'u'), "Pointer release could not be sent");
    ok &= check(!poll().leftButton, "Pointer releases must work while locked");

    vkShade::setWaylandOverlayMouseActive(false);
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'r'), "Hidden relative motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 50.5 && mouse.y == 25.75, "Relative motion must not move a hidden overlay");
    vkShade::setWaylandOverlayMouseActive(true);
    pointerLocked.store(false);
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'r'), "Unlocked relative motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 50.5 && mouse.y == 25.75, "Unlocked relative events must not double-count absolute motion");
    cursorHidden.store(true);
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'r'), "Hidden-cursor relative motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 61 && mouse.y == 21.5, "Relative motion must move the overlay when the game hides its cursor without a pointer lock");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'r') && sendCommand(commands[1], acknowledgements[0], 'm'),
                "Mixed relative and absolute motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 20 && mouse.y == 10, "Absolute motion must take precedence over relative motion in the same input batch");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'm') && sendCommand(commands[1], acknowledgements[0], 'r'),
                "Reversed mixed relative and absolute motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 20 && mouse.y == 10, "Absolute motion must take precedence regardless of event order");
    cursorHidden.store(false);
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'm'), "Absolute motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 20 && mouse.y == 10, "Absolute motion must resume after unlocking");

    ok &= check(sendCommand(commands[1], acknowledgements[0], 'k'), "Keyboard focus enter could not be sent");
    poll();
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'b'), "Focus test pointer press could not be sent");
    ok &= check(poll().leftButton, "Focus test pointer press was not received");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'p'), "Pointer focus leave could not be sent");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'l'), "Keyboard focus leave could not be sent");
    ok &= check(!poll().leftButton, "Losing keyboard focus must release overlay buttons");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'k'), "Keyboard focus re-entry could not be sent");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'e'), "Pointer focus re-entry could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 40 && mouse.y == 30, "Pointer position must update after focus returns");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'b'), "Pointer press after focus return could not be sent");
    ok &= check(poll().leftButton, "Overlay clicks must work after focus returns");
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'u'), "Pointer release after focus return could not be sent");
    ok &= check(!poll().leftButton, "Overlay pointer releases must work after focus returns");

    int   viewport = 0;
    auto& geometry = vkShade::waylandSurfaceGeometryTracker();
    geometry.associateViewport(&viewport, clientSurface);
    geometry.setViewportDestination(&viewport, 200, 100);
    geometry.commit(clientSurface);
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'm'), "Scaled pointer motion could not be sent");
    mouse = poll();
    ok &= check(mouse.x == 40 && mouse.y == 20 && mouse.framebufferScaleX == 2 && mouse.framebufferScaleY == 2,
                "Pointer position and overlay scale must describe the same Wayland surface geometry");
    geometry.destroySurface(clientSurface);

    vkShade::cleanupWaylandKeyboard();
    vkShade::cleanupWaylandMouse();
    vkShade::cleanupWaylandInputCommon();
    ok &= check(!queueWarning, "Wayland input cleanup must release proxies before the event queue");
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(clientSurface));
    wl_display_disconnect(clientDisplay);
    ok &= check(sendCommand(commands[1], acknowledgements[0], 'q'), "Wayland test server did not stop");
    serverThread.join();
    wl_display_destroy(server.display);
    close(commands[0]);
    close(commands[1]);
    close(acknowledgements[0]);
    close(acknowledgements[1]);
    return ok ? 0 : 1;
}
