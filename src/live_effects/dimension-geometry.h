// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure dimension model: anchors, per-kind geometry and label formatting.
 *
 * This is the single source of truth shared by the "dimension" live path
 * effect, the app.dimension-* actions and the interactive Dimension tool.
 * It depends only on lib2geom, glibmm and util units - no SPDocument,
 * SPItem or Gtk - so it is fully unit-testable in isolation.
 *
 * All geometry happens in document coordinates. Document coordinates are
 * y-down (SVG); callers rendering to a y-up desktop pass y_down = false.
 * Lengths in Style are document user units; conversion from mm etc. is the
 * caller's business.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_LIVE_EFFECTS_DIMENSION_GEOMETRY_H
#define SEEN_LIVE_EFFECTS_DIMENSION_GEOMETRY_H

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/rect.h>
#include <glibmm/ustring.h>

namespace Inkscape::LivePathEffect::Dimension {

/// What a dimension measures. "auto" exists only at creation time and is
/// resolved to Horizontal or Vertical by resolve_auto(); it is never stored.
enum class Kind {
    Aligned,
    Horizontal,
    Vertical,
    Angular,
    Segment,
};

/// Parse a stored kind key. "auto" and anything unknown yield nullopt.
std::optional<Kind> parse_kind(std::string_view key);

/// The canonical key of a kind: aligned|horizontal|vertical|angular|segment.
char const *kind_key(Kind kind);

/// Creation-time resolution: Horizontal if |dx| >= |dy|, else Vertical.
Kind resolve_auto(Geom::Point p1, Geom::Point p2);

/**
 * Where a dimension point is anchored on a measured item.
 *
 * Grammar (shared by LPE params, actions and tool):
 *  - bbox:<tl|t|tr|l|c|r|bl|b|br> : one of the 9 handles of the document
 *    geometric bounding box;
 *  - node:<k> : the k-th node of the item's curve in document coords (each
 *    subpath's start point plus every segment end point; a closed subpath's
 *    closing segment end is not counted twice);
 *  - rel:<u>/<v> : fraction of the geometric bbox, follows move AND scale.
 */
struct Anchor {
    enum Type { BBox, Node, Rel };
    Type type = BBox;
    int handle = -1; ///< BBox only: 0..8, tl t tr l c r bl b br.
    int node = -1;   ///< Node only: k >= 0.
    double u = 0.0;  ///< Rel only.
    double v = 0.0;  ///< Rel only.
};

/// Parse an anchor spec; rejects empty specs, unknown prefixes, unknown
/// bbox handles, negative/non-numeric node indices, missing or non-finite
/// rel components (NaN, inf) and trailing garbage.
std::optional<Anchor> parse_anchor(std::string_view spec);

/// Exact round-trip form of an anchor ("rel" uses "/" and g_ascii_dtostr).
std::string format_anchor(Anchor const &anchor);

/// The k-th node of @a curve_doc in document coords (see Anchor::node).
std::vector<Geom::Point> curve_nodes(Geom::PathVector const &curve_doc);

/**
 * Resolve an anchor against a document geometric bbox and curve.
 *
 * Empty bbox where a bbox/rel anchor needs it, or an out-of-range node
 * index, yields nullopt (the LPE freezes on its last points instead).
 */
std::optional<Geom::Point> resolve_anchor(Anchor const &anchor,
                                          Geom::OptRect const &geom_bbox_doc,
                                          Geom::PathVector const &curve_doc);

/// Where the label sits relative to the dimension line / arc.
enum class TextPosition {
    Above,  ///< Past the line, away from the measured geometry.
    Inline, ///< On the line / on the arc.
    Below,  ///< Between the line and the measured geometry.
};

/// Visual styling of the built geometry, all lengths in document user units.
struct Style {
    double offset = 5.0;        ///< Dim line distance from the measured points; angular: arc radius (<= 0: half the shorter arm).
    double arrow_length = 2.5;  ///< Arrowhead length along the line.
    double ext_gap = 1.0;       ///< Gap between measured point and extension line start.
    double ext_overshoot = 2.0; ///< Extension line overshoot past the dim line (clamped at 0).
    TextPosition text_position = TextPosition::Above;
    bool text_horizontal = false; ///< True: label angle 0; false: parallel to the line, upright-normalised.
    double label_gap = 1.0;       ///< Label shift per TextPosition; negative Below uses it toward the geometry.
};

/// The built dimension: plain geometry, label placement and the measured value.
struct Result {
    Geom::PathVector path;      ///< Extension lines, dim line/arc, arrowheads (empty when invalid).
    Geom::Point label_pos;      ///< Anchor point of the label text.
    double label_angle_deg = 0; ///< Rotation for svg transform="rotate()", always upright in [-90, 90] unless text_horizontal.
    double value_uu_or_deg = 0; ///< Linear kinds: length in user units; Angular: angle in degrees.
    bool valid = false;         ///< False for degenerate input (coincident points, zero arm, non-finite input).
};

/**
 * Build the geometry of one dimension.
 *
 * @param pts 2 points (P1, P2) for Aligned/Horizontal/Vertical/Segment or
 *            3 points (A, V, B) for Angular (angle between V->A and V->B).
 *
 * Emitted subpaths, in order: extension lines, dimension line (or the
 * angular arc emitted as arc + reversed arc, enclosing zero area), then two
 * closed filled arrowhead triangles. Arrowheads point outward (tip at the
 * extension line / arc end) and are flipped to the OUTSIDE (pointing
 * inward, bodies beyond the extension lines) when the line is shorter than
 * 2.5 x arrow_length. Degenerate input yields valid == false, an empty path
 * and no NaN anywhere.
 */
Result build(Kind kind, std::span<Geom::Point const> pts, Style const &style, bool y_down = true);

/**
 * Format a dimension label: {prefix}{value}{sep}{unit}{suffix}.
 *
 * The value uses classic-locale fixed formatting with a '.' decimal point
 * (precision clamped to 0..6); a negative zero result is normalised to
 * positive. @a sep is " " for linear units and "" for the degree sign.
 * Units are omitted when @a show_units is false; for an angular label with
 * an empty @a unit_abbr the degree sign is used. Classic-suite default look:
 * format_label(50.0, 2, "mm", true) == "50.00 mm".
 */
Glib::ustring format_label(double value, int precision, Glib::ustring const &unit_abbr, bool show_units,
                           Glib::ustring const &prefix, Glib::ustring const &suffix, bool angular);

/**
 * Convert a user-unit length to a display unit.
 *
 * value = Quantity::convert(length_uu * doc_scale_uu_to_px, "px", unit) * drawing_scale,
 * i.e. the physical length in @a unit after the document scale, times the
 * drawing scale factor.
 */
double to_unit(double length_uu, double doc_scale_uu_to_px, Glib::ustring const &unit, double drawing_scale = 1.0);

} // namespace Inkscape::LivePathEffect::Dimension

#endif // SEEN_LIVE_EFFECTS_DIMENSION_GEOMETRY_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4