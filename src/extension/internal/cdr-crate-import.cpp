// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Open a drawing through the TraceDraw bridge and hand its pages to the
 * document builder.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "cdr-crate-import.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <glib.h>

namespace Inkscape::Extension::Internal::CdrImport {

namespace {

/// The largest file the bridge will read into memory.
constexpr std::uintmax_t MAX_FILE_BYTES = 1024ull * 1024 * 1024;

/// What rvng_parse_pages() puts in front of a generated page too.
constexpr char const *SVG_HEADER =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";

/// A buffer the library returned; freed with the length it wrote.
struct RpBuf
{
    RpBuf() = default;
    RpBuf(RpBuf const &) = delete;
    RpBuf &operator=(RpBuf const &) = delete;
    ~RpBuf()
    {
        if (p) {
            rp_free(p, n);
        }
    }

    std::uint8_t *p = nullptr;
    std::size_t n = 0;
};

struct RpDocDeleter
{
    void operator()(RpCdrDoc *doc) const
    {
        if (doc) {
            rp_cdr_close(doc);
        }
    }
};

/// The file name without its directory and extension, used as the title.
std::string file_title(char const *uri)
{
    auto const stem = std::filesystem::path(uri).stem().string();
    return stem.empty() ? "Untitled" : stem;
}

} // namespace

CrateOutcome open_with_crate(char const *uri)
{
    CrateOutcome outcome;
    if (uri == nullptr || uri[0] == '\0') {
        outcome.code = RP_ERR_ARG;
        return outcome;
    }

    std::ifstream file(uri, std::ios::binary | std::ios::ate);
    if (!file) {
        outcome.code = RP_ERR_CONTAINER;
        return outcome;
    }
    auto const end = file.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > MAX_FILE_BYTES) {
        outcome.code = RP_ERR_ARG;
        return outcome;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!bytes.empty() &&
        !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        outcome.code = RP_ERR_CONTAINER;
        return outcome;
    }

    RpCdrDoc *handle = nullptr;
    outcome.code = rp_cdr_open(bytes.data(), bytes.size(), file_title(uri).c_str(), &handle);
    if (outcome.code != RP_OK || handle == nullptr) {
        return outcome;
    }
    std::unique_ptr<RpCdrDoc, RpDocDeleter> const doc(handle);

    if (rp_cdr_info(doc.get(), &outcome.info) != RP_OK) {
        outcome.code = RP_ERR_PANIC;
        return outcome;
    }

    outcome.pages.reserve(outcome.info.pages);
    for (std::size_t index = 0; index < outcome.info.pages; ++index) {
        RpBuf svg;
        auto const code = rp_cdr_page_svg(doc.get(), index, &svg.p, &svg.n);
        if (code != RP_OK || svg.p == nullptr) {
            outcome.code = code != RP_OK ? code : RP_ERR_PANIC;
            outcome.pages.clear();
            return outcome;
        }
        std::string page(SVG_HEADER);
        page.append(reinterpret_cast<char const *>(svg.p), svg.n);
        outcome.pages.push_back(std::move(page));
    }

    outcome.warnings.reserve(outcome.info.warnings);
    for (std::size_t index = 0; index < outcome.info.warnings; ++index) {
        RpBuf warning;
        if (rp_cdr_warning(doc.get(), index, &warning.p, &warning.n) != RP_OK || warning.p == nullptr) {
            break;
        }
        std::string text(reinterpret_cast<char const *>(warning.p), warning.n);
        g_info("cdr import: %s", text.c_str());
        outcome.warnings.push_back(std::move(text));
    }
    if (outcome.info.skipped_objects > 0) {
        g_message("cdr import: %u objects skipped", outcome.info.skipped_objects);
    }

    return outcome;
}

} // namespace Inkscape::Extension::Internal::CdrImport

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
