#pragma once

#include <memory>
#include "document/DocumentObject.h"

namespace rpfg {

enum class ShapeType {
    Rectangle,
    RoundedRectangle,
    Ellipse
};

class ShapeObject : public DocumentObject {
public:
    ShapeObject();
    ShapeObject(ShapeType shapeType, const Rect& bounds);

    Rect bounds() const override { return bounds_; }
    void setBounds(const Rect& bounds) override { bounds_ = bounds; }

    ShapeType shapeType() const { return shapeType_; }
    void setShapeType(ShapeType t) { shapeType_ = t; }

    float cornerRadius() const { return cornerRadius_; }
    void setCornerRadius(float r) { cornerRadius_ = r; }

    bool isFilled() const { return filled_; }
    void setFilled(bool f) { filled_ = f; }

    bool isStroked() const { return stroked_; }
    void setStroked(bool s) { stroked_ = s; }

    const Color& fillColor() const { return fillColor_; }
    void setFillColor(Color c) { fillColor_ = c; }

    const Color& strokeColor() const { return strokeColor_; }
    void setStrokeColor(Color c) { strokeColor_ = c; }

    float strokeWidth() const { return strokeWidth_; }
    void setStrokeWidth(float w) { strokeWidth_ = w; }

    std::shared_ptr<DocumentObject> clone() const override;

private:
    ShapeType shapeType_ = ShapeType::Rectangle;
    Rect bounds_;
    float cornerRadius_ = 0.0f;
    bool filled_ = true;
    bool stroked_ = false;
    Color fillColor_ = Color::black();
    Color strokeColor_ = Color::black();
    float strokeWidth_ = 1.0f;
};

}  // namespace rpfg
