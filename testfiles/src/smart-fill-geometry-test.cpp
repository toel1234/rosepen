// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure Smart Fill model: boundary classification,
 * per-loop containment, face at a point with holes, open-line and
 * self-intersection splits, unbounded detection and source pick.
 *
 * Expected values are hand derivations, never copied from output.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "path/smart-fill-geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <2geom/circle.h>
#include <2geom/pathvector.h>
#include <2geom/rect.h>
#include <2geom/svg-path-parser.h>

#include "path/path-boolop.h"

using namespace Inkscape::SmartFill;

namespace {

Geom::PathVector read_pv(char const *d)
{
    return Geom::parse_svg_path(d);
}

Geom::PathVector rect_pv(double x0, double y0, double x1, double y1)
{
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "M %g,%g H %g V %g H %g Z", x0, y0, x1, y1, x0);
    return read_pv(buffer);
}

Geom::PathVector circle_pv(Geom::Point const &centre, double radius)
{
    Geom::PathVector result;
    result.push_back(Geom::Path(Geom::Circle(centre, radius)));
    return result;
}

/// Self-intersecting five-pointed star; the outline is one closed subpath.
Geom::PathVector pentagram_pv(double cx, double cy, double radius)
{
    Geom::Path path;
    for (int k = 0; k < 5; ++k) {
        double const theta = -M_PI / 2.0 + 4.0 * M_PI * k / 5.0; // 144 degree steps
        Geom::Point const point(cx + radius * std::cos(theta), cy + radius * std::sin(theta));
        if (k == 0) {
            path.start(point);
        } else {
            path.appendNew<Geom::LineSegment>(point);
        }
    }
    path.close();

    Geom::PathVector result;
    result.push_back(std::move(path));
    return result;
}

void expect_area(double actual, double expected, double relative = 0.005)
{
    EXPECT_NEAR(actual, expected, std::max(std::abs(expected) * relative, 1e-9))
        << "expected area " << expected << ", got " << actual;
}

void expect_bbox(Geom::PathVector const &region, double x0, double y0, double x1, double y1,
                 double tolerance = 0.02)
{
    auto const bounds = region.boundsExact();
    ASSERT_TRUE(bounds) << "empty region";
    EXPECT_NEAR(bounds->min().x(), x0, tolerance);
    EXPECT_NEAR(bounds->min().y(), y0, tolerance);
    EXPECT_NEAR(bounds->max().x(), x1, tolerance);
    EXPECT_NEAR(bounds->max().y(), y1, tolerance);
}

std::size_t segment_count(Geom::PathVector const &region)
{
    std::size_t count = 0;
    for (auto const &path : region) {
        count += path.size();
    }
    return count;
}

/// Rect 10..50 x 10..40 and circle centre (50,25) r 10, circle on top.
std::vector<Source> lens_sources()
{
    return {
        {circle_pv(Geom::Point(50, 25), 10.0), Rule::NonZero},
        {rect_pv(10, 10, 50, 40), Rule::NonZero},
    };
}

} // namespace

// -------------------------------------------------------------------------

TEST(SmartFillOperandOrderTest, DiffIsSecondMinusFirst)
{
    // Lock the reversed operand order of sp_pathvector_boolop: the whole
    // algorithm depends on diff returning the second operand minus the first.
    auto const small = rect_pv(0, 0, 10, 10);
    auto const large = rect_pv(0, 0, 20, 20);

    auto const difference = sp_pathvector_boolop(small, large, bool_op_diff, fill_nonZero, fill_nonZero);
    expect_area(region_area(difference), 300.0, 1e-6);

    auto const swapped = sp_pathvector_boolop(large, small, bool_op_diff, fill_nonZero, fill_nonZero);
    EXPECT_LT(region_area(swapped), 1e-6);
}

// -------------------------------------------------------------------------

