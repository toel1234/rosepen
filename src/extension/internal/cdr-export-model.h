// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Rosepen drawing export model: an SPDocument as the
 * "rosepen-cdr-export/1" JSON that the TraceDraw bridge's writer consumes.
 *
 * Everything a drawing has that the format cannot carry becomes a named
 * warning instead of silent loss; the model is plain JSON text and this file
 * pulls in no GTK and no Rust.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_INTERNAL_CDR_EXPORT_MODEL_H
#define INKSCAPE_EXTENSION_INTERNAL_CDR_EXPORT_MODEL_H

#include <string>
#include <vector>

class SPDocument;

namespace Inkscape::Extension::Internal::CdrExport {

/// The JSON text and every fallback warning of one export.
struct Result
{
    std::string json;                  ///< the "rosepen-cdr-export/1" model
    std::vector<std::string> warnings; ///< one line per fallback, in order
};

/**
 * Build the export model of @a doc.
 *
 * Deterministic: two calls on the same document give byte-identical output
 * (pages in order, layers and items in z-order). The document is brought up
 * to date first.
 */
Result build_json(SPDocument *doc);

} // namespace Inkscape::Extension::Internal::CdrExport

#endif // INKSCAPE_EXTENSION_INTERNAL_CDR_EXPORT_MODEL_H

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
