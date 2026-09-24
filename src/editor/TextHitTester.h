#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "document/Geometry.h"
#include "document/Page.h"
#include "document/TextFrame.h"

namespace rpfg {

struct DocumentPosition {
    int pageIndex = 0;
    std::string frameId;
    std::size_t paragraphIndex = 0;
    std::size_t runIndex = 0;
    std::size_t charOffset = 0;

    bool operator==(const DocumentPosition& other) const = default;
    bool isValid() const { return !frameId.empty(); }
};

struct CaretGeometry {
    Point position;     // Caret baseline position (x, baselineY)
    Point top;          // Top of the caret line
    Point bottom;       // Bottom of the caret line
    float height = 14.0f;
};

class TextHitTester {
public:
    // Finds the closest DocumentPosition for a point on the given page.
    static DocumentPosition hitTest(const Page& page, Point pagePt, int pageIndex = 0);

    // Computes the CaretGeometry for a given DocumentPosition.
    static CaretGeometry positionToGeometry(const Page& page, const DocumentPosition& pos);

    // Computes visual highlight bounding boxes for a selection range [start, end].
    static std::vector<Rect> selectionToRectangles(const Page& page,
                                                  const DocumentPosition& start,
                                                  const DocumentPosition& end);
};

}  // namespace rpfg
