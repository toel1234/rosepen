// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure PDF/X helpers: mode, page boxes, crop marks, XMP and preflight.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef EXTENSION_INTERNAL_PDFOUTPUT_PDFX_H
#define EXTENSION_INTERNAL_PDFOUTPUT_PDFX_H

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <2geom/point.h>
#include <2geom/rect.h>

class SPDocument;

namespace Inkscape::Extension::Internal::PdfX {

/**
 * The PDF/X flavour selected by the exporter. `None` is the unchanged default
 * PDF path; every PDF/X branch in the exporter is gated on it.
 */
enum class Mode {
    None,
    X1a,
    X4,
};

/** Map the `PDFversion` extension option to a mode; anything else is None. */
Mode mode_from_param(std::string_view value);

/**
 * Media/Bleed/Trim rectangles for one page, in document px.
 *
 * Trim is the page rect, Bleed is the page rect grown by `inkscape:bleed`,
 * and Media is the bleed rect or, with crop marks, the page rect grown by
 * max(bleed side, 3 mm) + 5 mm on each side.
 */
struct Boxes
{
    Geom::Rect media;
    Geom::Rect bleed;
    Geom::Rect trim;
};

Boxes page_boxes(Geom::Rect const &trim, Geom::Rect const &bleed, bool marks);

/**
 * Eight crop-mark segments for one page, in document px: two per trim corner,
 * collinear with the trim edges, starting at max(bleed side, 3 mm) from the
 * trim edge and 5 mm long. Callers map them through the page transform.
 */
std::vector<std::pair<Geom::Point, Geom::Point>> crop_mark_segments(Geom::Rect const &trim,
                                                                    Geom::Rect const &bleed);

/**
 * Build the PDF/X-4 XMP packet. The title is XML-escaped; `iso_date` is the
 * UTC timestamp shared with capypdf's Info dates, `doc_uuid` includes its
 * `uuid:` prefix. For non-X-4 modes this returns an empty string.
 */
std::string xmp_packet(Mode mode, std::string const &title, std::string const &iso_date,
                       std::string const &doc_uuid);

/** One preflight finding. Errors refuse the export; warnings are advisory. */
struct Finding
{
    std::string id;
    std::string reason;
    bool error;
};

/**
 * PDF/X preflight.
 *
 * Document-level: the output profile (missing/non-CMYK/unloadable), an overlap
 * of one page's bleed with another page, and master layers that carry their own
 * clip/mask/filter. Per item, in document order: an image that cannot be
 * decoded (both modes, error), and on X-1a only each transparency construct
 * (opacity, fill/stroke-opacity, mask, blend mode, gradient/mesh transparency,
 * a rasterized filter when `rasterize_filters` is set, and image alpha). Errors
 * refuse the export; warnings are advisory.
 */
std::vector<Finding> preflight(SPDocument *doc, Mode mode, bool rasterize_filters);

} // namespace Inkscape::Extension::Internal::PdfX

#endif // EXTENSION_INTERNAL_PDFOUTPUT_PDFX_H

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
