#include "document/LineObject.h"

#include <algorithm>

namespace rpfg {

LineObject::LineObject()
    : DocumentObject(ObjectType::Line) {}

LineObject::LineObject(Point start, Point end, Color color, float width)
    : DocumentObject(ObjectType::Line),
      start_(start),
      end_(end),
      color_(color),
      strokeWidth_(width) {}

Rect LineObject::bounds() const {
    const float pad = strokeWidth_ * 0.5f;
    return Rect{
        std::min(start_.x, end_.x) - pad,
        std::min(start_.y, end_.y) - pad,
        std::max(start_.x, end_.x) + pad,
        std::max(start_.y, end_.y) + pad
    };
}

void LineObject::setBounds(const Rect& r) {
    start_ = Point{r.x0, r.y0};
    end_ = Point{r.x1, r.y1};
}

std::shared_ptr<DocumentObject> LineObject::clone() const {
    auto copy = std::make_shared<LineObject>(start_, end_, color_, strokeWidth_);
    copy->setId(id());
    copy->setZIndex(zIndex());
    copy->setVisible(isVisible());
    copy->setLocked(isLocked());
    copy->setTransform(transform());
    copy->setClipRect(clipRect());
    copy->setClipPath(clipPath());
    return copy;
}

}  // namespace rpfg
