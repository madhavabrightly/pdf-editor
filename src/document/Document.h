#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "document/Page.h"
#include "document/TextStyle.h"

namespace rpfg {

struct DocumentFont {
    FontId id;
    std::string familyName;
    std::string postscriptName;
    bool isEmbedded = false;
    bool isBold = false;
    bool isItalic = false;
    std::vector<uint8_t> fontData;  // Raw TrueType / OpenType font bytes if embedded
};

class Document {
public:
    Document();

    const std::string& title() const { return title_; }
    void setTitle(std::string title) { title_ = std::move(title); }

    const std::string& author() const { return author_; }
    void setAuthor(std::string author) { author_ = std::move(author); }

    const std::string& sourcePath() const { return sourcePath_; }
    void setSourcePath(std::string path) { sourcePath_ = std::move(path); }

    std::size_t pageCount() const { return pages_.size(); }
    bool empty() const { return pages_.empty(); }

    const std::vector<std::shared_ptr<Page>>& pages() const { return pages_; }
    std::vector<std::shared_ptr<Page>>& pages() { return pages_; }

    std::shared_ptr<Page> page(std::size_t index) const;
    void addPage(std::shared_ptr<Page> p);
    void insertPage(std::size_t index, std::shared_ptr<Page> p);
    bool removePage(std::size_t index);

    // Font registry
    const std::unordered_map<FontId, DocumentFont>& fonts() const { return fonts_; }
    void registerFont(DocumentFont font);
    const DocumentFont* findFont(const FontId& id) const;

    // Change tracking
    bool isDirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void clearDirty() { dirty_ = false; }

    std::shared_ptr<Document> clone() const;

private:
    std::string title_;
    std::string author_;
    std::string sourcePath_;
    std::vector<std::shared_ptr<Page>> pages_;
    std::unordered_map<FontId, DocumentFont> fonts_;
    bool dirty_ = false;
};

}  // namespace rpfg
