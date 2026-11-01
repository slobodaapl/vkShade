#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace vkShade
{
    struct WaylandSurfaceGeometry
    {
        int32_t bufferScale       = 1;
        int32_t bufferTransform   = 0;
        double  sourceX           = 0;
        double  sourceY           = 0;
        double  sourceWidth       = -1;
        double  sourceHeight      = -1;
        int32_t destinationWidth  = -1;
        int32_t destinationHeight = -1;
    };

    struct WaylandPointerPosition
    {
        double x;
        double y;
        double logicalWidth;
        double logicalHeight;
    };

    struct WaylandFramebufferScale
    {
        double x = 1;
        double y = 1;
    };

    WaylandPointerPosition mapWaylandPointer(const WaylandSurfaceGeometry& geometry, uint32_t bufferWidth, uint32_t bufferHeight, double x, double y);
    WaylandFramebufferScale waylandFramebufferScale(const WaylandSurfaceGeometry& geometry, uint32_t bufferWidth, uint32_t bufferHeight);

    class WaylandVirtualPointer
    {
    public:
        void   setPosition(double x, double y);
        void   setExtent(double width, double height);
        void   move(double dx, double dy);
        double x() const
        {
            return positionX;
        }
        double y() const
        {
            return positionY;
        }

    private:
        double positionX = 0;
        double positionY = 0;
        double width     = 0;
        double height    = 0;
    };

    class WaylandSurfaceGeometryTracker
    {
    public:
        void                   associateProxyWrapper(const void* wrapper, const void* proxy);
        void                   forgetProxyWrapper(const void* wrapper);
        const void*            canonicalProxy(const void* proxy);
        void                   associateViewport(const void* viewport, const void* surface);
        void                   setViewportSource(const void* viewport, double x, double y, double width, double height);
        void                   setViewportDestination(const void* viewport, int32_t width, int32_t height);
        void                   destroyViewport(const void* viewport);
        void                   forgetViewport(const void* viewport);
        void                   setBufferScale(const void* surface, int32_t scale);
        void                   setBufferTransform(const void* surface, int32_t transform);
        void                   commit(const void* surface);
        void                   destroySurface(const void* surface);
        WaylandSurfaceGeometry geometry(const void* surface);

    private:
        const void* canonicalProxyLocked(const void* proxy) const;
        void        forgetProxyAliasesLocked(const void* proxy);

        struct Surface
        {
            WaylandSurfaceGeometry pending;
            WaylandSurfaceGeometry current;
        };

        std::mutex                                   mutex;
        std::unordered_map<const void*, Surface>     surfaces;
        std::unordered_map<const void*, const void*> viewports;
        std::unordered_map<const void*, const void*> proxyWrappers;
    };

    WaylandSurfaceGeometryTracker& waylandSurfaceGeometryTracker();
} // namespace vkShade
