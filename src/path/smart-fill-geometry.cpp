// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure Smart Fill model: the face of the planar arrangement of object
 * outlines that contains a point.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "path/smart-fill-geometry.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <2geom/geom.h>
#include <2geom/rect.h>
#include <2geom/sbasis-geometric.h>

#include "path/path-boolop.h"

// NOTE: sp_pathvector_boolop(A, B, bool_op_diff, ...) returns B minus A, not
// A minus B: its implementation calls shape.Booleen(&shapeb, &shapea, bop)
// (path-boolop.cpp:193). Every difference below passes the subtrahend first
// and the minuend second, as booleans-subitems.cpp does.

namespace Inkscape::SmartFill {
namespace {

bool is_finite(Geom::Point const &p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y());
}

bool closed_subpath(Geom::Path const &path)
{
    return path.closed() ||
           Geom::are_near(path.initialPoint(), path.finalPoint(), 1e-9);
}

bool subpath_bounds_intersect(Geom::Path const &path, Geom::OptRect const &bounds)
{
    auto const path_bounds = path.boundsFast();
    return path_bounds && bounds && path_bounds->intersects(*bounds);
}

Geom::PathVector single_path(Geom::Path const &path)
{
    Geom::PathVector result;
    result.push_back(path);
    return result;
}

} // namespace

bool covers(Geom::PathVector const &path, Rule rule, Geom::Point const &p)
{
    int const winding = path.winding(p);
    return rule == Rule::EvenOdd ? winding % 2 != 0 : winding != 0;
}

void split_boundaries(Geom::PathVector const &path, std::vector<Geom::Path> &loops,
                      std::vector<Geom::Path> &lines)
{
    for (auto const &subpath : path) {
        if (subpath.empty()) {
            continue;
        }
        if (closed_subpath(subpath) && std::abs(signed_area(subpath)) >= 1e-9) {
            loops.push_back(subpath);
        } else {
            lines.push_back(subpath);
        }
    }
}

double signed_area(Geom::Path const &path)
{
    if (path.empty()) {
        return 0.0;
    }
    Geom::Point centroid;
    double area = 0.0;
    Geom::centroid(path.toPwSb(), centroid, area);
    return area;
}

double region_area(Geom::PathVector const &region)
{
    double total = 0.0;
    for (auto const &path : region) {
        total += signed_area(path);
    }
    return std::abs(total);
}

Geom::PathVector component_at(Geom::PathVector const &region, Geom::Point const &p)
{
    struct Subpath {
        Geom::Path const *path;
        double area;
        int depth;
    };

    std::vector<Subpath> subpaths;
    subpaths.reserve(region.size());
    for (auto const &path : region) {
        if (path.empty()) {
            continue;
        }
        subpaths.push_back({&path, signed_area(path), 0});
    }
    if (subpaths.empty()) {
        return {};
    }

    // Depth = how many other subpaths contain this one's first point.
    for (auto &subpath : subpaths) {
        Geom::Point const probe = subpath.path->initialPoint();
        for (auto const &other : subpaths) {
            if (&other == &subpath) {
                continue;
            }
            if (other.path->winding(probe) != 0) {
                ++subpath.depth;
            }
        }
    }

    // The innermost outer ring (even depth) containing the point.
    int outer = -1;
    for (int i = 0; i < static_cast<int>(subpaths.size()); ++i) {
        if (subpaths[i].depth % 2 != 0 || subpaths[i].path->winding(p) == 0) {
            continue;
        }
        if (outer < 0 || std::abs(subpaths[i].area) < std::abs(subpaths[outer].area)) {
            outer = i;
        }
    }
    if (outer < 0) {
        return {};
    }

    Geom::PathVector result;
    result.push_back(*subpaths[outer].path);
    double const outer_sign = subpaths[outer].area >= 0 ? 1.0 : -1.0;

    // Direct holes: odd depth, parented by the chosen outer ring.
    for (int i = 0; i < static_cast<int>(subpaths.size()); ++i) {
        if (i == outer || subpaths[i].depth % 2 == 0) {
            continue;
        }
        Geom::Point const probe = subpaths[i].path->initialPoint();
        int parent = -1;
        for (int j = 0; j < static_cast<int>(subpaths.size()); ++j) {
            if (subpaths[j].depth % 2 != 0 || subpaths[j].depth + 1 != subpaths[i].depth) {
                continue;
            }
            if (subpaths[j].path->winding(probe) == 0) {
                continue;
            }
            if (parent < 0 || std::abs(subpaths[j].area) < std::abs(subpaths[parent].area)) {
                parent = j;
            }
        }
        if (parent != outer) {
            continue;
        }
        Geom::Path hole = *subpaths[i].path;
        double const hole_sign = subpaths[i].area >= 0 ? 1.0 : -1.0;
        if (hole_sign == outer_sign) {
            hole = hole.reversed();
        }
        result.push_back(std::move(hole));
    }

    return result;
}

