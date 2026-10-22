// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure artistic media model: shape-along-skeleton mapping, the exact flat-nib
 * sweep, freehand stroke fitting and the small list helpers.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live_effects/artistic-media-geometry.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <2geom/angle.h>
#include <2geom/bezier-utils.h>
#include <2geom/curve.h>
#include <2geom/d2.h>
#include <2geom/linear.h>
#include <2geom/path.h>
#include <2geom/piecewise.h>
#include <2geom/sbasis-geometric.h>
#include <2geom/sbasis-to-bezier.h>
#include <2geom/sbasis.h>
#include <2geom/svg-path-parser.h>
#include <2geom/transforms.h>

namespace Inkscape::LivePathEffect::ArtisticMedia {

namespace {

using Geom::X;
using Geom::Y;

/// Subpaths shorter than this cannot carry a stroke.
constexpr double kMinSkeletonLength = 1e-6;

/// Pieces whose cross product with the nib direction stays below this
/// fraction of their speed are parallel to the nib and sweep no area.
constexpr double kParallelRelative = 1e-12;

/// Two cut parameters closer than this are the same split point.
constexpr double kCutDuplicate = 1e-9;

/// The nib half-length sweep of one run, built in nib-aligned coordinates.
using PwD2 = Geom::Piecewise<Geom::D2<Geom::SBasis>>;

bool is_closed(Geom::Path const &path)
{
    return path.closed() || Geom::are_near(path.initialPoint(), path.finalPoint(), kConversionTolerance);
}

/// Start point of a piecewise (the first segment's at0).
Geom::Point pw_start(PwD2 const &pw)
{
    return pw.empty() ? Geom::Point() : pw[0].at0();
}

/// End point of a piecewise (the last segment's at1).
Geom::Point pw_end(PwD2 const &pw)
{
    return pw.empty() ? Geom::Point() : pw[pw.size() - 1].at1();
}

/// A straight piece from @a a to @a b as a piecewise of one linear segment.
PwD2 line_pw(Geom::Point const &a, Geom::Point const &b)
{
    PwD2 ret;
    ret.push_cut(0.0);
    ret.push(Geom::D2<Geom::SBasis>(Geom::SBasis(Geom::Linear(a[X], b[X])),
                                    Geom::SBasis(Geom::Linear(a[Y], b[Y]))),
             1.0);
    return ret;
}

/// Signed area of a piecewise (Green's theorem; the parameterisation cancels).
double signed_area(PwD2 const &pw)
{
    Geom::Point centroid;
    double area = 0.0;
    // Geom::centroid accumulates dot(p, rot90(p')) = -(x dy - y dx), so its
    // "area" is the negated shoelace area; flip it back.
    Geom::centroid(pw, centroid, area);
    return -area;
}

/**
 * The cross(tangent, nib) sign of one piece in nib-aligned coordinates, where
 * the nib points along +x so cross(tangent, nib) == -tangent.y:
 * +1, -1, or 0 when the piece is parallel to the nib (no swept area).
 */
int piece_sign(PwD2 const &piece)
{
    PwD2 const deriv = Geom::derivative(piece);
    double max_dy = 0.0;
    double max_norm = 0.0;
    for (unsigned i = 0; i < deriv.size(); ++i) {
        Geom::SBasis const &dx = deriv[i][X];
        Geom::SBasis const &dy = deriv[i][Y];
        for (int s = 0; s <= 8; ++s) {
            double const t = s / 8.0;
            double const y = dy.valueAt(t);
            double const x = dx.valueAt(t);
            max_dy = std::max(max_dy, std::fabs(y));
            max_norm = std::max(max_norm, std::hypot(x, y));
        }
    }
    if (max_dy <= kParallelRelative * max_norm) {
        return 0;
    }
    double const delta = pw_end(piece)[Y] - pw_start(piece)[Y];
    if (delta > 0.0) {
        return 1;
    }
    if (delta < 0.0) {
        return -1;
    }
    return 0;
}

/**
 * One run of nib-aligned pieces as a closed polygon: the run offset by
 * +half along the nib, a straight nib end, the run reversed and offset by
 * -half, and a straight nib end back. Oriented so the shoelace area is
 * positive (every run polygon winds the same way, which is what makes a
 * nonzero fill rule union the runs).
 */
PwD2 run_polygon(std::vector<PwD2> const &pieces, double half)
{
    PwD2 fwd = pieces.front();
    for (std::size_t i = 1; i < pieces.size(); ++i) {
        fwd.concat(pieces[i]);
    }

    Geom::Point const off(half, 0.0);
    PwD2 poly = fwd + off;
    poly.concat(line_pw(pw_end(fwd) + off, pw_end(fwd) - off));
    poly.concat(Geom::reverse(fwd) - off);
    poly.concat(line_pw(pw_start(fwd) - off, pw_start(fwd) + off));

    if (signed_area(poly) < 0.0) {
        poly = Geom::reverse(poly);
    }
    return poly;
}

/// All root parameters in (0, 1) of the y derivative, in global piecewise time.
std::vector<double> nib_parallel_cuts(PwD2 const &pw)
{
    std::vector<double> cuts;
    PwD2 const deriv = Geom::derivative(pw);
    for (unsigned i = 0; i < deriv.size(); ++i) {
        Geom::SBasis const &dy = deriv[i][Y];
        if (dy.isZero(kParallelRelative)) {
            continue;
        }
        double const t0 = deriv.cuts[i];
        double const t1 = deriv.cuts[i + 1];
        for (double const root : Geom::roots(dy, Geom::Interval(0.0, 1.0))) {
            if (root <= kCutDuplicate || root >= 1.0 - kCutDuplicate) {
                continue;
            }
            cuts.push_back(t0 + root * (t1 - t0));
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(),
                           [](double a, double b) { return b - a <= kCutDuplicate; }),
               cuts.end());
    return cuts;
}

/**
 * Split a skeleton subpath into smooth spans.
 *
 * The map P(u, v) = S(uL) + v * W * N(uL) is discontinuous wherever the
 * tangent direction jumps (a corner): the offset point from one side and the
 * next differ, so a copy of the shape spanning the corner comes out as a set
 * of open fragments that fill as garbage. Each smooth span therefore gets its
 * own copy of the shape, which stays a closed band. Smooth joins (a Bezier
 * chain, a circle converted to arcs) are one span, so a round closed skeleton
 * still runs u once around.
 */
std::vector<PwD2> smooth_spans(Geom::Path const &path)
{
    std::vector<PwD2> spans;
    PwD2 current;
    Geom::Point previous_end;
    Geom::Point previous_dir;
    bool have_previous = false;

    for (auto const &curve : path) {
        if (curve.isDegenerate()) {
            continue;
        }
        Geom::Point const start = curve.initialPoint();
        Geom::Point const dir = curve.unitTangentAt(0.0);

        bool const joins = have_previous && Geom::are_near(previous_end, start, 1e-6) &&
                           Geom::dot(previous_dir, dir) > 1.0 - 1e-9;
        if (have_previous && !joins) {
            spans.push_back(std::move(current));
            current = PwD2();
        }
        PwD2 const piece(curve.toSBasis());
        if (current.empty()) {
            current = piece;
        } else {
            current.concat(piece);
        }
        previous_end = curve.finalPoint();
        previous_dir = curve.unitTangentAt(1.0);
        have_previous = true;
    }
    if (!current.empty()) {
        spans.push_back(std::move(current));
    }
    return spans;
}

} // namespace

std::optional<Geom::PathVector> parse_unit_shape(std::string const &d)
{
    if (d.empty()) {
        return std::nullopt;
    }

    Geom::PathVector path;
    try {
        path = Geom::parse_svg_path(d.c_str());
    } catch (std::exception const &) {
        return std::nullopt;
    }
    if (path.empty()) {
        return std::nullopt;
    }

    for (auto const &subpath : path) {
        if (!subpath.closed()) {
            return std::nullopt; // every shape subpath is a closed ring
        }
        for (auto const &curve : subpath) {
            for (auto const &p : {curve.initialPoint(), curve.finalPoint()}) {
                if (!std::isfinite(p[X]) || !std::isfinite(p[Y])) {
                    return std::nullopt;
                }
            }
        }
    }

    // Curve extrema, not control points: a quadratic control point may sit
    // outside the unit band (the lens does) while the curve stays inside.
    auto const bounds = path.boundsExact();
    if (!bounds) {
        return std::nullopt;
    }
    constexpr double eps = 1e-9;
    if (bounds->left() < -eps || bounds->top() < -0.5 - eps || bounds->right() > 1.0 + eps ||
        bounds->bottom() > 0.5 + eps) {
        return std::nullopt;
    }
    return path;
}

Result map_along(Geom::PathVector const &skeleton, Geom::PathVector const &unit_shape, double width_uu)
{
    Result res;
    if (!std::isfinite(width_uu) || width_uu <= 0.0 || skeleton.empty() || unit_shape.empty()) {
        return res;
    }

    // Much credit for the mapping below should go to jfb and mgsloan of
    // lib2geom development: it follows lpe-patternalongpath.cpp, but with the
    // pattern's u/v used verbatim instead of being renormalised by its bounds.
    for (auto const &path : skeleton) {
        if (path.empty()) {
            continue;
        }

        // One arc-length skeleton per subpath (u runs once along all of it),
        // concatenated from the smooth spans so the position stays continuous
        // while the normal jumps at the corners.
        Geom::Piecewise<Geom::D2<Geom::SBasis>> uskeleton;
        for (auto const &span : smooth_spans(path)) {
            Geom::Piecewise<Geom::D2<Geom::SBasis>> const uspan =
                Geom::arc_length_parametrization(span, 3, 0.001);
            if (uspan.empty() || uspan.domain().extent() <= kMinSkeletonLength) {
                continue;
            }
            if (uskeleton.empty()) {
                uskeleton = uspan;
            } else {
                uskeleton.concat(uspan);
            }
        }
        if (uskeleton.empty()) {
            continue;
        }
        double const length = uskeleton.domain().extent();
        if (length <= kMinSkeletonLength) {
            continue;
        }
        uskeleton.setDomain(Geom::Interval(0.0, length));

        // N = rot90(unit tangent). The arc-length parametrisation makes
        // |S'| == 1 to within its fit tolerance, so this stays a unit
        // normal and the stroke width is exactly width_uu.
        Geom::Piecewise<Geom::D2<Geom::SBasis>> const normal = Geom::rot90(Geom::derivative(uskeleton));

        for (auto const &shape_path : unit_shape) {
            if (shape_path.empty()) {
                continue;
            }
            Geom::D2<Geom::Piecewise<Geom::SBasis>> const pattern =
                Geom::make_cuts_independent(shape_path.toPwSb());
            Geom::Piecewise<Geom::SBasis> x = pattern[X];
            Geom::Piecewise<Geom::SBasis> y = pattern[Y];
            x *= length;
            y *= width_uu;

            Geom::Piecewise<Geom::D2<Geom::SBasis>> const output =
                Geom::compose(uskeleton, x) + y * Geom::compose(normal, x);

            // Where the shape outline crosses a corner the normal jumps and the
            // output breaks into pieces; stitch them back into ONE closed ring
            // with straight joins (a bevel on the outside of the turn, a
            // doubly-covered overlap on the inside, both filled under nonzero).
            Geom::Path ring;
            ring.setStitching(true);
            bool started = false;
            for (auto const &piece : Geom::path_from_piecewise(output, kConversionTolerance)) {
                if (!started) {
                    ring.start(piece.initialPoint());
                    started = true;
                }
                for (auto const &curve : piece) {
                    if (!curve.isDegenerate()) {
                        ring.append(curve);
                    }
                }
            }
            if (started && !ring.empty()) {
                ring.close(true);
                res.path.push_back(std::move(ring));
            }
        }
    }

    res.valid = !res.path.empty();
    return res;
}

Result calligraphic(Geom::PathVector const &skeleton, double width_uu, double angle_deg)
{
    Result res;
    if (!std::isfinite(width_uu) || width_uu <= 0.0 || skeleton.empty()) {
        return res;
    }

    // Rotate so the nib direction d lies along +x. Rotations preserve the
    // cross product, so cross(tangent, d) is exactly -tangent.y there and the
    // swept width is a plain horizontal offset.
    Geom::Rotate const to_nib(Geom::rad_from_deg(normalize_angle(angle_deg)));
    Geom::Rotate const to_page(Geom::rad_from_deg(-normalize_angle(angle_deg)));
    double const half = 0.5 * width_uu;

    bool usable = false;
    for (auto const &path : skeleton) {
        if (path.empty() || path.length(kConversionTolerance) <= kMinSkeletonLength) {
            continue;
        }
        usable = true;

        PwD2 const aligned = (path * to_nib).toPwSb();
        Geom::Interval const domain = aligned.domain();

        // Split at the existing segment boundaries AND wherever the tangent
        // becomes parallel to the nib; inside a piece the cross sign is then
        // constant (a corner can flip it without a root inside a segment).
        std::vector<double> bounds(aligned.cuts.begin(), aligned.cuts.end());
        for (double const cut : nib_parallel_cuts(aligned)) {
            if (cut > domain.min() + kCutDuplicate && cut < domain.max() - kCutDuplicate) {
                bounds.push_back(cut);
            }
        }
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(), bounds.end(),
                                 [](double a, double b) { return b - a <= kCutDuplicate; }),
                     bounds.end());

