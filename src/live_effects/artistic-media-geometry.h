// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure artistic media model: shape-along-skeleton mapping, the exact flat-nib
 * sweep, freehand stroke fitting and the small list helpers.
 *
 * This is the single source of truth shared by the "artistic_media" live
 * path effect, the app.artistic-* actions and the interactive Artistic Media
 * tool. It depends only on lib2geom and glib - no SPDocument, SPItem or Gtk -
 * so it is fully unit-testable in isolation.
 *
 * Coordinates are the path's own user units, y-down as in SVG.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_LIVE_EFFECTS_ARTISTIC_MEDIA_GEOMETRY_H
#define SEEN_LIVE_EFFECTS_ARTISTIC_MEDIA_GEOMETRY_H

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include <2geom/pathvector.h>
#include <2geom/point.h>

namespace Inkscape::LivePathEffect::ArtisticMedia {

/// Same approximation tolerance the LPE framework uses to turn SBasis output
/// into curves (LPE_CONVERSION_TOLERANCE in live_effects/effect.h).
inline constexpr double kConversionTolerance = 0.01;

/**
 * Pixels per millimetre, the SVG/CSS definition (96 px per inch).
 *
 * The document scale converts user units to pixels, so mm <-> user units is
 * mm * kPxPerMm / document_scale. The constant is spelled out rather than
 * read from the process-wide unit table so headless callers (tests, tools)
 * produce exactly the same numbers as the GUI.
 */
inline constexpr double kPxPerMm = 96.0 / 25.4;

/// Built output of one stroke.
struct Result {
    Geom::PathVector path; ///< Brush outline(s); empty when nothing is swept.
    bool valid = false;    ///< False for degenerate input (no usable skeleton, width <= 0).
};

/**
 * Parse and validate embedded unit-space shape data.
 *
 * A shape is one or more CLOSED subpaths inside u [0, 1] x v [-0.5, 0.5].
 * Empty input, malformed path data, non-finite coordinates, a subpath outside
 * those bounds or an open subpath yield nullopt.
 */
std::optional<Geom::PathVector> parse_unit_shape(std::string const &d);

/**
 * Map @a unit_shape onto every skeleton subpath (Preset and Brush modes).
 *
 * Per subpath of arc length L:  P(u, v) = S(u * L) + v * width * N(u * L),
 * where S is arc-length parametrised and N = rot90(unit tangent) (so for
 * travel towards +x, v > 0 is the +y side). Shapes are not renormalised:
 * u and v are absolute, so every library shape spans the whole width and
 * the area on a straight skeleton is area_fraction * L * width.
 *
 * Every subpath with L > 1e-6 gets ONE copy of every shape subpath, u running
 * once along the whole subpath (a closed one once around), so a taper on a
 * polyline or a freehand polyline tapers once, not per edge. The normal jumps
 * where the tangent direction does (a corner); the pieces of a shape outline
 * on either side of a corner are stitched back into one closed ring with a
 * straight join (a bevel outside the turn, a doubly-covered overlap inside it,
 * both filled under nonzero). Very sharp corners fold (accepted, as Pattern
 * Along Path does).
 *
 * The pattern-along-path math (arc_length_parametrization, rot90(
 * derivative()), compose) follows live_effects/lpe-patternalongpath.cpp, with
 * much credit to jfb and mgsloan of lib2geom development, but the pattern is
 * not bounds-renormalised, the normal is used exactly as the unit tangent
 * rotated (no force_continuity), and the width is absolute rather than a
 * scale factor.
 *
 * invalid when no span is long enough, or @a width_uu is <= 0 / non-finite.
 */
Result map_along(Geom::PathVector const &skeleton, Geom::PathVector const &unit_shape, double width_uu);

/**
 * Sweep a flat nib of length @a width_uu held at @a angle_deg along the skeleton.
 *
 * The nib direction is d = (cos angle, -sin angle), i.e. the angle runs
 * counter-clockwise on screen, and it is page-fixed (transforms never change
 * it). Every subpath is split where the tangent becomes parallel to d; runs
 * of constant cross(C', d) sign become closed polygons (run + (W/2)d
 * forward, line, run - (W/2)d reversed, close) with a uniform orientation,
 * so a nonzero fill rule yields exactly the swept region without any boolean
 * step. Ends are flat (nib-shaped).
 *
 * A skeleton entirely parallel to the nib yields valid == true and an EMPTY
 * path (the sweep has zero area); a zero-length skeleton or a width <= 0
 * yields valid == false.
 */
Result calligraphic(Geom::PathVector const &skeleton, double width_uu, double angle_deg);

/// Normalise an angle in degrees to [0, 180) (a nib and its reverse are the same nib).
double normalize_angle(double deg);

/**
 * Fit a freehand stroke through @a pts.
 *
 * Consecutive points closer than 1e-9 are dropped. A tolerance of 0 yields
 * the exact polyline through the points; otherwise the points are fitted
 * with Geom::bezier_fit_cubic_r (error = tolerance^2, at most points/2 + 1
 * segments) and the endpoints stay exact. Fewer than two distinct points
 * yield nullopt.
 */
std::optional<Geom::PathVector> fit_stroke(std::span<Geom::Point const> pts, double tolerance_uu);

/// Freehand smoothing (0..100) to a fit tolerance in mm: clamp(smoothing, 0, 100) * 0.05.
double smoothing_to_tolerance_mm(double smoothing);

/**
 * Skeleton node count: per subpath its start point plus every segment end,
 * the closing segment of a closed subpath not counted.
 */
std::size_t node_count(Geom::PathVector const &pv);

/// Total skeleton length, the closing segment of a closed subpath included.
double skeleton_length(Geom::PathVector const &pv);

} // namespace Inkscape::LivePathEffect::ArtisticMedia

#endif // SEEN_LIVE_EFFECTS_ARTISTIC_MEDIA_GEOMETRY_H

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
