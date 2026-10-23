// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Open a drawing through the TraceDraw bridge and hand its pages to the
 * document builder.
 *
 * Reads the file, calls the C ABI, copies every page's annotated SVG into a
 * string and collects the reader's warnings. No document, no GTK.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_INTERNAL_CDR_CRATE_IMPORT_H
#define INKSCAPE_EXTENSION_INTERNAL_CDR_CRATE_IMPORT_H

#include <string>
#include <vector>

#include "tracecdr-ffi.h"

namespace Inkscape::Extension::Internal::CdrImport {

/// What the bridge gave for one file.
struct CrateOutcome
{
    int code = RP_ERR_ARG;       ///< RP_OK, or the ABI error code
    RpCdrInfo info{};            ///< the reader's counters
    std::vector<std::string> pages;    ///< annotated SVG per page, XML header included
    std::vector<std::string> warnings; ///< the reader's warnings, in order
};

/**
 * Read @a uri through the bridge.
 *
 * Files over 1 GiB are refused without reading them. Reader warnings are
 * logged as information; skipped objects are summarised once. On any failure
 * @a pages is empty and @a code says why.
 */
CrateOutcome open_with_crate(char const *uri);

} // namespace Inkscape::Extension::Internal::CdrImport

#endif // INKSCAPE_EXTENSION_INTERNAL_CDR_CRATE_IMPORT_H

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
