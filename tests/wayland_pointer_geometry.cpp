#include "wayland_pointer_geometry.hpp"

#include <wayland-client-protocol.h>

#include <cmath>
#include <iostream>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

static bool positionIs(const WaylandPointerPosition& actual, double x, double y, double width, double height, const char* message)
{
    constexpr double tolerance = 1e-9;
    const bool ok = std::abs(actual.x - x) < tolerance && std::abs(actual.y - y) < tolerance && std::abs(actual.logicalWidth - width) < tolerance
                    && std::abs(actual.logicalHeight - height) < tolerance;
    if (!ok)
        std::cerr << message << '\n';
    return ok;
}

int main()
{
    WaylandSurfaceGeometryTracker tracker;
    int                           surface      = 0;
    int                           otherSurface = 0;
    int                           viewport     = 0;
    bool                          ok           = true;

    auto position = [&](const void* target, uint32_t width, uint32_t height, double x, double y) {
        return mapWaylandPointer(tracker.geometry(target), width, height, x, y);
    };

    ok &= positionIs(position(&surface, 3840, 2160, 1280, 720), 1280, 720, 3840, 2160, "Default geometry should be unscaled");
    tracker.associateViewport(&viewport, &surface);
    tracker.setViewportDestination(&viewport, 2560, 1440);
    ok &= positionIs(position(&surface, 3840, 2160, 1280, 720), 1280, 720, 3840, 2160, "Viewport changes must wait for commit");
    tracker.commit(&surface);
    ok &= positionIs(position(&surface, 3840, 2160, 1280, 720), 1920, 1080, 2560, 1440, "Fractional scaling must align the cursor");
    const auto fractionalScale = waylandFramebufferScale(tracker.geometry(&surface), 3840, 2160);
    ok &= check(std::abs(fractionalScale.x - 1.5) < 1e-9 && std::abs(fractionalScale.y - 1.5) < 1e-9,
                "The overlay must use the surface's fractional framebuffer scale");
    const auto fractionalPointer = position(&surface, 3840, 2160, 1280, 720);
    ok &= check(std::abs(fractionalPointer.x / fractionalScale.x - 1280) < 1e-9 && std::abs(fractionalPointer.y / fractionalScale.y - 720) < 1e-9,
                "Logical overlay coordinates must preserve pointer alignment");
    ok &= positionIs(position(&otherSurface, 3840, 2160, 1280, 720), 1280, 720, 3840, 2160, "Surface geometry must remain independent");

    tracker.setBufferScale(&surface, 2);
    tracker.setViewportSource(&viewport, 10, 20, 100, 50);
    tracker.setViewportDestination(&viewport, 200, 100);
    tracker.commit(&surface);
    ok &= positionIs(position(&surface, 800, 600, 100, 50), 120, 90, 200, 100, "Source crop and buffer scale must both apply");
    tracker.destroyViewport(&viewport);
    ok &= positionIs(position(&surface, 800, 600, 100, 50), 120, 90, 200, 100, "Viewport destruction must wait for commit");
    tracker.commit(&surface);
    ok &= positionIs(position(&surface, 800, 600, 100, 50), 200, 100, 400, 300, "Destroying a viewport must restore buffer scaling");

    tracker.destroySurface(&surface);
    tracker.setBufferTransform(&surface, WL_OUTPUT_TRANSFORM_90);
    tracker.commit(&surface);
    ok &= positionIs(position(&surface, 600, 400, 100, 50), 50, 300, 400, 600, "Rotated buffers must map to buffer coordinates");
    tracker.setBufferTransform(&surface, WL_OUTPUT_TRANSFORM_FLIPPED_270);
    tracker.commit(&surface);
    ok &= positionIs(position(&surface, 600, 400, 100, 50), 550, 300, 400, 600, "Flipped rotated buffers must map to buffer coordinates");
    tracker.destroySurface(&surface);
    ok &= positionIs(position(&surface, 600, 400, 100, 50), 100, 50, 600, 400, "Destroyed surfaces must not retain geometry");

    WaylandSurfaceGeometry nonuniform;
    nonuniform.bufferScale       = 2;
    nonuniform.sourceWidth       = 100;
    nonuniform.sourceHeight      = 50;
    nonuniform.destinationWidth  = 100;
    nonuniform.destinationHeight = 200;
    auto scale                   = waylandFramebufferScale(nonuniform, 800, 600);
    ok &= check(scale.x == 2 && scale.y == 0.5, "Framebuffer scale must account for a cropped and stretched viewport");
    nonuniform.bufferTransform = WL_OUTPUT_TRANSFORM_90;
    scale                      = waylandFramebufferScale(nonuniform, 800, 600);
    ok &= check(scale.x == 0.5 && scale.y == 2, "Rotated surfaces must swap framebuffer scale axes");
    scale = waylandFramebufferScale({}, 0, 0);
    ok &= check(scale.x == 1 && scale.y == 1, "An empty framebuffer must have a finite fallback scale");

    int surfaceWrapper = 0, nestedWrapper = 0, viewportWrapper = 0;
    tracker.associateViewport(&viewport, &surface);
    tracker.associateProxyWrapper(&surfaceWrapper, &surface);
    tracker.associateProxyWrapper(&nestedWrapper, &surfaceWrapper);
    tracker.associateProxyWrapper(&viewportWrapper, &viewport);
    tracker.setViewportDestination(&viewportWrapper, 2560, 1440);
    tracker.commit(&nestedWrapper);
    ok &= positionIs(position(&surface, 3840, 2160, 1280, 720),
                     1920,
                     1080,
                     2560,
                     1440,
                     "Wrapped viewport changes and nested surface commits must affect the original surface");
    tracker.forgetProxyWrapper(&nestedWrapper);
    tracker.forgetProxyWrapper(&surfaceWrapper);
    tracker.associateProxyWrapper(&surfaceWrapper, &otherSurface);
    tracker.setBufferScale(&surfaceWrapper, 2);
    tracker.commit(&surfaceWrapper);
    ok &= positionIs(position(&otherSurface, 800, 600, 100, 50), 200, 100, 400, 300, "A reused wrapper address must refer to its new surface");
    ok &=
        positionIs(position(&surface, 3840, 2160, 1280, 720), 1920, 1080, 2560, 1440, "A reused wrapper address must not change its former surface");
    tracker.forgetProxyWrapper(&surfaceWrapper);
    tracker.forgetProxyWrapper(&viewportWrapper);
    tracker.destroySurface(&surface);
    tracker.destroySurface(&otherSurface);

    WaylandVirtualPointer pointer;
    pointer.setExtent(2560, 1440);
    pointer.setPosition(1280, 720);
    pointer.move(0.5, -0.25);
    ok &= check(std::abs(pointer.x() - 1280.5) < 1e-9 && std::abs(pointer.y() - 719.75) < 1e-9, "Relative motion must retain subpixel precision");
    pointer.move(5000, -5000);
    ok &= check(pointer.x() == 2559 && pointer.y() == 0, "Relative motion must remain within the surface");
    pointer.setPosition(100, 200);
    ok &= check(pointer.x() == 100 && pointer.y() == 200, "Absolute motion must replace the virtual cursor position");

    return ok ? 0 : 1;
}
