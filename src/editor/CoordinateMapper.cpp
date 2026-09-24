#include "editor/CoordinateMapper.h"

#include <algorithm>
#include <cmath>

namespace rpfg {

Point CoordinateMapper::viewportToPage(Point viewportPt, const PageViewport& vp) {
    const float zoom = vp.zoom > 0.001f ? vp.zoom : 1.0f;

    // Remove viewport translation (scroll and page offset)
    const float rx = viewportPt.x - (vp.pageOffsetX - vp.scrollX);
    const float ry = viewportPt.y - (vp.pageOffsetY - vp.scrollY);

    // Unscale
    const float ux = rx / zoom;
    const float uy = ry / zoom;

    // Handle rotation
    const int rot = ((vp.rotationDegrees % 360) + 360) % 360;
    switch (rot) {
        case 90:
            return Point{uy, vp.pageHeight - ux};
        case 180:
            return Point{vp.pageWidth - ux, vp.pageHeight - uy};
        case 270:
            return Point{vp.pageWidth - uy, ux};
        case 0:
        default:
            return Point{ux, uy};
    }
}

Point CoordinateMapper::pageToViewport(Point pagePt, const PageViewport& vp) {
    const float zoom = vp.zoom > 0.001f ? vp.zoom : 1.0f;

    // Apply rotation
    float rx = pagePt.x;
    float ry = pagePt.y;
    const int rot = ((vp.rotationDegrees % 360) + 360) % 360;
    switch (rot) {
        case 90:
            rx = vp.pageHeight - pagePt.y;
            ry = pagePt.x;
            break;
        case 180:
            rx = vp.pageWidth - pagePt.x;
            ry = vp.pageHeight - pagePt.y;
            break;
        case 270:
            rx = pagePt.y;
            ry = vp.pageWidth - pagePt.x;
            break;
        case 0:
        default:
            break;
    }

    // Apply zoom and viewport translation
    const float vx = rx * zoom + (vp.pageOffsetX - vp.scrollX);
    const float vy = ry * zoom + (vp.pageOffsetY - vp.scrollY);

    return Point{vx, vy};
}

Rect CoordinateMapper::pageToViewport(const Rect& pageRect, const PageViewport& vp) {
    const Point p0 = pageToViewport(Point{pageRect.x0, pageRect.y0}, vp);
    const Point p1 = pageToViewport(Point{pageRect.x1, pageRect.y1}, vp);
    return Rect{
        std::min(p0.x, p1.x),
        std::min(p0.y, p1.y),
        std::max(p0.x, p1.x),
        std::max(p0.y, p1.y)
    };
}

Rect CoordinateMapper::viewportToPage(const Rect& viewportRect, const PageViewport& vp) {
    const Point p0 = viewportToPage(Point{viewportRect.x0, viewportRect.y0}, vp);
    const Point p1 = viewportToPage(Point{viewportRect.x1, viewportRect.y1}, vp);
    return Rect{
        std::min(p0.x, p1.x),
        std::min(p0.y, p1.y),
        std::max(p0.x, p1.x),
        std::max(p0.y, p1.y)
    };
}

}  // namespace rpfg