TEST(SmartFillLensTest, OverlappingRectAndCircle)
{
    auto const sources = lens_sources();

    auto const lens = face_at(sources, Geom::Point(45, 25));
    ASSERT_EQ(lens.status, Status::Ok);
    expect_area(region_area(lens.region), 157.080); // pi * 100 / 2
    expect_bbox(lens.region, 40, 15, 50, 35);
    EXPECT_EQ(lens.source, 0); // the circle
    EXPECT_LE(segment_count(lens.region), 8u) << "curves must stay curves, not be flattened";

    auto const rest = face_at(sources, Geom::Point(20, 25));
    ASSERT_EQ(rest.status, Status::Ok);
    expect_area(region_area(rest.region), 1042.920); // 1200 - 157.080
    expect_bbox(rest.region, 10, 10, 50, 40);
    EXPECT_EQ(rest.source, 1); // the rectangle

    auto const outside = face_at(sources, Geom::Point(55, 25));
    ASSERT_EQ(outside.status, Status::Ok);
    expect_area(region_area(outside.region), 157.080);
    expect_bbox(outside.region, 50, 15, 60, 35);
    EXPECT_EQ(outside.source, 0);
}

TEST(SmartFillLensTest, OpenLineSplitsTheRemainingRectangle)
{
    std::vector<Source> sources = lens_sources();
    sources.push_back({read_pv("M 30,5 V 45"), Rule::NonZero});

    auto const left = face_at(sources, Geom::Point(20, 25));
    ASSERT_EQ(left.status, Status::Ok);
    expect_area(region_area(left.region), 600.0);
    expect_bbox(left.region, 10, 10, 30, 40);

    auto const right = face_at(sources, Geom::Point(35, 12));
    ASSERT_EQ(right.status, Status::Ok);
    expect_area(region_area(right.region), 442.920); // 600 - 157.080
    expect_bbox(right.region, 30, 10, 50, 40);

    // The line does not reach the disc: the lens is unchanged.
    auto const lens = face_at(sources, Geom::Point(45, 25));
    ASSERT_EQ(lens.status, Status::Ok);
    expect_area(region_area(lens.region), 157.080);
    expect_bbox(lens.region, 40, 15, 50, 35);
}

// -------------------------------------------------------------------------

TEST(SmartFillDonutTest, EvenOddRingAndHole)
{
    std::vector<Source> sources = {
        {read_pv("M 70,10 H 110 V 50 H 70 Z M 80,20 V 40 H 100 V 20 Z"), Rule::EvenOdd},
    };

    auto const ring = face_at(sources, Geom::Point(75, 30));
    ASSERT_EQ(ring.status, Status::Ok);
    expect_area(region_area(ring.region), 1200.0);
    EXPECT_EQ(ring.region.size(), 2u) << "outer ring plus hole";
    expect_bbox(ring.region, 70, 10, 110, 50);
    EXPECT_FALSE(covers(ring.region, Rule::EvenOdd, Geom::Point(90, 30)));
    EXPECT_FALSE(covers(ring.region, Rule::NonZero, Geom::Point(90, 30)));
    EXPECT_EQ(ring.source, 0);

    // The hole is enclosed by the inner ring, not by the donut's even-odd fill.
    auto const hole = face_at(sources, Geom::Point(90, 30));
    ASSERT_EQ(hole.status, Status::Ok);
    expect_area(region_area(hole.region), 400.0);
    expect_bbox(hole.region, 80, 20, 100, 40);
    EXPECT_EQ(hole.source, -1);
}

TEST(SmartFillDonutTest, NonZeroAndSameWindingStillHoleByLoop)
{
    // Both subpaths wound the same way: per-loop containment makes the ring
    // a hole either way, but nonzero winding now covers the hole centre.
    std::vector<Source> sources = {
        {read_pv("M 70,10 H 110 V 50 H 70 Z M 80,20 H 100 V 40 H 80 Z"), Rule::NonZero},
    };

    auto const hole = face_at(sources, Geom::Point(90, 30));
    ASSERT_EQ(hole.status, Status::Ok);
    expect_area(region_area(hole.region), 400.0);
    EXPECT_EQ(hole.source, 0);
}

// -------------------------------------------------------------------------

