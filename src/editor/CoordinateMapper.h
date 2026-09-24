#pragma once

#include "document/Geometry.h"

namespace rpfg {

struct PageViewport {
    float zoom = 1.0f;          // Scale factor (1.0 = 100%)
    float scrollX = 0.0f;       // Horizontal scroll offset in viewport pixels
    float scrollY = 0.0f;       // Vertical scroll offset in viewport pixels
    float pageOffsetX = 0.0f;   // Horizontal page position within canvas/viewport
    float pageOffsetY = 0.0f;   // Vertical page position within canvas/viewport
    float pageWidth = 0.0f;     // Page width in document points (1/72")
    float pageHeight = 0.0f;    // Page height in document points (1/72")
    int rotationDegrees = 0;    // 0, 90, 180, 270
    float devicePixelRatio = 1.0f; // Windows DPI scaling factor (e.g. 1.25, 1.5, 2.0)
};

class CoordinateMapper {
public:
    // Maps a point from Viewport/Screen pixel space to Document/Page point space
    static Point viewportToPage(Point viewportPt, const PageViewport& vp);

    // Maps a point from Document/Page point space to Viewport/Screen pixel space
    static Point pageToViewport(Point pagePt, const PageViewport& vp);

    // Maps a rectangle from Document/Page space to Viewport/Screen pixel space
    static Rect pageToViewport(const Rect& pageRect, const PageViewport& vp);

    // Maps a rectangle from Viewport/Screen pixel space to Document/Page space
    static Rect viewportToPage(const Rect& viewportRect, const PageViewport& vp);
};

}  // namespace rpfg
