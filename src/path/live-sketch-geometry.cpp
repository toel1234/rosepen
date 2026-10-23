// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure Live Sketch model: the smoothing-to-tolerance mapping, the release-time
 * whole-stroke fit and the windowed live fitter with tangent carry.
 *
 * The input cleaning and the polyline fallback follow ArtisticMedia::fit_stroke
 * (live_effects/artistic-media-geometry.cpp); the window rotation and the
 * tangent carry follow PencilTool::_fitAndSplit (ui/tools/pencil-tool.cpp),
 * except that this fitter keeps the newest point on a window restart. The
 * in_plane guard mirrors pencil-tool.cpp's in_svg_plane.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "path/live-sketch-geometry.h"

#include <algorithm>
#include <cmath>

#include <2geom/bezier-utils.h>

namespace Inkscape::LiveSketch {

namespace {

/// End tangent of a cubic: the last non-degenerate control polygon edge, so a
/// cubic whose final handle is collapsed still carries a direction.
Geom::Point end_tangent(Geom::Point const &p0, Geom::Point const &p1, Geom::Point const &p2,
                        Geom::Point const &p3)
{
    Geom::Point const candidates[] = {p3 - p2, p3 - p1, p3 - p0};
    for (auto const &edge : candidates) {
        if (!Geom::is_zero(edge)) {
            return in_plane(edge) ? Geom::unit_vector(edge) : Geom::Point();
        }
    }
    return Geom::Point();
}

/// One cubic fit of @a window; false when lib2geom cannot stay within @a tolerance.
bool fit_window(std::vector<Geom::Point> const &window, Geom::Point const &tangent, double tolerance,
                Geom::Point fitted[4])
{
    int const count = Geom::bezier_fit_cubic_full(fitted, nullptr, window.data(),
                                                  static_cast<int>(window.size()), tangent,
                                                  Geom::Point{}, tolerance * tolerance, 1);
    return count > 0;
}

void append_cubic(Geom::Path &path, Geom::Point const &p1, Geom::Point const &p2, Geom::Point const &p3)
{
    path.appendNew<Geom::CubicBezier>(p1, p2, p3);
}

void append_polyline(Geom::Path &path, std::vector<Geom::Point> const &points)
{
    for (std::size_t i = 1; i < points.size(); ++i) {
        path.appendNew<Geom::LineSegment>(points[i]);
    }
}

bool finite(Geom::Point const &p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y());
}

} // namespace

double smoothing_to_tolerance_mm(double smoothing)
{
    if (!std::isfinite(smoothing)) {
        smoothing = kDefaultSmoothing;
    }
    return std::clamp(smoothing, 0.0, 100.0) * kToleranceMmPerSmoothing;
}

bool in_plane(Geom::Point const &p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y()) && Geom::LInfty(p) < kPlaneLimit;
}

std::optional<Geom::Path> fit_whole(std::span<Geom::Point const> points, double tolerance)
{
    std::vector<Geom::Point> cleaned;
    cleaned.reserve(points.size());
    for (auto const &p : points) {
        if (!in_plane(p)) {
            return std::nullopt;
        }
        if (cleaned.empty() || (p - cleaned.back()).length() > kDuplicateEpsilon) {
            cleaned.push_back(p);
        }
    }
    if (cleaned.size() < 2) {
        return std::nullopt;
    }

    Geom::Path path(cleaned.front());
    if (!(tolerance > 0.0) || !std::isfinite(tolerance)) {
        append_polyline(path, cleaned);
        return path;
    }

    // One cubic per input point at most, so noisy freehand input is smoothed
    // rather than rejected as too many segments (fit_stroke caps at n/2 + 1).
    auto const count = static_cast<int>(cleaned.size());
    auto const max_segments = static_cast<unsigned>(count - 1);
    std::vector<Geom::Point> controls(4 * static_cast<std::size_t>(max_segments) + 1);
    int const fitted = Geom::bezier_fit_cubic_r(controls.data(), cleaned.data(), count,
                                                tolerance * tolerance, max_segments);

    bool usable = fitted > 0;
    for (int c = 0; usable && c < fitted; ++c) {
        for (int k = 1; k <= 3; ++k) {
            usable = finite(controls[4 * c + k]);
        }
    }
    if (!usable) {
        append_polyline(path, cleaned);
        return path;
    }

    for (int c = 0; c < fitted; ++c) {
        append_cubic(path, controls[4 * c + 1], controls[4 * c + 2], controls[4 * c + 3]);
    }
    return path;
}

std::size_t node_count(Geom::Path const &path)
{
    if (path.empty()) {
        return 0;
    }
    return path.size_open() + 1;
}

LiveFitter::LiveFitter(double tolerance, std::size_t window)
    : tolerance_(tolerance)
    , capacity_(std::clamp(window, std::size_t{3}, std::size_t{64}))
    , tangent_()
{
}

bool LiveFitter::add(Geom::Point const &p)
{
    if (!in_plane(p)) {
        return false;
    }
    if (!points_.empty() && (p - points_.back()).length() <= kDuplicateEpsilon) {
        return false;
    }
    points_.push_back(p);

    if (!(tolerance_ > 0.0) || !std::isfinite(tolerance_)) {
        // Zero tolerance is the raw trail; no window is kept (it would grow
        // without bound while the user drags).
        return true;
    }

    window_.push_back(p);
    if (window_.size() < 2) {
        return true;
    }

    Geom::Point fitted[4];
    if (fit_window(window_, tangent_, tolerance_, fitted)) {
        Cubic const cubic{fitted[0], fitted[1], fitted[2], fitted[3]};
        if (window_.size() < capacity_) {
            tentative_ = cubic;
        } else {
            // A full window: the fit is final, so commit it, drop the tentative
            // cubic (its points are covered) and restart at the newest point.
            commit(cubic);
            tentative_.reset();
            window_.assign(1, p);
        }
        return true;
    }

    // The window cannot be one cubic. The previous tentative covered every
    // point but the newest and ends exactly at window_[size - 2]; commit it
    // and restart from there, keeping the newest point.
    if (tentative_) {
        commit(*tentative_);
        Geom::Point const restart = tentative_->p3;
        tentative_.reset();
        window_.clear();
        window_.push_back(restart);
    } else {
        window_.clear();
    }
    window_.push_back(p);

    // A two-point window always fits, so the restart has a tentative join with
    // the carried tangent.
    if (fit_window(window_, tangent_, tolerance_, fitted)) {
        tentative_ = Cubic{fitted[0], fitted[1], fitted[2], fitted[3]};
    }
    return true;
}

Geom::Path LiveFitter::preview() const
{
    if (points_.empty()) {
        return Geom::Path();
    }

    Geom::Path path(points_.front());
    if (!(tolerance_ > 0.0) || !std::isfinite(tolerance_)) {
        append_polyline(path, points_);
        return path;
    }

    for (auto const &cubic : committed_) {
        append_cubic(path, cubic.p1, cubic.p2, cubic.p3);
    }
    if (tentative_) {
        append_cubic(path, tentative_->p1, tentative_->p2, tentative_->p3);
    }
    return path;
}

std::optional<Geom::Path> LiveFitter::finish() const
{
    return fit_whole(points_, tolerance_);
}

void LiveFitter::commit(Cubic const &cubic)
{
    committed_.push_back(cubic);
    tangent_ = end_tangent(cubic.p0, cubic.p1, cubic.p2, cubic.p3);
}

} // namespace Inkscape::LiveSketch

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
