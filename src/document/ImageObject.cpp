#include "document/ImageObject.h"

namespace rpfg {

ImageObject::ImageObject()
    : DocumentObject(ObjectType::Image) {}

ImageObject::ImageObject(const Rect& bounds, int pixelWidth, int pixelHeight,
                         ImageFormat format, std::vector<uint8_t> data)
    : DocumentObject(ObjectType::Image),
      bounds_(bounds),
      pixelWidth_(pixelWidth),
      pixelHeight_(pixelHeight),
      format_(format),
      data_(std::move(data)) {}

void ImageObject::setData(std::vector<uint8_t> data, int width, int height, ImageFormat fmt) {
    data_ = std::move(data);
    pixelWidth_ = width;
    pixelHeight_ = height;
    format_ = fmt;
}

std::shared_ptr<DocumentObject> ImageObject::clone() const {
    auto copy = std::make_shared<ImageObject>(bounds_, pixelWidth_, pixelHeight_, format_, data_);
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