TEST(SmartFillNestedTest, NestedSquares)
{
    std::vector<Source> sources = {
        {rect_pv(140, 20, 150, 30), Rule::NonZero},
        {rect_pv(130, 10, 160, 40), Rule::NonZero},
    };

    auto const ring = face_at(sources, Geom::Point(135, 15));
    ASSERT_EQ(ring.status, Status::Ok);
    expect_area(region_area(ring.region), 800.0); // 900 - 100
    EXPECT_EQ(ring.region.size(), 2u) << "outer ring plus hole";

    auto const inner = face_at(sources, Geom::Point(145, 25));
    ASSERT_EQ(inner.status, Status::Ok);
    expect_area(region_area(inner.region), 100.0);
    EXPECT_EQ(inner.region.size(), 1u);
}

TEST(SmartFillNestedTest, IslandInHole)
{
    std::vector<Source> sources = {
        {rect_pv(143, 23, 147, 27), Rule::NonZero}, // island, topmost
        {rect_pv(140, 20, 150, 30), Rule::NonZero},
        {rect_pv(130, 10, 160, 40), Rule::NonZero},
    };

    // A click inside the island is the island's own face, not the ring.
    auto const island = face_at(sources, Geom::Point(145, 25));
    ASSERT_EQ(island.status, Status::Ok);
    expect_area(region_area(island.region), 16.0); // 4 x 4
    EXPECT_EQ(island.region.size(), 1u);
    EXPECT_EQ(island.source, 0);

    // A click in the ring around the island keeps the island as a hole.
    auto const ring = face_at(sources, Geom::Point(141, 21));
    ASSERT_EQ(ring.status, Status::Ok);
    expect_area(region_area(ring.region), 84.0); // 100 - 16
    EXPECT_EQ(ring.region.size(), 2u);

    // An unrelated loop whose bounds miss the region is not subtracted.
    std::vector<Source> pruned = {
        {rect_pv(200, 10, 210, 20), Rule::NonZero},
        {rect_pv(140, 20, 150, 30), Rule::NonZero},
        {rect_pv(130, 10, 160, 40), Rule::NonZero},
    };
    auto const unchanged = face_at(pruned, Geom::Point(145, 25));
    ASSERT_EQ(unchanged.status, Status::Ok);
    expect_area(region_area(unchanged.region), 100.0);
}

TEST(SmartFillAbuttingTest, SharedEdgeRects)
{
    // R2: coincident edges must not turn into slivers.
    std::vector<Source> sources = {
        {rect_pv(0, 0, 10, 10), Rule::NonZero},
        {rect_pv(10, 0, 20, 10), Rule::NonZero},
    };

    auto const left = face_at(sources, Geom::Point(5, 5));
    ASSERT_EQ(left.status, Status::Ok);
    expect_area(region_area(left.region), 100.0);
    EXPECT_EQ(left.region.size(), 1u) << "one outer ring";
}

// -------------------------------------------------------------------------

TEST(SmartFillOpenLinesTest, GridGivesTheCentreSquare)
{
    std::vector<Source> sources = {
        {read_pv("M 10,0 V 40"), Rule::NonZero},
        {read_pv("M 30,0 V 40"), Rule::NonZero},
        {read_pv("M 0,10 H 40"), Rule::NonZero},
        {read_pv("M 0,30 H 40"), Rule::NonZero},
    };

    auto const cell = face_at(sources, Geom::Point(20, 20));
    ASSERT_EQ(cell.status, Status::Ok);
    expect_area(region_area(cell.region), 400.0);
    expect_bbox(cell.region, 10, 10, 30, 30);
    EXPECT_EQ(cell.source, -1);

    EXPECT_EQ(face_at(sources, Geom::Point(5, 20)).status, Status::NoRegion);
    EXPECT_EQ(face_at(sources, Geom::Point(60, 60)).status, Status::NoRegion);
}