        struct Run {
            int sign = 0;
            std::vector<PwD2> pieces;
        };
        std::vector<Run> runs;
        for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
            PwD2 piece = Geom::portion(aligned, bounds[i], bounds[i + 1]);
            int const sign = piece_sign(piece);
            if (sign == 0) {
                continue; // parallel to the nib: zero-area sweep
            }
            if (runs.empty() || runs.back().sign != sign) {
                Run run;
                run.sign = sign;
                runs.push_back(std::move(run));
            }
            runs.back().pieces.push_back(std::move(piece));
        }

        // A closed skeleton whose first and last runs share a sign sweeps
        // across the seam: join them (the pieces are contiguous there).
        if (is_closed(path) && runs.size() > 1 && runs.front().sign == runs.back().sign) {
            std::vector<PwD2> merged = std::move(runs.back().pieces);
            for (auto &piece : runs.front().pieces) {
                merged.push_back(std::move(piece));
            }
            runs.front().pieces = std::move(merged);
            runs.pop_back();
        }

        for (auto const &run : runs) {
            PwD2 poly = run_polygon(run.pieces, half);
            poly = poly * to_page;
            auto subpaths = Geom::path_from_piecewise(poly, kConversionTolerance);
            for (auto &subpath : subpaths) {
                res.path.push_back(std::move(subpath));
            }
        }
    }

    // An all-parallel sweep is valid but empty; only a degenerate skeleton is invalid.
    res.valid = usable;
    return res;
}

