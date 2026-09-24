#pragma once

#include <memory>
#include <vector>
#include "document/DocumentObject.h"

namespace rpfg {

class VectorObject : public DocumentObject {
public:
    VectorObject();
    explicit VectorObject(std::vector<PathCommand> commands);

    Rect bounds() const override;
    void setBounds(const Rect& bounds) override { cachedBounds_ = bounds; }

    const std::vector<PathCommand>& commands() const { return commands_; }
    void setCommands(std::vector<PathCommand> cmds);
    void appendCommand(PathCommand cmd);

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

    bool evenOdd() const { return evenOdd_; }
    void setEvenOdd(bool eo) { evenOdd_ = eo; }

    std::shared_ptr<DocumentObject> clone() const override;

private:
    std::vector<PathCommand> commands_;
    bool filled_ = false;
    bool stroked_ = true;
    Color fillColor_ = Color::black();
    Color strokeColor_ = Color::black();
    float strokeWidth_ = 1.0f;
    bool evenOdd_ = false;

    mutable Rect cachedBounds_;
    mutable bool boundsDirty_ = true;
    void recalculateBounds() const;
};

}  // namespace rpfg
