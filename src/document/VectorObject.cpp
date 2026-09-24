#include "document/VectorObject.h"

#include <algorithm>

namespace rpfg {

VectorObject::VectorObject()
    : DocumentObject(ObjectType::Vector) {}

VectorObject::VectorObject(std::vector<PathCommand> commands)
    : DocumentObject(ObjectType::Vector), commands_(std::move(commands)) {}

void VectorObject::setCommands(std::vector<PathCommand> cmds) {
    commands_ = std::move(cmds);
    boundsDirty_ = true;
}

void VectorObject::appendCommand(PathCommand cmd) {
    commands_.push_back(std::move(cmd));
    boundsDirty_ = true;
}

void VectorObject::recalculateBounds() const {
    if (commands_.empty()) {
        cachedBounds_ = Rect{};
        boundsDirty_ = false;
        return;
    }

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    auto includePoint = [&](const Point& pt) {
        minX = std::min(minX, pt.x);
        minY = std::min(minY, pt.y);
        maxX = std::max(maxX, pt.x);
        maxY = std::max(maxY, pt.y);
    };

    for (const auto& cmd : commands_) {
        switch (cmd.verb) {
            case PathVerb::MoveTo:
            case PathVerb::LineTo:
                includePoint(cmd.p0);
                break;
            case PathVerb::CubicTo:
                includePoint(cmd.p0);
                includePoint(cmd.p1);
                includePoint(cmd.p2);
                break;
            case PathVerb::Close:
                break;
        }
    }

    if (minX <= maxX && minY <= maxY) {
        const float pad = stroked_ ? strokeWidth_ * 0.5f : 0.0f;
        cachedBounds_ = Rect{minX - pad, minY - pad, maxX + pad, maxY + pad};
    } else {
        cachedBounds_ = Rect{};
    }
    boundsDirty_ = false;
}

Rect VectorObject::bounds() const {
    if (boundsDirty_) {
        recalculateBounds();
    }
    return cachedBounds_;
}

std::shared_ptr<DocumentObject> VectorObject::clone() const {
    auto copy = std::make_shared<VectorObject>(commands_);
    copy->setId(id());
    copy->setZIndex(zIndex());
    copy->setVisible(isVisible());
    copy->setLocked(isLocked());
    copy->setTransform(transform());
    copy->setClipRect(clipRect());
    copy->setClipPath(clipPath());
    copy->filled_ = filled_;
    copy->stroked_ = stroked_;
    copy->fillColor_ = fillColor_;
    copy->strokeColor_ = strokeColor_;
    copy->strokeWidth_ = strokeWidth_;
    copy->evenOdd_ = evenOdd_;
    copy->cachedBounds_ = cachedBounds_;
    copy->boundsDirty_ = boundsDirty_;
    return copy;
}

}  // namespace rpfg
