#include "document/ShapeObject.h"

namespace rpfg {

ShapeObject::ShapeObject()
    : DocumentObject(ObjectType::Shape) {}

ShapeObject::ShapeObject(ShapeType shapeType, const Rect& bounds)
    : DocumentObject(ObjectType::Shape), shapeType_(shapeType), bounds_(bounds) {}

std::shared_ptr<DocumentObject> ShapeObject::clone() const {
    auto copy = std::make_shared<ShapeObject>(shapeType_, bounds_);
    copy->setId(id());
    copy->setZIndex(zIndex());
    copy->setVisible(isVisible());
    copy->setLocked(isLocked());
    copy->setTransform(transform());
    copy->setClipRect(clipRect());
    copy->setClipPath(clipPath());
    copy->cornerRadius_ = cornerRadius_;
    copy->filled_ = filled_;
    copy->stroked_ = stroked_;
    copy->fillColor_ = fillColor_;
    copy->strokeColor_ = strokeColor_;
    copy->strokeWidth_ = strokeWidth_;
    return copy;
}

}  // namespace rpfg
