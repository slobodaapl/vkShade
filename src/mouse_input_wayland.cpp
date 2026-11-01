#include "mouse_input_wayland.hpp"
#include "wayland_input_common.hpp"
#include "wayland_interpose.hpp"
#include "wayland_display.hpp"
#include "wayland_pointer_geometry.hpp"
#include "logger.hpp"
#include "relative-pointer-client-protocol.h"

#include <wayland-client.h>

#include <chrono>
#include <cstring>
#include <atomic>
#include <mutex>

namespace vkShade
{
    // Mouse-specific state (seat/queue come from wayland_input_common)
    static wl_pointer*              wlPointer       = nullptr;
    static zwp_relative_pointer_v1* relativePointer = nullptr;
    static std::atomic<bool>        overlayMouseActive{false};

    // Mouse state
    static std::mutex            mouseStateMutex;
    static WaylandVirtualPointer pointerPosition;
    static bool                  leftButton        = false;
    static bool                  rightButton       = false;
    static bool                  middleButton      = false;
    static float                 scrollAccumulator = 0.0f;

    // Per-frame flag: true when axis_discrete or axis_value120 fired for this
    // pointer frame, so we skip the continuous axis event to avoid double-counting.
    static bool discreteScrollReceived = false;

    // Time-based auto-release safety net. We intentionally avoid releasing
    // just because the cursor stopped moving while still inside the surface
    // (that breaks drag/resize holds). Fast release is only used after leave.
    // A long hard timeout remains as fallback for truly stuck states.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point leftPressTime{};
    static Clock::time_point rightPressTime{};
    static Clock::time_point middlePressTime{};
    static constexpr int     AUTO_RELEASE_AFTER_LEAVE_MS  = 250;
    static constexpr int     AUTO_RELEASE_HARD_TIMEOUT_MS = 30000;

    // Track whether motion occurred since last getMouseStateWayland() poll
    static bool   motionSinceLastPoll         = false;
    static bool   absoluteMotionSinceLastPoll = false;
    static double hiddenRelativeX             = 0;
    static double hiddenRelativeY             = 0;
    // Last time we saw motion — used to measure idle duration
    static Clock::time_point lastMotionTime{};
    // Track pointer focus so we only do aggressive release when pointer left.
    static bool pointerInsideSurface = false;

    static bool mouseInitialized = false;

    void setWaylandOverlayMouseActive(bool active)
    {
        overlayMouseActive.store(active, std::memory_order_release);
        if (!active)
        {
            std::lock_guard lock(mouseStateMutex);
            hiddenRelativeX = 0;
            hiddenRelativeY = 0;
        }
    }

    void resetWaylandMouseButtons()
    {
        std::lock_guard lock(mouseStateMutex);
        leftButton             = false;
        rightButton            = false;
        middleButton           = false;
        scrollAccumulator      = 0;
        discreteScrollReceived = false;
    }

