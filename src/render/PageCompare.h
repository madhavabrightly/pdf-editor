// Renders whole PDF pages to grayscale and compares them. This is the safety
// net from the plan: the editable document is only accepted if rendering it
// back to a PDF stays visually close to the original page.

#pragma once

#include <string>
#include <vector>

namespace rpfg {

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

// Rasterises one page of `path` at `scale` into an 8-bit grayscale image.
bool renderPdfPageGray(const std::string& path, int pageIndex, float scale, GrayImage& out,
                       std::string& error);

// Number of pages in `path` (0 on failure).
int pdfPageCount(const std::string& path);

// Fraction of pixels whose luminance differs by more than `tolerance` (0..255).
// 0.0 means identical. Returns 1.0 if the images do not share a size.
double imageDifferenceFraction(const GrayImage& a, const GrayImage& b, int tolerance);

}  // namespace rpfg
