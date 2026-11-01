#include "wayland_pointer_geometry.hpp"

#include <wayland-client-protocol.h>
#include <algorithm>
#include <cmath>

namespace vkShade
{
    void WaylandVirtualPointer::setPosition(double x, double y)
    {
        positionX = x;
        positionY = y;
    }

    void WaylandVirtualPointer::setExtent(double widthValue, double heightValue)
    {
        width  = widthValue;
        height = heightValue;
    }

    void WaylandVirtualPointer::move(double dx, double dy)
    {
        positionX += dx;
        positionY += dy;
        if (width > 0 && height > 0)
        {
            positionX = std::clamp(positionX, 0.0, std::max(0.0, width - 1));
            positionY = std::clamp(positionY, 0.0, std::max(0.0, height - 1));
        }
    }

    WaylandPointerPosition mapWaylandPointer(const WaylandSurfaceGeometry& geometry, uint32_t bufferWidth, uint32_t bufferHeight, double x, double y)
    {
        const bool   rotated       = geometry.bufferTransform == WL_OUTPUT_TRANSFORM_90 || geometry.bufferTransform == WL_OUTPUT_TRANSFORM_270
                                     || geometry.bufferTransform == WL_OUTPUT_TRANSFORM_FLIPPED_90
                                     || geometry.bufferTransform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
        const double scale         = geometry.bufferScale;
        const double sourceWidth   = geometry.sourceWidth > 0 ? geometry.sourceWidth : (rotated ? bufferHeight : bufferWidth) / scale;
        const double sourceHeight  = geometry.sourceHeight > 0 ? geometry.sourceHeight : (rotated ? bufferWidth : bufferHeight) / scale;
        const double logicalWidth  = geometry.destinationWidth > 0 ? geometry.destinationWidth : sourceWidth;
        const double logicalHeight = geometry.destinationHeight > 0 ? geometry.destinationHeight : sourceHeight;
        const double sourceX       = geometry.sourceWidth > 0 ? geometry.sourceX : 0;
        const double sourceY       = geometry.sourceHeight > 0 ? geometry.sourceY : 0;
        const double tx            = (sourceX + x * sourceWidth / logicalWidth) * scale;
        const double ty            = (sourceY + y * sourceHeight / logicalHeight) * scale;

        switch (geometry.bufferTransform)
        {
            case WL_OUTPUT_TRANSFORM_90: return {ty, bufferHeight - tx, logicalWidth, logicalHeight};
            case WL_OUTPUT_TRANSFORM_180: return {bufferWidth - tx, bufferHeight - ty, logicalWidth, logicalHeight};
            case WL_OUTPUT_TRANSFORM_270: return {bufferWidth - ty, tx, logicalWidth, logicalHeight};
            case WL_OUTPUT_TRANSFORM_FLIPPED: return {bufferWidth - tx, ty, logicalWidth, logicalHeight};
            case WL_OUTPUT_TRANSFORM_FLIPPED_90: return {ty, tx, logicalWidth, logicalHeight};
            case WL_OUTPUT_TRANSFORM_FLIPPED_180: return {tx, bufferHeight - ty, logicalWidth, logicalHeight};
            case WL_OUTPUT_TRANSFORM_FLIPPED_270: return {bufferWidth - ty, bufferHeight - tx, logicalWidth, logicalHeight};
            default: return {tx, ty, logicalWidth, logicalHeight};
        }
    }

    WaylandFramebufferScale waylandFramebufferScale(const WaylandSurfaceGeometry& geometry, uint32_t bufferWidth, uint32_t bufferHeight)
    {
        const auto   origin = mapWaylandPointer(geometry, bufferWidth, bufferHeight, 0, 0);
        const auto   x      = mapWaylandPointer(geometry, bufferWidth, bufferHeight, 1, 0);
        const auto   y      = mapWaylandPointer(geometry, bufferWidth, bufferHeight, 0, 1);
        const double scaleX = std::abs(x.x - origin.x) + std::abs(y.x - origin.x);
        const double scaleY = std::abs(x.y - origin.y) + std::abs(y.y - origin.y);
        return {std::isfinite(scaleX) && scaleX > 0 ? scaleX : 1, std::isfinite(scaleY) && scaleY > 0 ? scaleY : 1};
    }

    const void* WaylandSurfaceGeometryTracker::canonicalProxyLocked(const void* proxy) const
    {
        auto it = proxyWrappers.find(proxy);
        return it == proxyWrappers.end() ? proxy : it->second;
    }

    void WaylandSurfaceGeometryTracker::forgetProxyAliasesLocked(const void* proxy)
    {
        for (auto it = proxyWrappers.begin(); it != proxyWrappers.end();)
        {
            if (it->first == proxy || it->second == proxy)
                it = proxyWrappers.erase(it);
            else
                ++it;
        }
    }

