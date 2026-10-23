// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure Smart Fill model: the face of the planar arrangement of object
 * outlines that contains a point.
 *
 * The region is computed from boundary pathvectors only (no SP objects, no
 * document, no Gtk), so it is shared by the app.smart-fill action and the
 * interactive tool and can be unit-tested in isolation.
 *
 * All geometry is in document user units, y-down (SVG). Sources are passed
 * topmost first; only the source style is picked from them, never geometry.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_PATH_SMART_FILL_GEOMETRY_H
#define SEEN_PATH_SMART_FILL_GEOMETRY_H

#include <span>
#include <vector>

#include <2geom/pathvector.h>
#include <2geom/point.h>

namespace Inkscape::SmartFill {

/// How a source's whole geometry decides whether it covers a point.
enum class Rule {
    NonZero,
    EvenOdd,
};

/// One object's boundary geometry in document user units.
struct Source {
    Geom::PathVector path;
    Rule rule = Rule::NonZero;
};

enum class Status {
    Ok,
    NoRegion,
    OnBoundary,
};

/// The face at a point; region is empty unless status is Ok.
struct Face {
    Status status = Status::NoRegion;
    Geom::PathVector region;
    int source = -1;
};

/// Distance below which a point counts as lying on an outline.
inline constexpr double kBoundaryTolerance = 1e-3;
/// Margin around the boundary bounds used to close the unbounded face.
inline constexpr double kUniverseMargin = 10.0;
/// Regions smaller than this are treated as empty.
inline constexpr double kMinArea = 1e-6;

/**
 * The enclosed face containing @a p.
 *
 * Step by step: reject boundary clicks; build a universe around all
 * subpaths; intersect the loops containing the point; subtract the other
 * loops; split the result along open lines and self-intersecting loops;
 * keep the outer ring containing the point with its direct holes; reject
 * faces open to the universe; then pick the first source (topmost first)
 * whose whole geometry covers the point under the source's own fill rule.
 */
Face face_at(std::span<Source const> sources, Geom::Point const &p);

/// Whether @a path covers @a p under @a rule.
bool covers(Geom::PathVector const &path, Rule rule, Geom::Point const &p);

/// Classify each subpath as a closed loop or an open/zero-area line.
void split_boundaries(Geom::PathVector const &path, std::vector<Geom::Path> &loops,
                      std::vector<Geom::Path> &lines);

/// Signed area of one subpath (curves included, closing segment implied).
double signed_area(Geom::Path const &path);

/// |sum of signed areas| of a normalised region (outer minus holes).
double region_area(Geom::PathVector const &region);

/// Keep the outer ring containing @a p plus its direct holes; empty if none.
Geom::PathVector component_at(Geom::PathVector const &region, Geom::Point const &p);

/// Whether @a p is closer than @a tol to any loop or line.
bool on_boundary(std::span<Source const> sources, Geom::Point const &p, double tol);

} // namespace Inkscape::SmartFill

#endif // SEEN_PATH_SMART_FILL_GEOMETRY_H

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