TEST(SmartFillOpenLinesTest, CrossingAndDanglingLine)
{
    std::vector<Source> crossing = {
        {rect_pv(0, 0, 100, 50), Rule::NonZero},
        {read_pv("M 50,-10 V 60"), Rule::NonZero},
    };
    auto const half = face_at(crossing, Geom::Point(25, 25));
    ASSERT_EQ(half.status, Status::Ok);
    expect_area(region_area(half.region), 2500.0);
    expect_bbox(half.region, 0, 0, 50, 50);

    // R1: a line that stops inside the rectangle does not split the face.
    std::vector<Source> dangling = {
        {rect_pv(0, 0, 100, 50), Rule::NonZero},
        {read_pv("M 50,-10 V 25"), Rule::NonZero},
    };
    auto const whole = face_at(dangling, Geom::Point(25, 25));
    ASSERT_EQ(whole.status, Status::Ok);
    expect_area(region_area(whole.region), 5000.0);
}

// -------------------------------------------------------------------------

TEST(SmartFillPentagramTest, SelfIntersectingLoopIsAlsoACut)
{
    std::vector<Source> sources = {
        {pentagram_pv(100, 100, 50), Rule::NonZero},
    };

    double const inner_radius = 50.0 * std::cos(2.0 * M_PI / 5.0) / std::cos(M_PI / 5.0);
    double const pentagon_area = 2.5 * inner_radius * inner_radius * std::sin(2.0 * M_PI / 5.0);
    double const tip_area = (5.0 * 50.0 * inner_radius * std::sin(M_PI / 5.0) - pentagon_area) / 5.0;

    ASSERT_NEAR(inner_radius, 19.0983, 1e-3);
    ASSERT_NEAR(pentagon_area, 867.24, 0.01);
    ASSERT_NEAR(tip_area, 387.84, 0.01);

    auto const centre = face_at(sources, Geom::Point(100, 100));
    ASSERT_EQ(centre.status, Status::Ok);
    expect_area(region_area(centre.region), pentagon_area);

    auto const tip = face_at(sources, Geom::Point(100, 65));
    ASSERT_EQ(tip.status, Status::Ok);
    expect_area(region_area(tip.region), tip_area, 0.01);
}

// -------------------------------------------------------------------------

TEST(SmartFillBoundaryTest, BoundaryToleranceAndGuards)
{
    auto const sources = lens_sources();

    EXPECT_EQ(face_at(sources, Geom::Point(10, 25)).status, Status::OnBoundary);
    EXPECT_EQ(face_at(sources, Geom::Point(10.0005, 25)).status, Status::OnBoundary);
    EXPECT_EQ(face_at(sources, Geom::Point(10.01, 25)).status, Status::Ok);

    EXPECT_EQ(face_at(sources, Geom::Point(500, 500)).status, Status::NoRegion);

    std::span<Source const> const no_sources;
    EXPECT_EQ(face_at(no_sources, Geom::Point(0, 0)).status, Status::NoRegion);

    double const nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(face_at(sources, Geom::Point(nan, 25)).status, Status::NoRegion);
}

TEST(SmartFillBoundaryTest, CoversHonoursTheFillRule)
{
    auto const twice = read_pv("M 0,0 H 10 V 10 H 0 Z M 0,0 H 10 V 10 H 0 Z");
    EXPECT_TRUE(covers(twice, Rule::NonZero, Geom::Point(5, 5)));
    EXPECT_FALSE(covers(twice, Rule::EvenOdd, Geom::Point(5, 5)));
}

TEST(SmartFillBoundaryTest, SplitBoundaries)
{
    std::vector<Geom::Path> loops;
    std::vector<Geom::Path> lines;
    auto const reset = [&] {
        loops.clear();
        lines.clear();
    };

    split_boundaries(rect_pv(0, 0, 10, 10), loops, lines);
    EXPECT_EQ(loops.size(), 1u);
    EXPECT_EQ(lines.size(), 0u);

    reset();
    split_boundaries(read_pv("M 0,0 L 10,0"), loops, lines);
    EXPECT_EQ(loops.size(), 0u);
    EXPECT_EQ(lines.size(), 1u);

    // Closed but zero area is still a line.
    reset();
    split_boundaries(read_pv("M 0,0 L 10,0 Z"), loops, lines);
    EXPECT_EQ(loops.size(), 0u);
    EXPECT_EQ(lines.size(), 1u);

    // Last point equals the first without a Z command: a loop.
    reset();
    split_boundaries(read_pv("M 0,0 H 10 V 10 H 0 V 0"), loops, lines);
    EXPECT_EQ(loops.size(), 1u);
    EXPECT_EQ(lines.size(), 0u);
}

