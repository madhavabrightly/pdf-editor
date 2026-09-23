// Verifies the native document model: a real page is parsed into characters ->
// words -> lines -> paragraphs, and the structure is correct (a heading and a
// two-line paragraph must come out as two paragraphs, with the right word and
// line counts). This is the foundation the editor's layout and caret work on.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "pdf/PdfEngine.h"

namespace {

std::mutex g_mutex;
std::condition_variable g_ready;
bool g_ready_flag = false;

void onText(std::shared_ptr<const rpfg::PageText> /*text*/) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_ready_flag = true;
    g_ready.notify_all();
}

bool analyse(const std::string& path, rpfg::PdfEngine& engine, int page) {
    engine.setPageTextCallback(onText);
    std::string error;
    if (!engine.openDocument(path, error).has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    engine.requestViewport({page}, 1.0f);

    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_ready.wait_for(lock, std::chrono::seconds(60), [] { return g_ready_flag; })) {
        std::fprintf(stderr, "FAIL: no text arrived\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rpfg_model_test <file.pdf>\n");
        return 2;
    }

    rpfg::PdfEngine engine;
    if (!analyse(argv[1], engine, 0)) {
        return 1;
    }

    const std::shared_ptr<const rpfg::PageModel> model = engine.pageModel(0);
    if (!model) {
        std::fprintf(stderr, "FAIL: page model was not built\n");
        return 1;
    }

    // The generator draws one heading and one smaller line, well apart.
    std::printf("glyphs=%zu paragraphs=%zu\n", model->glyphs.size(), model->paragraphs.size());
    for (std::size_t i = 0; i < model->paragraphs.size(); ++i) {
        const rpfg::Paragraph& paragraph = model->paragraphs[i];
        std::printf("  paragraph %zu: lines=%zu size=%.1f leading=%.1f text=\"%s\"\n", i,
                    paragraph.lines.size(), paragraph.size, paragraph.leading,
                    paragraph.text().c_str());
    }

    if (model->glyphs.empty()) {
        std::fprintf(stderr, "FAIL: no glyphs in the model\n");
        return 1;
    }
    if (model->paragraphs.size() != 2) {
        std::fprintf(stderr, "FAIL: expected 2 paragraphs (heading + line), got %zu\n",
                     model->paragraphs.size());
        return 1;
    }

    const rpfg::Paragraph& heading = model->paragraphs[0];
    if (heading.lines.size() != 1 || heading.lines.front().words.size() != 5) {
        std::fprintf(stderr, "FAIL: heading structure wrong (%zu lines, %zu words)\n",
                     heading.lines.size(), heading.lines.front().words.size());
        return 1;
    }
    if (heading.text() != "Rpfg test - page 1") {
        std::fprintf(stderr, "FAIL: heading text is \"%s\"\n", heading.text().c_str());
        return 1;
    }
    if (heading.size < 30.0f) {
        std::fprintf(stderr, "FAIL: heading size %.1f does not match the 36pt drawn\n",
                     heading.size);
        return 1;
    }

    const rpfg::Paragraph& body = model->paragraphs[1];
    if (body.text().find("rasterised by MuPDF") == std::string::npos) {
        std::fprintf(stderr, "FAIL: body text is \"%s\"\n", body.text().c_str());
        return 1;
    }

    // A structural unit test for the grouping itself, independent of any PDF.
    {
        std::vector<rpfg::Glyph> glyphs;
        auto add = [&glyphs](const std::string& text, float x, float y, float size) {
            rpfg::Glyph glyph;
            glyph.text = text;
            glyph.x = x;
            glyph.y = y;
            glyph.size = size;
            glyph.advance = size * 0.5f;
            glyphs.push_back(glyph);
        };
        // "Hello world" on two lines, both 12pt, 14pt leading.
        const std::string line1 = "Hello world";
        float x = 50.0f;
        for (const char ch : line1) {
            add(std::string(1, ch == ' ' ? ' ' : ch), x, 100.0f, 12.0f);
            x += ch == ' ' ? 6.0f : 7.0f;
        }
        x = 50.0f;
        for (const char ch : std::string("second line")) {
            add(std::string(1, ch), x, 114.0f, 12.0f);
            x += ch == ' ' ? 6.0f : 7.0f;
        }
        const rpfg::PageModel grouped = rpfg::buildPageModel(glyphs);
        if (grouped.paragraphs.size() != 1 || grouped.paragraphs[0].lines.size() != 2 ||
            grouped.paragraphs[0].lines[0].words.size() != 2) {
            std::fprintf(stderr, "FAIL: synthetic grouping wrong (%zu paragraphs)\n",
                         grouped.paragraphs.size());
            return 1;
        }
        std::printf("synthetic: \"%s\" (leading %.1f)\n", grouped.paragraphs[0].text().c_str(),
                    grouped.paragraphs[0].leading);
        if (grouped.paragraphs[0].lines[0].text() != "Hello world") {
            std::fprintf(stderr, "FAIL: synthetic line 1 text is \"%s\"\n",
                         grouped.paragraphs[0].lines[0].text().c_str());
            return 1;
        }
    }

    std::printf("PASS\n");
    return 0;
}
