#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "document/DocumentObject.h"

namespace rpfg {

class ProtectedObject : public DocumentObject {
public:
    ProtectedObject();
    ProtectedObject(const Rect& bounds, std::string reason, int originalPdfObject = 0);

    Rect bounds() const override { return bounds_; }
    void setBounds(const Rect& bounds) override { bounds_ = bounds; }

    const std::string& reason() const { return reason_; }
    void setReason(std::string r) { reason_ = std::move(r); }

    int originalPdfObject() const { return originalPdfObject_; }
    void setOriginalPdfObject(int objNum) { originalPdfObject_ = objNum; }

    // Optional cached raster pixmap or display list data to render verbatim
    const std::vector<uint8_t>& renderedPixels() const { return renderedPixels_; }
    void setRenderedPixels(std::vector<uint8_t> pixels, int width, int height);

    int pixelWidth() const { return pixelWidth_; }
    int pixelHeight() const { return pixelHeight_; }

    std::shared_ptr<DocumentObject> clone() const override;

private:
    Rect bounds_;
    std::string reason_;
    int originalPdfObject_ = 0;
    std::vector<uint8_t> renderedPixels_;
    int pixelWidth_ = 0;
    int pixelHeight_ = 0;
};

}  // namespace rpfg
