// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Turn the source colours an imported drawing carries into document paints:
 * CMYK and spot inks instead of their sRGB preview.
 *
 * The reader annotates its page SVG with data-rp-fill / data-rp-stroke (see
 * the bridge's SCHEMA.md). This pass reads them, writes the file's own
 * numbers into the filling or stroking paint of the element and removes the
 * attributes, so the document is clean SVG again.
 *
 * No GTK and no Rust: the mapping is locked by a gtest that runs everywhere.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_INTERNAL_CDR_SOURCE_COLORS_H
#define INKSCAPE_EXTENSION_INTERNAL_CDR_SOURCE_COLORS_H

class SPDocument;

namespace Inkscape::Extension::Internal::CdrImport {

/// What apply_source_colors() did, per paint property.
struct SourceColorStats
{
    int cmyk = 0;      ///< written verbatim as an icc-color of the output profile
    int spot = 0;      ///< turned into a named spot swatch
    int flattened = 0; ///< a spot tint, flattened to its process stand-in
    int untouched = 0; ///< no CMYK output space, or a value that did not parse
};

/**
 * Apply the source colours of an imported drawing to its paints.
 *
 * With a CMYK output space, a CMYK paint becomes
 * `<element's sRGB hex> icc-color(<output profile>, c, m, y, k)` with the
 * file's numbers verbatim (they are the print intent, so nothing is
 * converted); a spot at full strength becomes a reference to a named spot
 * swatch whose alternate is that CMYK colour; a spot tint is flattened to its
 * tinted process stand-in. Without a CMYK output space the paints keep their
 * sRGB preview, as the library reader gives them.
 *
 * Both attributes are removed in every case. Elements inside defs are not
 * walked, and the child of a clone is not either.
 *
 * Runs without undo history and is idempotent: a second call changes nothing.
 */
SourceColorStats apply_source_colors(SPDocument *doc);

} // namespace Inkscape::Extension::Internal::CdrImport

#endif // INKSCAPE_EXTENSION_INTERNAL_CDR_SOURCE_COLORS_H

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
