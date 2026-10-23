// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Map the flat colours of an imported drawing into the document's output
 * (CMYK) profile.
 *
 * Attaching the profile and converting flat paints is document level work
 * without any user interface or undo history; it runs on File > Open only.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_INTERNAL_CDR_IMPORT_MAPPING_H
#define INKSCAPE_EXTENSION_INTERNAL_CDR_IMPORT_MAPPING_H

#include <optional>
#include <string>

#include "colors/spaces/enum.h"

class SPDocument;

namespace Inkscape::Extension::Internal::CdrImport {

/**
 * Attach the ICC profile at @a uri to @a doc and make it the output profile.
 *
 * Mirrors what InkscapeApplication::document_new does for a new document: an
 * empty or missing file is silently ignored, an unreadable file is reported as
 * a warning, and a successful attach is followed by setOutputProfile().
 *
 * @returns The profile name in the document, or nothing when it was not attached.
 */
std::optional<std::string> attach_output_profile(SPDocument *doc, std::string const &uri,
                                                 Colors::RenderingIntent intent);

/**
 * Convert every flat fill and stroke set on an object outside defs into the
 * document's output space.
 *
 * Colours already in the output space are left alone, so calling this twice
 * changes nothing. Opacity properties, gradient stops, pattern, marker and
 * filter colours stay as they are.
 *
 * @returns The number of converted paint properties, or 0 without an output space.
 */
int convert_flat_paints(SPDocument *doc);

struct Mapping
{
    std::optional<std::string> profile;
    int converted = 0;
};

/// Attach the profile at @a uri, then convert the flat paints if it was attached.
Mapping map_to_output_profile(SPDocument *doc, std::string const &uri, Colors::RenderingIntent intent);

/// Like map_to_output_profile() with the preferred output profile of the preferences.
Mapping map_to_preferred_output_profile(SPDocument *doc);

} // namespace Inkscape::Extension::Internal::CdrImport

#endif // INKSCAPE_EXTENSION_INTERNAL_CDR_IMPORT_MAPPING_H

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