double normalize_angle(double deg)
{
    if (!std::isfinite(deg)) {
        return 0.0;
    }
    double angle = std::fmod(deg, 180.0);
    if (angle < 0.0) {
        angle += 180.0;
    }
    if (angle >= 180.0) {
        angle -= 180.0;
    }
    if (angle == 0.0) {
        angle = 0.0; // normalise -0
    }
    return angle;
}

std::optional<Geom::PathVector> fit_stroke(std::span<Geom::Point const> pts, double tolerance_uu)
{
    std::vector<Geom::Point> points;
    points.reserve(pts.size());
    for (auto const &p : pts) {
        if (!std::isfinite(p[X]) || !std::isfinite(p[Y])) {
            return std::nullopt;
        }
        if (points.empty() || (p - points.back()).length() > 1e-9) {
            points.push_back(p);
        }
    }
    if (points.size() < 2) {
        return std::nullopt;
    }

    Geom::Path path(points.front());
    if (tolerance_uu <= 0.0 || !std::isfinite(tolerance_uu)) {
        for (std::size_t i = 1; i < points.size(); ++i) {
            path.appendNew<Geom::LineSegment>(points[i]);
        }
    } else {
        int const count = static_cast<int>(points.size());
        unsigned const max_segments = static_cast<unsigned>(count / 2 + 1);
        std::vector<Geom::Point> controls(4 * static_cast<std::size_t>(max_segments) + 1);
        int const fitted = Geom::bezier_fit_cubic_r(controls.data(), points.data(), count,
                                                    tolerance_uu * tolerance_uu, max_segments);
        if (fitted > 0) {
            for (int c = 0; c < fitted; ++c) {
                path.appendNew<Geom::CubicBezier>(controls[4 * c + 1], controls[4 * c + 2], controls[4 * c + 3]);
            }
        } else {
            for (std::size_t i = 1; i < points.size(); ++i) {
                path.appendNew<Geom::LineSegment>(points[i]);
            }
        }
    }

    Geom::PathVector out;
    out.push_back(std::move(path));
    return out;
}

double smoothing_to_tolerance_mm(double smoothing)
{
    return std::clamp(smoothing, 0.0, 100.0) * 0.05;
}

std::size_t node_count(Geom::PathVector const &pv)
{
    std::size_t count = 0;
    for (auto const &path : pv) {
        if (path.empty()) {
            continue;
        }
        count += 1 + path.size_open();
    }
    return count;
}

double skeleton_length(Geom::PathVector const &pv)
{
    double length = 0.0;
    for (auto const &path : pv) {
        length += path.length(kConversionTolerance);
    }
    return length;
}

} // namespace Inkscape::LivePathEffect::ArtisticMedia

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
