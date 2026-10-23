// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure Live Sketch model: the smoothing-to-tolerance mapping, the release-time
 * whole-stroke fit and the windowed live fitter with tangent carry.
 *
 * Geometry is fitted with lib2geom only (no SP objects, no document, no Gtk),
 * so the same rules are shared by the app.live-sketch action and the
 * interactive tool and can be unit-tested in isolation.
 *
 * All geometry is in document pixels; the caller converts. Every returned path
 * is OPEN: its endpoints are the cleaned first and last input points and its
 * segments are cubics (or line segments at zero tolerance).
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_PATH_LIVE_SKETCH_GEOMETRY_H
#define SEEN_PATH_LIVE_SKETCH_GEOMETRY_H

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <2geom/path.h>
#include <2geom/point.h>

namespace Inkscape::LiveSketch {

/// CSS pixels per millimetre at 96 dpi.
inline constexpr double kPxPerMm = 96.0 / 25.4;
/// Visual fit tolerance added per smoothing unit, in millimetres.
inline constexpr double kToleranceMmPerSmoothing = 0.05;
/// Smoothing used when the requested value is not finite.
inline constexpr double kDefaultSmoothing = 50.0;
/// Consecutive points closer than this are the same point.
inline constexpr double kDuplicateEpsilon = 1e-9;
/// Coordinates at or beyond this magnitude are off the SVG plane.
inline constexpr double kPlaneLimit = 1e18;
/// Default number of points in the live fitter's window.
inline constexpr std::size_t kWindowPoints = 16;

/**
 * The visual tolerance, in millimetres, for a smoothing value.
 *
 * clamp(@a smoothing, 0, 100) * 0.05: 0 is the exact raw polyline, 50 (the
 * default) allows 2.5 mm of deviation and 100 allows 5 mm. A non-finite value
 * uses the default. This matches Artistic Media's smoothing slider, so a
 * sketch behaves the same when it is turned into a brush stroke later.
 */
double smoothing_to_tolerance_mm(double smoothing);

/// Whether @a p is finite and inside the SVG plane coordinate range.
bool in_plane(Geom::Point const &p);

/**
 * Fit one whole stroke of @a points, in the units of the caller.
 *
 * Every point outside the SVG plane rejects the stroke (nullopt), consecutive
 * points within kDuplicateEpsilon are dropped and fewer than two remaining
 * points yield nullopt. A zero or non-finite @a tolerance yields the exact
 * polyline; otherwise the points are fitted with Geom::bezier_fit_cubic_r
 * (error = tolerance^2, at most n - 1 cubics, so noisy input is smoothed
 * instead of falling back to the raw trail) and a failed fit falls back to the
 * polyline. The result is open and its endpoints are the first and last
 * cleaned points.
 */
std::optional<Geom::Path> fit_whole(std::span<Geom::Point const> points, double tolerance);

/// Nodes of an open path: its start point plus every segment end; 0 if empty.
std::size_t node_count(Geom::Path const &path);

/**
 * The live fitter: a bounded window of the newest points, fitted afresh on
 * every accepted point so the preview is the smooth curve, not the raw trail.
 *
 * The window capacity is the requested size clamped to [3, 64]. Each window is
 * fitted as one cubic whose start tangent is the end tangent of the previously
 * committed cubic (G1 carry). A full window commits and restarts at the newest
 * point; a window that cannot be fitted in one cubic commits the previous
 * tentative cubic and restarts at THAT cubic's end, keeping the newest point
 * (the pencil reference drops it, which loses the corner).
 */
class LiveFitter {
public:
    explicit LiveFitter(double tolerance, std::size_t window = kWindowPoints);

    /// Capture @a p; false (and no state change) if it is unusable.
    bool add(Geom::Point const &p);

    /// The committed cubics followed by the tentative one, as one open path.
    Geom::Path preview() const;

    /// The release-time whole fit of every accepted point.
    std::optional<Geom::Path> finish() const;

    /// Every accepted point, in capture order.
    std::vector<Geom::Point> const &points() const { return points_; }
    std::size_t committed_segments() const { return committed_.size(); }
    std::size_t window_size() const { return window_.size(); }
    std::size_t window_capacity() const { return capacity_; }
    double tolerance() const { return tolerance_; }

private:
    /// One preview cubic, in the caller's units.
    struct Cubic {
        Geom::Point p0;
        Geom::Point p1;
        Geom::Point p2;
        Geom::Point p3;
    };

    void commit(Cubic const &cubic);

    double tolerance_;
    std::size_t capacity_;
    std::vector<Geom::Point> points_;
    std::vector<Geom::Point> window_;
    std::vector<Cubic> committed_;
    std::optional<Cubic> tentative_;
    Geom::Point tangent_;
};

} // namespace Inkscape::LiveSketch

#endif // SEEN_PATH_LIVE_SKETCH_GEOMETRY_H

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
