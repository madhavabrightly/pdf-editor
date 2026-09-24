#pragma once

#include <memory>
#include "document/DocumentObject.h"

namespace rpfg {

class LineObject : public DocumentObject {
public:
    LineObject();
    LineObject(Point start, Point end, Color color = Color::black(), float width = 1.0f);

    Rect bounds() const override;
    void setBounds(const Rect& bounds) override;

    const Point& startPoint() const { return start_; }
    void setStartPoint(const Point& pt) { start_ = pt; }

    const Point& endPoint() const { return end_; }
    void setEndPoint(const Point& pt) { end_ = pt; }

    const Color& color() const { return color_; }
    void setColor(Color c) { color_ = c; }

    float strokeWidth() const { return strokeWidth_; }
    void setStrokeWidth(float w) { strokeWidth_ = w; }

    std::shared_ptr<DocumentObject> clone() const override;

private:
    Point start_;
    Point end_;
    Color color_ = Color::black();
    float strokeWidth_ = 1.0f;
};

}  // namespace rpfg
