#include "document/ProtectedObject.h"

namespace rpfg {

ProtectedObject::ProtectedObject()
    : DocumentObject(ObjectType::Protected) {
    setLocked(true);
}

ProtectedObject::ProtectedObject(const Rect& bounds, std::string reason, int originalPdfObject)
    : DocumentObject(ObjectType::Protected),
      bounds_(bounds),
      reason_(std::move(reason)),
      originalPdfObject_(originalPdfObject) {
    setLocked(true);
}

void ProtectedObject::setRenderedPixels(std::vector<uint8_t> pixels, int width, int height) {
    renderedPixels_ = std::move(pixels);
    pixelWidth_ = width;
    pixelHeight_ = height;
}

std::shared_ptr<DocumentObject> ProtectedObject::clone() const {
    auto copy = std::make_shared<ProtectedObject>(bounds_, reason_, originalPdfObject_);
    copy->setId(id());
    copy->setZIndex(zIndex());
    copy->setVisible(isVisible());
    copy->setLocked(isLocked());
    copy->setTransform(transform());
    copy->setClipRect(clipRect());
    copy->setClipPath(clipPath());
    copy->renderedPixels_ = renderedPixels_;
    copy->pixelWidth_ = pixelWidth_;
    copy->pixelHeight_ = pixelHeight_;
    return copy;
}

}  // namespace rpfg
