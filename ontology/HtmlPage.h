#ifndef SUPERTYPE_HTMLPAGE_H__
#define SUPERTYPE_HTMLPAGE_H__


#include "../core_defs.h"
#include "Ephemeral.h"
#include <fstream>
#include <filesystem>

class HtmlPage_ : virtual public ETCS::Entity
{
public:
    virtual ~HtmlPage_() = default;

    const ETCS::NBuffer& GetHtmlContent() const { return html_content_; }
    const ETCS::NBuffer& GetCssContent() const  { return css_content_; }
    const ETCS::NBuffer& GetJsContent() const   { return js_content_; }

    void SetHtmlFromFile(const ETCS::Buffer& filepath) { LoadFileIntoBuffer(filepath, html_content_, "HTML"); }
    void SetCssFromFile(const ETCS::Buffer& filepath)  { LoadFileIntoBuffer(filepath, css_content_, "CSS"); }
    void SetJsFromFile(const ETCS::Buffer& filepath)   { LoadFileIntoBuffer(filepath, js_content_, "JS"); }

    void SetHtmlRaw(const ETCS::NBuffer& overwrite) { html_content_ = overwrite; }
    void SetCssRaw(const ETCS::NBuffer& overwrite)  { css_content_ = overwrite; }
    void SetJsRaw(const ETCS::NBuffer& overwrite)   { js_content_ = overwrite; }

    // ResolvedAsset — a flattened, non-owning {data, length, mime} triple,
    // at FAMILY scope rather than inside any one implementor because it is
    // this family's answer shape. A server holding a bare HtmlPage_* builds
    // its whole HTTP response from this without knowing whether the source
    // was a disk tree, a synthesized buffer, or something a future page type
    // invents.
    //
    // Non-owning by design: data points into whatever the resolving page
    // already holds (a std::string's bytes, an NBuffer's buf), valid only as
    // long as that page is alive and unmodified. Callers build a response
    // from it immediately and must never store it, as the data can go stale.
    struct ResolvedAsset
    {
        bool        matched   = false;
        const char* data      = nullptr;
        size_t      length    = 0;
        std::string mime_type = "application/octet-stream";
    };

    // The routing surface, and the reason it belongs to the family rather
    // than to FileHtmlPage alone: NetworkProvider previously carried four
    // near-identical recv/parse/route/send consumers (TestPageOld, TestPage,
    // StartWebserver, ServeTree) differing ONLY here -- one hardcoded
    // string, one if/else over three fixed names, and one tree walk. With
    // resolution dispatched through the family, a single server
    // implementation serves every page type, and adding a page type requires
    // no change to any server at all.
    // Const because two connections resolve concurrently against the same
    // page instance with no lock anywhere on this path.
    virtual ResolvedAsset Resolve(const std::string& request_path) const = 0;
    virtual bool IsFileBacked() const = 0;
    
protected:
    ETCS::NBuffer html_content_;
    ETCS::NBuffer css_content_;
    ETCS::NBuffer js_content_;

    // Centralized secure loader
    void LoadFileIntoBuffer(const ETCS::Buffer& filepath, ETCS::NBuffer& target_buffer, const char* asset_type)
    {
        target_buffer.reset();
        if (!filepath) return;

        std::error_code ec;
        std::filesystem::path allowedRoot = std::filesystem::canonical(std::filesystem::current_path(), ec);
        if (ec) {
            ETCS_LOG("HtmlPage_", "Failed to determine canonical root directory.");
            return;
        }

        std::filesystem::path requestedPath(filepath.c_str());
        std::filesystem::path absoluteTarget = std::filesystem::absolute(allowedRoot / requestedPath);
        std::filesystem::path canonicalTarget = std::filesystem::canonical(absoluteTarget, ec);
        
        if (ec) {
            ETCS_LOG("HtmlPage_", asset_type << " file not found or access denied: " << filepath.c_str());
            return;
        }

        auto [root_it, target_it] = std::mismatch(allowedRoot.begin(), allowedRoot.end(), canonicalTarget.begin());
        if (root_it != allowedRoot.end()) {
            ETCS_LOG("HtmlPage_", "SECURITY VIOLATION: Path traversal blocked for: " << filepath.c_str());
            return;
        }

        std::ifstream file(canonicalTarget.string(), std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            ETCS_LOG("HtmlPage_", "Failed to open " << asset_type << " file: " << canonicalTarget.string());
            return;
        }

        std::streamsize size = file.tellg();
        if (size < 0) {
            ETCS_LOG("HtmlPage_", "Failed to determine file size: " << canonicalTarget.string());
            return;
        }

        if (static_cast<size_t>(size) > target_buffer.bufsize - 1) {
            ETCS_LOG("HtmlPage_", "File exceeds NBuffer limit: " << canonicalTarget.string() 
                     << " (" << size << " bytes > " << (target_buffer.bufsize - 1) << ")");
            return;
        }

        file.seekg(0, std::ios::beg);
        if (file.read(target_buffer.buf, size)) {
            target_buffer.written = static_cast<size_t>(size);
            target_buffer.buf[target_buffer.written] = '\0';
            ETCS_LOG("HtmlPage_", "Successfully loaded " << size << " bytes of " << asset_type);
        } else {
            ETCS_LOG("HtmlPage_", "Stream read failed for: " << canonicalTarget.string());
            target_buffer.reset();
        }
    }
};

#endif // SUPERTYPE_HTMLPAGE_H__