// -------------------------------------------------------------------------

TEST(SmartFillNormalisationTest, NonZeroAndEvenOddAgree)
{
    auto const check_region = [](Geom::PathVector const &region) {
        auto const bounds = region.boundsExact();
        ASSERT_TRUE(bounds);
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 4; ++j) {
                Geom::Point const q(bounds->min().x() + bounds->width() * (i + 0.5) / 5.0,
                                    bounds->min().y() + bounds->height() * (j + 0.5) / 4.0);
                EXPECT_EQ(covers(region, Rule::NonZero, q), covers(region, Rule::EvenOdd, q))
                    << "at " << q;
            }
        }
    };

    std::vector<Source> const donut = {
        {read_pv("M 70,10 H 110 V 50 H 70 Z M 80,20 V 40 H 100 V 20 Z"), Rule::EvenOdd},
    };
    auto const ring = face_at(donut, Geom::Point(75, 30));
    ASSERT_EQ(ring.status, Status::Ok);
    check_region(ring.region);

    std::vector<Source> const island = {
        {rect_pv(143, 23, 147, 27), Rule::NonZero},
        {rect_pv(140, 20, 150, 30), Rule::NonZero},
        {rect_pv(130, 10, 160, 40), Rule::NonZero},
    };
    auto const ring_with_hole = face_at(island, Geom::Point(141, 21));
    ASSERT_EQ(ring_with_hole.status, Status::Ok);
    check_region(ring_with_hole.region);
}

// -------------------------------------------------------------------------

TEST(SmartFillComponentTest, PicksOnlyTheRingContainingThePoint)
{
    auto const two_squares = read_pv("M 0,0 H 10 V 10 H 0 Z M 20,0 H 30 V 10 H 20 Z");

    auto const picked = component_at(two_squares, Geom::Point(5, 5));
    ASSERT_EQ(picked.size(), 1u);
    expect_area(std::abs(signed_area(picked[0])), 100.0, 1e-9);

    EXPECT_TRUE(component_at(two_squares, Geom::Point(15, 5)).empty());
}

// -------------------------------------------------------------------------

TEST(SmartFillOpenLinesTest, UnboundedFaceOffTheLivarotGrid)
{
    // Coordinates off livarot's 1/512 grid: the face open to infinity must
    // still be rejected, the centre cell still found.
    std::vector<Source> sources = {
        {read_pv("M 10.3,0.1 V 40.9"), Rule::NonZero},
        {read_pv("M 30.3,0.1 V 40.9"), Rule::NonZero},
        {read_pv("M 0.1,10.7 H 40.9"), Rule::NonZero},
        {read_pv("M 0.1,30.7 H 40.9"), Rule::NonZero},
    };

    auto const cell = face_at(sources, Geom::Point(20, 20));
    ASSERT_EQ(cell.status, Status::Ok);
    expect_area(region_area(cell.region), 400.0);
    expect_bbox(cell.region, 10.3, 10.7, 30.3, 30.7);

    EXPECT_EQ(face_at(sources, Geom::Point(5.1, 20)).status, Status::NoRegion);
    EXPECT_EQ(face_at(sources, Geom::Point(20, 35.3)).status, Status::NoRegion);

    // One rectangle off the grid: the outside is unbounded.
    std::vector<Source> rect = {{rect_pv(0.13, 0.37, 10.71, 9.93), Rule::NonZero}};
    EXPECT_EQ(face_at(rect, Geom::Point(11.5, 5)).status, Status::NoRegion);
}

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