    void WaylandSurfaceGeometryTracker::associateProxyWrapper(const void* wrapper, const void* proxy)
    {
        std::lock_guard lock(mutex);
        proxyWrappers[wrapper] = canonicalProxyLocked(proxy);
    }

    void WaylandSurfaceGeometryTracker::forgetProxyWrapper(const void* wrapper)
    {
        std::lock_guard lock(mutex);
        proxyWrappers.erase(wrapper);
    }

    const void* WaylandSurfaceGeometryTracker::canonicalProxy(const void* proxy)
    {
        std::lock_guard lock(mutex);
        return canonicalProxyLocked(proxy);
    }

    void WaylandSurfaceGeometryTracker::associateViewport(const void* viewport, const void* surface)
    {
        std::lock_guard lock(mutex);
        viewports[canonicalProxyLocked(viewport)] = canonicalProxyLocked(surface);
    }

    void WaylandSurfaceGeometryTracker::setViewportSource(const void* viewport, double x, double y, double width, double height)
    {
        std::lock_guard lock(mutex);
        auto            viewportIt = viewports.find(canonicalProxyLocked(viewport));
        if (viewportIt == viewports.end())
            return;
        auto& pending        = surfaces[viewportIt->second].pending;
        pending.sourceX      = x;
        pending.sourceY      = y;
        pending.sourceWidth  = width;
        pending.sourceHeight = height;
    }

    void WaylandSurfaceGeometryTracker::setViewportDestination(const void* viewport, int32_t width, int32_t height)
    {
        std::lock_guard lock(mutex);
        auto            viewportIt = viewports.find(canonicalProxyLocked(viewport));
        if (viewportIt == viewports.end())
            return;
        auto& pending             = surfaces[viewportIt->second].pending;
        pending.destinationWidth  = width;
        pending.destinationHeight = height;
    }

    void WaylandSurfaceGeometryTracker::destroyViewport(const void* viewport)
    {
        std::lock_guard lock(mutex);
        viewport        = canonicalProxyLocked(viewport);
        auto viewportIt = viewports.find(viewport);
        if (viewportIt == viewports.end())
        {
            forgetProxyAliasesLocked(viewport);
            return;
        }
        auto& pending             = surfaces[viewportIt->second].pending;
        pending.sourceX           = 0;
        pending.sourceY           = 0;
        pending.sourceWidth       = -1;
        pending.sourceHeight      = -1;
        pending.destinationWidth  = -1;
        pending.destinationHeight = -1;
        viewports.erase(viewportIt);
        forgetProxyAliasesLocked(viewport);
    }

    void WaylandSurfaceGeometryTracker::forgetViewport(const void* viewport)
    {
        std::lock_guard lock(mutex);
        viewport = canonicalProxyLocked(viewport);
        viewports.erase(viewport);
        forgetProxyAliasesLocked(viewport);
    }

    void WaylandSurfaceGeometryTracker::setBufferScale(const void* surface, int32_t scale)
    {
        std::lock_guard lock(mutex);
        surfaces[canonicalProxyLocked(surface)].pending.bufferScale = scale;
    }

    void WaylandSurfaceGeometryTracker::setBufferTransform(const void* surface, int32_t transform)
    {
        std::lock_guard lock(mutex);
        surfaces[canonicalProxyLocked(surface)].pending.bufferTransform = transform;
    }

    void WaylandSurfaceGeometryTracker::commit(const void* surface)
    {
        std::lock_guard lock(mutex);
        auto            it = surfaces.find(canonicalProxyLocked(surface));
        if (it != surfaces.end())
            it->second.current = it->second.pending;
    }

    void WaylandSurfaceGeometryTracker::destroySurface(const void* surface)
    {
        std::lock_guard lock(mutex);
        surface = canonicalProxyLocked(surface);
        surfaces.erase(surface);
        for (auto it = viewports.begin(); it != viewports.end();)
        {
            if (it->second == surface)
                it = viewports.erase(it);
            else
                ++it;
        }
        forgetProxyAliasesLocked(surface);
    }

    WaylandSurfaceGeometry WaylandSurfaceGeometryTracker::geometry(const void* surface)
    {
        std::lock_guard lock(mutex);
        auto            it = surfaces.find(canonicalProxyLocked(surface));
        return it == surfaces.end() ? WaylandSurfaceGeometry{} : it->second.current;
    }

    WaylandSurfaceGeometryTracker& waylandSurfaceGeometryTracker()
    {
        static WaylandSurfaceGeometryTracker tracker;
        return tracker;
    }
} // namespace vkShade