bool on_boundary(std::span<Source const> sources, Geom::Point const &p, double tol)
{
    for (auto const &source : sources) {
        for (auto const &path : source.path) {
            if (path.empty()) {
                continue;
            }
            Geom::Coord distance = 0.0;
            path.nearestTime(p, &distance);
            if (distance < tol) {
                return true;
            }
        }
    }
    return false;
}

Face face_at(std::span<Source const> sources, Geom::Point const &p)
{
    Face face;

    if (sources.empty() || !is_finite(p)) {
        return face;
    }

    // 1. Boundary guard.
    if (on_boundary(sources, p, kBoundaryTolerance)) {
        face.status = Status::OnBoundary;
        return face;
    }

    std::vector<Geom::Path> loops;
    std::vector<Geom::Path> lines;
    for (auto const &source : sources) {
        split_boundaries(source.path, loops, lines);
    }

    // 2. Universe around every subpath.
    Geom::OptRect total_bounds;
    for (auto const &loop : loops) {
        total_bounds |= loop.boundsFast();
    }
    for (auto const &line : lines) {
        total_bounds |= line.boundsFast();
    }
    if (!total_bounds) {
        return face;
    }

    Geom::Rect universe = *total_bounds;
    universe.expandBy(kUniverseMargin);

    Geom::PathVector region = single_path(Geom::Path(universe));

    // 3. Intersect the loops that contain the point.
    std::vector<Geom::Path> inside;
    std::vector<Geom::Path> outside;
    for (auto const &loop : loops) {
        if (loop.winding(p) != 0) {
            inside.push_back(loop);
        } else {
            outside.push_back(loop);
        }
    }

    bool const open_to_universe = inside.empty();
    if (!inside.empty()) {
        std::sort(inside.begin(), inside.end(), [](Geom::Path const &a, Geom::Path const &b) {
            return std::abs(signed_area(a)) < std::abs(signed_area(b));
        });
        region = single_path(inside.front());
        for (std::size_t i = 1; i < inside.size(); ++i) {
            region = sp_pathvector_boolop(region, single_path(inside[i]), bool_op_inters,
                                          fill_nonZero, fill_nonZero);
            if (region.empty()) {
                return face;
            }
        }
    }

    // 4. Subtract every other loop that can reach the region.
    auto region_bounds = region.boundsFast();
    Geom::PathVector subtrahend;
    for (auto const &loop : outside) {
        if (!subpath_bounds_intersect(loop, region_bounds)) {
            continue;
        }
        subtrahend = subtrahend.empty()
                         ? single_path(loop)
                         : sp_pathvector_boolop(subtrahend, single_path(loop), bool_op_union,
                                                fill_nonZero, fill_nonZero);
    }
    if (!subtrahend.empty()) {
        // Reversed operands: this is region - subtrahend.
        region = sp_pathvector_boolop(subtrahend, region, bool_op_diff, fill_nonZero, fill_nonZero);
        if (region.empty()) {
            return face;
        }
    }

    // 5. Split along open lines and self-intersecting loops.
    region_bounds = region.boundsFast();
    Geom::PathVector cuts;
    for (auto const &line : lines) {
        if (subpath_bounds_intersect(line, region_bounds)) {
            cuts.push_back(line);
        }
    }
    for (auto const &loop : loops) {
        if (!subpath_bounds_intersect(loop, region_bounds)) {
            continue;
        }
        if (!single_path(loop).intersectSelf().empty()) {
            cuts.push_back(loop);
        }
    }
    if (!cuts.empty()) {
        auto pieces = pathvector_cut(region, cuts);
        bool found = false;
        for (auto &piece : pieces) {
            if (piece.winding(p) != 0) {
                region = std::move(piece);
                found = true;
                break;
            }
        }
        if (!found) {
            return face;
        }
    }

    // 6. Keep the outer ring containing the point plus its direct holes.
    region = component_at(region, p);
    if (region.empty()) {
        return face;
    }

    // 7. Reject faces open to infinity and degenerate areas. A bounded face
    // lies inside the bounds of the boundaries; one that reaches into the
    // universe margin is open. (Comparing with the universe edges within
    // 1e-6 is not enough: livarot snaps cut output to its 1/512 grid.)
    if (open_to_universe) {
        auto const bounds = region.boundsExact();
        Geom::Rect inner = *total_bounds;
        inner.expandBy(kUniverseMargin / 2);
        if (bounds && !inner.contains(*bounds)) {
            return face;
        }
    }

    double const area = region_area(region);
    if (!std::isfinite(area) || area < kMinArea) {
        return face;
    }

    // 8. The first source whose whole geometry covers the point.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (covers(sources[i].path, sources[i].rule, p)) {
            face.source = static_cast<int>(i);
            break;
        }
    }

    face.status = Status::Ok;
    face.region = std::move(region);
    return face;
}

} // namespace Inkscape::SmartFill

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
