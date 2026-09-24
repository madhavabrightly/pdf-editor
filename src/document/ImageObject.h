#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "document/DocumentObject.h"

namespace rpfg {

enum class ImageFormat {
    RGBA,
    RGB,
    JPEG,
    PNG
};

class ImageObject : public DocumentObject {
public:
    ImageObject();
    ImageObject(const Rect& bounds, int pixelWidth, int pixelHeight,
                ImageFormat format, std::vector<uint8_t> data);

    Rect bounds() const override { return bounds_; }
    void setBounds(const Rect& bounds) override { bounds_ = bounds; }

    int pixelWidth() const { return pixelWidth_; }
    int pixelHeight() const { return pixelHeight_; }
    ImageFormat format() const { return format_; }

    const std::vector<uint8_t>& data() const { return data_; }
    void setData(std::vector<uint8_t> data, int width, int height, ImageFormat fmt);

    std::shared_ptr<DocumentObject> clone() const override;

private:
    Rect bounds_;
    int pixelWidth_ = 0;
    int pixelHeight_ = 0;
    ImageFormat format_ = ImageFormat::RGBA;
    std::vector<uint8_t> data_;
};

}  // namespace rpfg
