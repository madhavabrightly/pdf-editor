#pragma once

#include <string>
#include "document/Geometry.h"

namespace rpfg {

using FontId = std::string;

struct TextStyle {
    FontId font = "helv";
    FontId fontId;
    std::string fontName = "Helvetica";
    float fontSize = 12.0f;

    bool bold = false;
    bool italic = false;

    Color color = Color::black();

    float characterSpacing = 0.0f;
    float wordSpacing = 0.0f;
    float horizontalScale = 1.0f;
    float baselineShift = 0.0f;

    float lineHeight = 1.2f;  // Multiple of fontSize, or points if > 3.0f

    float paragraphSpacingBefore = 0.0f;
    float paragraphSpacingAfter = 0.0f;

    TextAlignment alignment = TextAlignment::Left;

    bool matchesTypography(const TextStyle& other) const {
        return font == other.font &&
               std::abs(fontSize - other.fontSize) < 0.1f &&
               bold == other.bold &&
               italic == other.italic &&
               color == other.color &&
               std::abs(characterSpacing - other.characterSpacing) < 0.01f &&
               std::abs(wordSpacing - other.wordSpacing) < 0.01f &&
               std::abs(horizontalScale - other.horizontalScale) < 0.01f;
    }
};

}  // namespace rpfg
