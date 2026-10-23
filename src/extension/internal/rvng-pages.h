// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Build one document from the per-page SVG strings of an RVNG drawing import.
 *
 * The drawing format generator produces one complete SVG document per page.
 * Opening such a file without a GUI should give one document with every page
 * as a page of its own, not just the first page.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_INTERNAL_RVNG_PAGES_H
#define INKSCAPE_EXTENSION_INTERNAL_RVNG_PAGES_H

#include <memory>
#include <span>
#include <string>
#include <vector>

class SPDocument;

namespace Inkscape::Extension::Internal::RvngPages {

/**
 * Build a single document from one SVG page string.
 *
 * A page without a viewBox (as emitted by older generator versions) is fixed
 * up to the 72 dpi shape the generator intends: width and height in points and
 * a viewBox matching them.
 *
 * @returns The document, or nullptr when the string cannot be parsed.
 */
std::unique_ptr<SPDocument> document_from_page(std::string const &svg);

struct Result
{
    std::unique_ptr<SPDocument> doc;
    std::vector<int> failed_pages; ///< 1-based page numbers that could not be read
};

/**
 * Build a single document from all page strings of a drawing.
 *
 * One page gives exactly the document of document_from_page() (no page element,
 * no wrapper layer), so a single page import is structurally unchanged. Two or
 * more pages give one page per input page, each with its own "Page N" layer;
 * pages are placed next to each other in the document's user units.
 *
 * A page that fails to parse after the first one becomes an empty page of the
 * previous page's size and is reported in Result::failed_pages. A failed first
 * page fails the whole import.
 */
Result document_from_pages(std::span<std::string const> pages);

} // namespace Inkscape::Extension::Internal::RvngPages

#endif // INKSCAPE_EXTENSION_INTERNAL_RVNG_PAGES_H

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