    static void relativePointerMotion(void*, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
    {
        if (!overlayMouseActive.load(std::memory_order_acquire))
            return;
        const bool locked = isWaylandGamePointerLocked();
        if (!locked && !isWaylandGameCursorHidden())
            return;

        std::lock_guard lock(mouseStateMutex);
        if (!overlayMouseActive.load(std::memory_order_acquire))
            return;
        if (locked)
            pointerPosition.move(wl_fixed_to_double(dx), wl_fixed_to_double(dy));
        else
        {
            hiddenRelativeX += wl_fixed_to_double(dx);
            hiddenRelativeY += wl_fixed_to_double(dy);
        }
        motionSinceLastPoll = true;
    }

    static const zwp_relative_pointer_v1_listener relativePointerListener = {
        .relative_motion = relativePointerMotion,
    };

    // Pointer listener callbacks
    static void pointerEnter(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*serial*/, wl_surface* /*surface*/, wl_fixed_t sx, wl_fixed_t sy)
    {
        std::lock_guard lock(mouseStateMutex);
        pointerInsideSurface = true;
        pointerPosition.setPosition(wl_fixed_to_double(sx), wl_fixed_to_double(sy));
        absoluteMotionSinceLastPoll = true;
        lastMotionTime              = Clock::now();

        // Do NOT clear button state here. Surface reconfigurations (swapchain
        // resize) cause rapid leave/enter cycles while the user is dragging.
        // Clearing buttons on enter breaks ImGui drag operations. Button state
        // is tracked purely from wl_pointer.button events. If a compositor grab
        // (Alt+drag) consumes a release, the next user click naturally clears it.

        Logger::trace("Wayland: pointer enter at " + std::to_string(pointerPosition.x()) + "," + std::to_string(pointerPosition.y()));
    }

    static void pointerLeave(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*serial*/, wl_surface* /*surface*/)
    {
        std::lock_guard lock(mouseStateMutex);
        pointerInsideSurface = false;
        Logger::trace("Wayland: pointer leave");
    }

    static void pointerMotion(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*time*/, wl_fixed_t sx, wl_fixed_t sy)
    {
        std::lock_guard lock(mouseStateMutex);
        pointerPosition.setPosition(wl_fixed_to_double(sx), wl_fixed_to_double(sy));
        absoluteMotionSinceLastPoll = true;
        motionSinceLastPoll         = true;
    }

    static void pointerButton(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*serial*/, uint32_t /*time*/, uint32_t button, uint32_t state)
    {
        bool pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
        Logger::trace("Wayland: pointer button " + std::to_string(button) + " " + (pressed ? "pressed" : "released"));

        std::lock_guard lock(mouseStateMutex);
        // Linux evdev button codes: BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112
        auto now = Clock::now();
        switch (button)
        {
            case 0x110:
                leftButton = pressed;
                if (pressed)
                    leftPressTime = now;
                break;
            case 0x111:
                rightButton = pressed;
                if (pressed)
                    rightPressTime = now;
                break;
            case 0x112:
                middleButton = pressed;
                if (pressed)
                    middlePressTime = now;
                break;
        }
        if (pressed)
            lastMotionTime = now;
    }

    static void pointerAxis(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*time*/, uint32_t axis, wl_fixed_t value)
    {
        // Only use continuous axis as fallback when discrete events are not sent.
        // When both fire for the same pointer frame, discrete/value120 takes priority.
        if (axis != 0)
            return;
        std::lock_guard lock(mouseStateMutex);
        if (discreteScrollReceived)
            return;

        // Negative value = scroll up, positive = scroll down
        // Normalize: typical step is 10.0 fixed-point
        float scroll = wl_fixed_to_double(value);
        scrollAccumulator -= scroll / 10.0f;
    }

    static void pointerFrame(void* /*data*/, wl_pointer* /*pointer*/)
    {
        std::lock_guard lock(mouseStateMutex);
        // Reset per-frame discrete flag at the end of each pointer frame
        discreteScrollReceived = false;
    }

    static void pointerAxisSource(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*source*/)
    {
    }

    static void pointerAxisStop(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*time*/, uint32_t /*axis*/)
    {
    }

    static void pointerAxisDiscrete(void* /*data*/, wl_pointer* /*pointer*/, uint32_t axis, int32_t discrete)
    {
        // Discrete scroll events (wheel clicks) — preferred over continuous axis
        if (axis != 0)
            return;

        std::lock_guard lock(mouseStateMutex);
        discreteScrollReceived = true;
        scrollAccumulator -= (float) discrete; // Wayland: positive = scroll down, ImGui: positive = scroll up
    }

    static void pointerAxisValue120(void* /*data*/, wl_pointer* /*pointer*/, uint32_t axis, int32_t value120)
    {
        // High-resolution scroll (wl_pointer v8+). 120 units = one wheel click.
        // Preferred over both axis and axis_discrete when available.
        if (axis != 0)
            return;

        std::lock_guard lock(mouseStateMutex);
        discreteScrollReceived = true;
        scrollAccumulator -= (float) value120 / 120.0f;
    }

    static void pointerAxisRelativeDirection(void* /*data*/, wl_pointer* /*pointer*/, uint32_t /*axis*/, uint32_t /*direction*/)
    {
    }

    static const wl_pointer_listener pointerListener = {
        .enter                   = pointerEnter,
        .leave                   = pointerLeave,
        .motion                  = pointerMotion,
        .button                  = pointerButton,
        .axis                    = pointerAxis,
        .frame                   = pointerFrame,
        .axis_source             = pointerAxisSource,
        .axis_stop               = pointerAxisStop,
        .axis_discrete           = pointerAxisDiscrete,
        .axis_value120           = pointerAxisValue120,
        .axis_relative_direction = pointerAxisRelativeDirection,
    };

    // Called by shared seat listener when pointer capability is available
    static void bindPointer(wl_seat* seat)
    {
        if (wlPointer)
            return;

        wlPointer = wl_seat_get_pointer(seat);
        // Register as overlay proxy BEFORE add_listener so the interposition
        // layer passes this through without wrapping
        registerOverlayProxy(reinterpret_cast<wl_proxy*>(wlPointer));
        wl_pointer_add_listener(wlPointer, &pointerListener, nullptr);
        Logger::debug("Wayland: pointer bound from shared seat");
    }

    bool initWaylandMouse()
    {
        if (mouseInitialized)
            return wlPointer != nullptr;

        // Register our callback before initializing shared resources
        setPointerBindCallback(bindPointer);

        // Initialize shared seat/queue/registry (triggers seat capability callbacks)
        if (!initWaylandInputCommon())
            return false;

        if (wlPointer)
        {
            auto* manager = getWaylandRelativePointerManager();
            if (manager && !relativePointer)
            {
                relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(manager, wlPointer);
                registerOverlayProxy(reinterpret_cast<wl_proxy*>(relativePointer));
                zwp_relative_pointer_v1_add_listener(relativePointer, &relativePointerListener, nullptr);
            }
            mouseInitialized = true;
            Logger::info("Wayland mouse input initialized");
        }
        else
            Logger::warn("Wayland: no pointer found on seat");

        return wlPointer != nullptr;
    }

    void cleanupWaylandMouse()
    {
        if (relativePointer)
        {
            unregisterOverlayProxy(reinterpret_cast<wl_proxy*>(relativePointer));
            zwp_relative_pointer_v1_destroy(relativePointer);
            relativePointer = nullptr;
        }
        if (wlPointer)
        {
            unregisterOverlayProxy(reinterpret_cast<wl_proxy*>(wlPointer));
            wl_pointer_destroy(wlPointer);
            wlPointer = nullptr;
        }

        mouseInitialized = false;
        overlayMouseActive.store(false, std::memory_order_release);
        {
            std::lock_guard lock(mouseStateMutex);
            pointerPosition.setExtent(0, 0);
            hiddenRelativeX             = 0;
            hiddenRelativeY             = 0;
            absoluteMotionSinceLastPoll = false;
        }
    }

    void mirrorButtonState(uint32_t button, bool pressed)
    {
        std::lock_guard lock(mouseStateMutex);
        auto            now = Clock::now();
        switch (button)
        {
            case 0x110:
                leftButton = pressed;
                if (pressed)
                    leftPressTime = now;
                break;
            case 0x111:
                rightButton = pressed;
                if (pressed)
                    rightPressTime = now;
                break;
            case 0x112:
                middleButton = pressed;
                if (pressed)
                    middlePressTime = now;
                break;
        }
        if (pressed)
            lastMotionTime = now;
    }

    MouseState getMouseStateWayland(uint32_t framebufferWidth, uint32_t framebufferHeight)
    {
        MouseState state;

        const bool initialized  = initWaylandMouse();
        const auto geometry     = waylandSurfaceGeometryTracker().geometry(getWaylandSurface());
        const auto scale        = waylandFramebufferScale(geometry, framebufferWidth, framebufferHeight);
        state.framebufferScaleX = scale.x;
        state.framebufferScaleY = scale.y;
        if (!initialized)
            return state;
        {
            std::lock_guard lock(mouseStateMutex);
            const auto      bounds = mapWaylandPointer(geometry, framebufferWidth, framebufferHeight, pointerPosition.x(), pointerPosition.y());
            pointerPosition.setExtent(bounds.logicalWidth, bounds.logicalHeight);
        }

        dispatchWaylandInputEvents();

        std::lock_guard lock(mouseStateMutex);
        if (!absoluteMotionSinceLastPoll)
            pointerPosition.move(hiddenRelativeX, hiddenRelativeY);
        hiddenRelativeX             = 0;
        hiddenRelativeY             = 0;
        absoluteMotionSinceLastPoll = false;
        // Time-based auto-release for stuck buttons.
        // Do NOT synthesize release from motion idle while pointer remains
        // inside the surface (this breaks hold-to-drag). Release quickly only
        // after leave, with a long hard timeout as a fallback.
        auto now = Clock::now();
        if (motionSinceLastPoll)
        {
            lastMotionTime      = now;
            motionSinceLastPoll = false;
        }

        auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMotionTime).count();
        if (leftButton || rightButton || middleButton)
        {
            auto leftMs   = std::chrono::duration_cast<std::chrono::milliseconds>(now - leftPressTime).count();
            auto rightMs  = std::chrono::duration_cast<std::chrono::milliseconds>(now - rightPressTime).count();
            auto middleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - middlePressTime).count();

            const bool shouldReleaseAfterLeave = !pointerInsideSurface && idleMs > AUTO_RELEASE_AFTER_LEAVE_MS;
            const bool shouldReleaseHard       = idleMs > AUTO_RELEASE_HARD_TIMEOUT_MS;

            if (leftButton && (shouldReleaseAfterLeave || shouldReleaseHard) && leftMs > AUTO_RELEASE_AFTER_LEAVE_MS)
                leftButton = false;
            if (rightButton && (shouldReleaseAfterLeave || shouldReleaseHard) && rightMs > AUTO_RELEASE_AFTER_LEAVE_MS)
                rightButton = false;
            if (middleButton && (shouldReleaseAfterLeave || shouldReleaseHard) && middleMs > AUTO_RELEASE_AFTER_LEAVE_MS)
                middleButton = false;
        }

        const auto position = mapWaylandPointer(geometry, framebufferWidth, framebufferHeight, pointerPosition.x(), pointerPosition.y());
        state.x             = position.x;
        state.y             = position.y;
        state.leftButton    = leftButton;
        state.rightButton   = rightButton;
        state.middleButton  = middleButton;
        state.scrollDelta   = scrollAccumulator;
        scrollAccumulator   = 0.0f;

        return state;
    }
} // namespace vkShade
