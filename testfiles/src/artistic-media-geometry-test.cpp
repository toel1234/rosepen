// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure artistic media model: the built-in library, the
 * shape-along-skeleton mapping, the flat-nib sweep, freehand fitting and the
 * list helpers.
 *
 * Every expected value below is hand-derived from the model in
 * artistic-media-geometry.h / artistic-media-library.h, not copied from
 * program output.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live_effects/artistic-media-geometry.h"
#include "live_effects/artistic-media-library.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <2geom/circle.h>
#include <2geom/elliptical-arc.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

using namespace Inkscape::LivePathEffect::ArtisticMedia;

namespace {

Geom::Path line_path(Geom::Point const &a, Geom::Point const &b)
{
    Geom::Path path(a);
    path.appendNew<Geom::LineSegment>(b);
    return path;
}

Geom::PathVector straight_skeleton(Geom::Point const &a, Geom::Point const &b)
{
    Geom::PathVector pv;
    pv.push_back(line_path(a, b));
    return pv;
}

/// Closed square (x, y)..(x+w, y+h) built with three segments plus close().
Geom::Path square_path(double x, double y, double w, double h)
{
    Geom::Path path(Geom::Point(x, y));
    path.appendNew<Geom::LineSegment>(Geom::Point(x + w, y));
    path.appendNew<Geom::LineSegment>(Geom::Point(x + w, y + h));
    path.appendNew<Geom::LineSegment>(Geom::Point(x, y + h));
    path.close();
    return path;
}

Geom::PathVector square_skeleton(double x, double y, double w, double h)
{
    Geom::PathVector pv;
    pv.push_back(square_path(x, y, w, h));
    return pv;
}

/// Signed area of one subpath (Green's theorem on the exact curves).
double subpath_area(Geom::Path const &path)
{
    Geom::Point centroid;
    double area = 0.0;
    Geom::centroid(path.toPwSb(), centroid, area);
    // Geom::centroid accumulates -(x dy - y dx), i.e. the negated shoelace
    // area; the hand-derived fractions are in shoelace orientation.
    return -area;
}

/// Sum of the signed subpath areas (holes subtract).
double total_area(Geom::PathVector const &pv)
{
    double area = 0.0;
    for (auto const &path : pv) {
        area += subpath_area(path);
    }
    return area;
}

bool pv_has_nan(Geom::PathVector const &pv)
{
    for (auto const &path : pv) {
        for (auto const &curve : path) {
            for (auto const &p : {curve.initialPoint(), curve.finalPoint()}) {
                if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
                    return true;
                }
            }
        }
    }
    return false;
}

Geom::PathVector unit_shape(char const *name, Mode mode = Mode::Preset)
{
    auto const *entry = find(mode, name);
    EXPECT_NE(entry, nullptr) << name;
    if (!entry) {
        return {};
    }
    auto parsed = parse_unit_shape(entry->unit_d);
    EXPECT_TRUE(parsed) << "unit data of " << name << " does not parse";
    return parsed.value_or(Geom::PathVector{});
}

std::vector<Geom::Point> points(std::initializer_list<Geom::Point> pts)
{
    return std::vector<Geom::Point>(pts);
}

} // namespace

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

TEST(ArtisticMediaLibrary, Contents)
{
    auto const entries = library();
    ASSERT_EQ(entries.size(), 11u);

    char const *const expected_names[11] = {
        "uniform", "taper-end", "taper-start", "taper-both", "lens", "teardrop", "swell",
        "dry-brush", "split-stroke", "stipple", "bristle",
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(std::string(entries[i].name), expected_names[i]) << "entry " << i;
        EXPECT_EQ(entries[i].mode, i < 7 ? Mode::Preset : Mode::Brush) << "entry " << i;
    }

    // Labels are the translated user-facing names (N_ strings, so the source text).
    EXPECT_EQ(std::string(entries[0].label), "Uniform");
    EXPECT_EQ(std::string(entries[4].label), "Lens");
    EXPECT_EQ(std::string(entries[10].label), "Bristle");
}

TEST(ArtisticMediaLibrary, ShapesParseAndCoverTheUnitBand)
{
    for (auto const &entry : library()) {
        auto const parsed = parse_unit_shape(entry.unit_d);
        ASSERT_TRUE(parsed) << entry.name;
        auto const bounds = parsed->boundsExact();
        ASSERT_TRUE(bounds) << entry.name;
        EXPECT_NEAR(bounds->left(), 0.0, 1e-9) << entry.name;
        EXPECT_NEAR(bounds->top(), -0.5, 1e-9) << entry.name;
        EXPECT_NEAR(bounds->right(), 1.0, 1e-9) << entry.name;
        EXPECT_NEAR(bounds->bottom(), 0.5, 1e-9) << entry.name;

        double const area = total_area(*parsed);
        EXPECT_NEAR(area, entry.area_fraction, 1e-6) << entry.name;

        // Outer subpaths wind positively; only the bristle has a hole.
        for (std::size_t i = 0; i < parsed->size(); ++i) {
            double const sub = subpath_area((*parsed)[i]);
            if (entry.mode == Mode::Brush && std::string(entry.name) == "bristle" && i == 1) {
                EXPECT_LT(sub, 0.0) << "the bristle hole winds negative";
                EXPECT_NEAR(sub, -0.16, 1e-6);
            } else {
                EXPECT_GT(sub, 0.0) << entry.name << " subpath " << i;
            }
        }
    }
}

TEST(ArtisticMediaLibrary, LookupAndModeKeys)
{
    for (auto const &entry : library()) {
        auto const *found = find(entry.mode, entry.name);
        ASSERT_NE(found, nullptr) << entry.name;
        EXPECT_STREQ(found->unit_d, entry.unit_d);
        EXPECT_STREQ(found->label, entry.label);
    }
    EXPECT_EQ(find(Mode::Preset, "dry-brush"), nullptr);  // a brush name, not a preset
    EXPECT_EQ(find(Mode::Brush, "lens"), nullptr);
    EXPECT_EQ(find(Mode::Preset, "no-such-shape"), nullptr);
    EXPECT_EQ(find(Mode::Calligraphic, "lens"), nullptr);
    EXPECT_EQ(find(Mode::Preset, ""), nullptr);

    EXPECT_STREQ(default_shape(Mode::Preset), "lens");
    EXPECT_STREQ(default_shape(Mode::Brush), "dry-brush");
    EXPECT_EQ(default_shape(Mode::Calligraphic), nullptr);

    EXPECT_EQ(parse_mode("preset"), Mode::Preset);
    EXPECT_EQ(parse_mode("brush"), Mode::Brush);
    EXPECT_EQ(parse_mode("calligraphic"), Mode::Calligraphic);
    EXPECT_EQ(parse_mode("Preset"), std::nullopt);
    EXPECT_EQ(parse_mode(""), std::nullopt);

    EXPECT_STREQ(mode_key(Mode::Preset), "preset");
    EXPECT_STREQ(mode_key(Mode::Brush), "brush");
    EXPECT_STREQ(mode_key(Mode::Calligraphic), "calligraphic");
}

TEST(ArtisticMediaLibrary, ParseUnitShapeRejects)
{
    EXPECT_EQ(parse_unit_shape(""), std::nullopt);
    EXPECT_EQ(parse_unit_shape("M 0,0 L 1,0"), std::nullopt);                 // open subpath
    EXPECT_EQ(parse_unit_shape("M 0,0 L 2,0 L 0,0.5 Z"), std::nullopt);       // u > 1
    EXPECT_EQ(parse_unit_shape("M 0,0 L 1,0.7 L 0,0.5 Z"), std::nullopt);     // v > 0.5
    EXPECT_EQ(parse_unit_shape("M 0,0 L 1,NaN L 0,0.5 Z"), std::nullopt);     // NaN text
    EXPECT_EQ(parse_unit_shape("not path data"), std::nullopt);

    // Control points may leave the band as long as the curve does not (lens).
    EXPECT_TRUE(parse_unit_shape("M 0,0 Q 0.5,-1 1,0 Q 0.5,1 0,0 Z"));
}

// ---------------------------------------------------------------------------
// Preset / brush mapping
// ---------------------------------------------------------------------------

TEST(ArtisticMediaMapAlong, EveryLibraryShapeOnAStraightSkeleton)
{
    auto const skeleton = straight_skeleton({0, 0}, {100, 0});
    for (auto const &entry : library()) {
        auto const shape = parse_unit_shape(entry.unit_d);
        ASSERT_TRUE(shape) << entry.name;

        auto const result = map_along(skeleton, *shape, 10.0);
        ASSERT_TRUE(result.valid) << entry.name;
        ASSERT_FALSE(result.path.empty()) << entry.name;
        EXPECT_FALSE(pv_has_nan(result.path)) << entry.name;

        auto const bounds = result.path.boundsExact();
        ASSERT_TRUE(bounds) << entry.name;
        EXPECT_NEAR(bounds->left(), 0.0, 1e-3) << entry.name;
        EXPECT_NEAR(bounds->top(), -5.0, 1e-3) << entry.name;
        EXPECT_NEAR(bounds->right(), 100.0, 1e-3) << entry.name;
        EXPECT_NEAR(bounds->bottom(), 5.0, 1e-3) << entry.name;

        // Area = area_fraction * length * width, within 0.5 %.
        double const expected = entry.area_fraction * 100.0 * 10.0;
        EXPECT_NEAR(std::fabs(total_area(result.path)), std::fabs(expected), std::fabs(expected) * 0.005)
            << entry.name;
    }
}

TEST(ArtisticMediaMapAlong, TaperEndIsWidestAtTheStart)
{
    auto const result = map_along(straight_skeleton({0, 0}, {100, 0}), unit_shape("taper-end"), 10.0);
    ASSERT_TRUE(result.valid);

    // Half width 5 * (1 - x/100): 4.9 at x = 2, 0.1 at x = 98.
    EXPECT_NE(result.path.winding(Geom::Point(2, 4)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(98, 4)), 0);
    EXPECT_NE(result.path.winding(Geom::Point(2, -4)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(98, -4)), 0);
}

TEST(ArtisticMediaMapAlong, TaperRunsOnceAlongAPolyline)
{
    // (0,0) -> (50,0) -> (50,50), L = 100: one taper over the whole subpath,
    // half width 5 * (1 - s/100) at arc length s, not one taper per edge.
    Geom::PathVector skeleton;
    skeleton.push_back(line_path({0, 0}, {50, 0}));
    skeleton.back().appendNew<Geom::LineSegment>(Geom::Point(50, 50));

    auto const result = map_along(skeleton, unit_shape("taper-end"), 10.0);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.path.size(), 1u);
    EXPECT_FALSE(pv_has_nan(result.path));

    // s = 25: half width 3.75.
    EXPECT_NE(result.path.winding(Geom::Point(25, 3)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(25, 4.5)), 0);
    // s = 60 on the second edge: half width 2 (a per-edge copy would give 4).
    EXPECT_NE(result.path.winding(Geom::Point(51.5, 10)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(53, 10)), 0);
    // s = 98: half width 0.1.
    EXPECT_EQ(result.path.winding(Geom::Point(51, 48)), 0);
}

TEST(ArtisticMediaMapAlong, PositiveVFollowsTheSkeletonNormal)
{
    // Custom asymmetric shape: v in [0, 0.5] only.
    auto const shape = parse_unit_shape("M 0,0 L 1,0 L 1,0.5 L 0,0.5 Z");
    ASSERT_TRUE(shape);

    // Travel towards +x: N = (0, +1), so v > 0 lands on the +y side.
    auto const forward = map_along(straight_skeleton({0, 0}, {100, 0}), *shape, 10.0);
    ASSERT_TRUE(forward.valid);
    EXPECT_NE(forward.path.winding(Geom::Point(50, 2.5)), 0);
    EXPECT_EQ(forward.path.winding(Geom::Point(50, -2.5)), 0);

    // Reversed skeleton: N = (0, -1), so the same shape lands on -y.
    auto const backward = map_along(straight_skeleton({100, 0}, {0, 0}), *shape, 10.0);
    ASSERT_TRUE(backward.valid);
    EXPECT_NE(backward.path.winding(Geom::Point(50, -2.5)), 0);
    EXPECT_EQ(backward.path.winding(Geom::Point(50, 2.5)), 0);
}

TEST(ArtisticMediaMapAlong, QuarterCircleArea)
{
    // Quarter circle of radius 50: length pi * 50 / 2 = 78.5398.
    Geom::Path path{Geom::Point(50, 0)};
    path.appendNew<Geom::EllipticalArc>(50.0, 50.0, 0.0, false, true, Geom::Point(0, 50));
    Geom::PathVector skeleton;
    skeleton.push_back(path);

    auto const result = map_along(skeleton, unit_shape("uniform"), 10.0);
    ASSERT_TRUE(result.valid);

    double const expected = M_PI * 50.0 / 2.0 * 10.0; // 785.398
    EXPECT_NEAR(std::fabs(total_area(result.path)), expected, expected * 0.01);
    EXPECT_FALSE(pv_has_nan(result.path));
}

TEST(ArtisticMediaMapAlong, ClosedSquareSkeletonKeepsTheFullWidthOutside)
{
    auto const result = map_along(square_skeleton(0, 0, 40, 30), unit_shape("uniform"), 4.0);
    ASSERT_TRUE(result.valid);

    // ONE copy of the shape, u running once around (L = 140): a single closed
    // ring whose outer side is bevelled at the corners and whose inner side
    // overlaps itself there (winding 2, still filled under nonzero).
    ASSERT_EQ(result.path.size(), 1u);
    EXPECT_TRUE(result.path[0].closed());
    EXPECT_FALSE(pv_has_nan(result.path));

    // Half the width (2) on both sides of every edge, a hole in the middle.
    for (auto const &inside : {Geom::Point(20, 1.5), Geom::Point(20, -1.5), Geom::Point(41.5, 15),
                               Geom::Point(38.5, 15), Geom::Point(20, 31.5), Geom::Point(-1.5, 15)}) {
        EXPECT_NE(result.path.winding(inside), 0) << inside;
    }
    for (auto const &outside : {Geom::Point(20, -2.5), Geom::Point(42.5, 15), Geom::Point(20, 32.5),
                                Geom::Point(-2.5, 15), Geom::Point(20, 15)}) {
        EXPECT_EQ(result.path.winding(outside), 0) << outside;
    }
    // Corner (40, 0): bevel (40,-2)-(42,0) outside the turn, overlap inside.
    EXPECT_NE(result.path.winding(Geom::Point(40.5, -0.5)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(41.8, -1.8)), 0);
    EXPECT_NE(result.path.winding(Geom::Point(39, 1)), 0);
    // Seam at the start point (0, 0): the u = 0 and u = 1 ends of the one
    // copy meet there flat, so the outer corner square is not covered.
    EXPECT_EQ(result.path.winding(Geom::Point(-1, -1)), 0);

    // The stroke reaches half the width outside every edge.
    auto const bounds = result.path.boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), -2.0, 0.05);
    EXPECT_NEAR(bounds->top(), -2.0, 0.05);
    EXPECT_NEAR(bounds->right(), 42.0, 0.05);
    EXPECT_NEAR(bounds->bottom(), 32.0, 0.05);
}

TEST(ArtisticMediaMapAlong, EverySkeletonSubpathGetsEveryShapeSubpath)
{
    // Two-subpath skeleton, four-subpath brush: eight output subpaths.
    Geom::PathVector skeleton;
    skeleton.push_back(line_path({0, 0}, {50, 0}));
    skeleton.push_back(line_path({0, 20}, {50, 20}));

    auto const result = map_along(skeleton, unit_shape("stipple", Mode::Brush), 10.0);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.path.size(), 8u);
}

TEST(ArtisticMediaMapAlong, DegenerateInputIsInvalid)
{
    // No usable subpath.
    Geom::PathVector single_point;
    single_point.push_back(Geom::Path(Geom::Point(5, 5)));
    auto const no_skeleton = map_along(single_point, unit_shape("uniform"), 10.0);
    EXPECT_FALSE(no_skeleton.valid);
    EXPECT_TRUE(no_skeleton.path.empty());

    // Zero width.
    auto const zero_width = map_along(straight_skeleton({0, 0}, {100, 0}), unit_shape("uniform"), 0.0);
    EXPECT_FALSE(zero_width.valid);
    EXPECT_TRUE(zero_width.path.empty());

    // Empty inputs.
    EXPECT_FALSE(map_along({}, unit_shape("uniform"), 10.0).valid);
    EXPECT_FALSE(map_along(straight_skeleton({0, 0}, {100, 0}), {}, 10.0).valid);
}

// ---------------------------------------------------------------------------
// Calligraphic sweep
// ---------------------------------------------------------------------------

TEST(ArtisticMediaCalligraphic, VerticalNibOnAHorizontalLine)
{
    auto const skeleton = straight_skeleton({20, 30}, {120, 30});

    // theta = 90: nib straight across the line, full width.
    auto const result = calligraphic(skeleton, 10.0, 90.0);
    ASSERT_TRUE(result.valid);
    auto const bounds = result.path.boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 20.0, 1e-3);
    EXPECT_NEAR(bounds->top(), 25.0, 1e-3);
    EXPECT_NEAR(bounds->right(), 120.0, 1e-3);
    EXPECT_NEAR(bounds->bottom(), 35.0, 1e-3);
    EXPECT_NEAR(std::fabs(total_area(result.path)), 1000.0, 5.0);
}

TEST(ArtisticMediaCalligraphic, FortyFiveDegrees)
{
    // d = (cos 45, -sin 45): the sweep is a parallelogram of area L * W * sin 45.
    auto const result = calligraphic(straight_skeleton({20, 30}, {120, 30}), 10.0, 45.0);
    ASSERT_TRUE(result.valid);

    double const half = 5.0 / std::sqrt(2.0); // 3.5355
    auto const bounds = result.path.boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 20.0 - half, 1e-3);
    EXPECT_NEAR(bounds->top(), 30.0 - half, 1e-3);
    EXPECT_NEAR(bounds->right(), 120.0 + half, 1e-3);
    EXPECT_NEAR(bounds->bottom(), 30.0 + half, 1e-3);

    double const expected = 100.0 * 10.0 * std::sin(M_PI / 4.0); // 707.107
    EXPECT_NEAR(std::fabs(total_area(result.path)), expected, expected * 0.001);

    EXPECT_NE(result.path.winding(Geom::Point(22, 30)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(18.5, 30)), 0);
    EXPECT_NE(result.path.winding(Geom::Point(118.5, 30)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(121.5, 30)), 0);
}

TEST(ArtisticMediaCalligraphic, NibParallelToTheSkeletonSweepsNothing)
{
    // d = (1, 0) along the line: zero-area sweep, valid but empty.
    auto const result = calligraphic(straight_skeleton({20, 30}, {120, 30}), 10.0, 0.0);
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.path.empty());
}

TEST(ArtisticMediaCalligraphic, OneHundredThirtyFiveDegreesIsTheMirrorOfFortyFive)
{
    auto const result = calligraphic(straight_skeleton({20, 30}, {120, 30}), 10.0, 135.0);
    ASSERT_TRUE(result.valid);
    EXPECT_NE(result.path.winding(Geom::Point(20, 27)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(20, 33)), 0);
}

TEST(ArtisticMediaCalligraphic, ClosedSquareGivesTwoRuns)
{
    // Edges 1-2 have cross * d < 0, edges 3-4 > 0: exactly two runs.
    auto const result = calligraphic(square_skeleton(40, 60, 40, 30), 10.0, 45.0);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.path.size(), 2u);

    double const half = 5.0 / std::sqrt(2.0);
    auto const bounds = result.path.boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 40.0 - half, 1e-3);
    EXPECT_NEAR(bounds->top(), 60.0 - half, 1e-3);
    EXPECT_NEAR(bounds->right(), 80.0 + half, 1e-3);
    EXPECT_NEAR(bounds->bottom(), 90.0 + half, 1e-3);

    EXPECT_EQ(result.path.winding(Geom::Point(60, 75)), 0);   // hollow centre
    EXPECT_EQ(result.path.winding(Geom::Point(45.5, 75)), 0); // bands are 10 wide along the nib
    EXPECT_NE(result.path.winding(Geom::Point(60, 61.5)), 0);
    EXPECT_NE(result.path.winding(Geom::Point(40, 75)), 0);
    EXPECT_NE(result.path.winding(Geom::Point(60, 90)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(60, 65.5)), 0);

    // Every run polygon winds the same way.
    double const first = subpath_area(result.path[0]);
    double const second = subpath_area(result.path[1]);
    EXPECT_GT(first, 0.0);
    EXPECT_GT(second, 0.0);
}

TEST(ArtisticMediaCalligraphic, CircleWithAHorizontalNib)
{
    Geom::Path circle{Geom::Circle(Geom::Point(100, 50), 20)};
    Geom::PathVector skeleton;
    skeleton.push_back(circle);

    auto const result = calligraphic(skeleton, 10.0, 0.0);
    ASSERT_TRUE(result.valid);
    EXPECT_FALSE(pv_has_nan(result.path));

    // A horizontal nib offsets the circle by +-5 in x only: the sweep spans
    // x 75..125 (radius 20 plus half the nib) and y 30..70.
    auto const bounds = result.path.boundsExact();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 75.0, 0.05);
    EXPECT_NEAR(bounds->top(), 30.0, 0.05);
    EXPECT_NEAR(bounds->right(), 125.0, 0.05);
    EXPECT_NEAR(bounds->bottom(), 70.0, 0.05);

    EXPECT_EQ(result.path.winding(Geom::Point(100, 50)), 0); // W < 2R: the centre stays empty
    EXPECT_NE(result.path.winding(Geom::Point(120, 50)), 0);
    EXPECT_EQ(result.path.winding(Geom::Point(100, 28)), 0);
}

TEST(ArtisticMediaCalligraphic, SCurveRunsAreConsistentlyOriented)
{
    Geom::Path path{Geom::Point(0, 0)};
    path.appendNew<Geom::CubicBezier>(Geom::Point(50, -60), Geom::Point(50, 60), Geom::Point(100, 0));
    Geom::PathVector skeleton;
    skeleton.push_back(path);

    // With a horizontal nib the tangent is parallel to it at t = 0.5
    // (y(t) = 180 t (1-t) (2t-1)), so the sweep splits into two runs.
    auto const result = calligraphic(skeleton, 10.0, 0.0);
    ASSERT_TRUE(result.valid);
    EXPECT_FALSE(pv_has_nan(result.path));
    ASSERT_GE(result.path.size(), 2u);

    bool saw_positive = false;
    bool saw_negative = false;
    for (auto const &subpath : result.path) {
        double const area = subpath_area(subpath);
        ASSERT_NE(area, 0.0);
        (area > 0.0 ? saw_positive : saw_negative) = true;
    }
    EXPECT_FALSE(saw_positive && saw_negative) << "run polygons must share one orientation";

    // A vertical nib never becomes parallel to this curve (dx never vanishes),
    // so it sweeps one run; it must still be finite.
    auto const vertical = calligraphic(skeleton, 10.0, 90.0);
    ASSERT_TRUE(vertical.valid);
    EXPECT_FALSE(pv_has_nan(vertical.path));
    EXPECT_EQ(vertical.path.size(), 1u);
}

TEST(ArtisticMediaCalligraphic, DegenerateInput)
{
    Geom::PathVector single_point;
    single_point.push_back(Geom::Path(Geom::Point(5, 5)));
    EXPECT_FALSE(calligraphic(single_point, 10.0, 30.0).valid);

    EXPECT_FALSE(calligraphic(straight_skeleton({0, 0}, {100, 0}), 0.0, 30.0).valid);
    EXPECT_FALSE(calligraphic({}, 10.0, 30.0).valid);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

TEST(ArtisticMediaHelpers, NormalizeAngle)
{
    EXPECT_DOUBLE_EQ(normalize_angle(30.0), 30.0);
    EXPECT_DOUBLE_EQ(normalize_angle(-45.0), 135.0);
    EXPECT_DOUBLE_EQ(normalize_angle(180.0), 0.0);
    EXPECT_DOUBLE_EQ(normalize_angle(540.0), 0.0);
    EXPECT_DOUBLE_EQ(normalize_angle(200.0), 20.0);
    EXPECT_FALSE(std::signbit(normalize_angle(-0.0)));
    EXPECT_DOUBLE_EQ(normalize_angle(std::numeric_limits<double>::quiet_NaN()), 0.0);
    EXPECT_DOUBLE_EQ(normalize_angle(std::numeric_limits<double>::infinity()), 0.0);
}

TEST(ArtisticMediaHelpers, FitStrokePolyline)
{
    auto const pts = points({{20, 50}, {70, 50}, {120, 50}});
    auto const fitted = fit_stroke(pts, 0.0);
    ASSERT_TRUE(fitted);
    ASSERT_EQ(fitted->size(), 1u);
    EXPECT_EQ((*fitted)[0].size_open(), 2u); // two line segments
    EXPECT_EQ(node_count(*fitted), 3u);
    EXPECT_NEAR(skeleton_length(*fitted), 100.0, 1e-9);
}

TEST(ArtisticMediaHelpers, FitStrokeDropsDuplicates)
{
    auto const pts = points({{20, 50}, {20.0000000001, 50.0}, {70, 50}});
    auto const fitted = fit_stroke(pts, 0.0);
    ASSERT_TRUE(fitted);
    EXPECT_EQ((*fitted)[0].size_open(), 1u); // the near-duplicate is gone
    EXPECT_EQ(node_count(*fitted), 2u);

    auto const single = points({{20, 50}});
    EXPECT_EQ(fit_stroke(single, 0.0), std::nullopt);
}

TEST(ArtisticMediaHelpers, FitStrokeSmoothsNoise)
{
    std::vector<Geom::Point> pts;
    for (int i = 0; i < 50; ++i) {
        double const x = 20.0 + 100.0 * i / 49.0;
        double const y = 50.0 + (i % 2 == 0 ? 0.01 : -0.01);
        pts.push_back(Geom::Point(x, y));
    }
    auto const fitted = fit_stroke(pts, 5.0);
    ASSERT_TRUE(fitted);
    ASSERT_EQ(fitted->size(), 1u);
    EXPECT_LE((*fitted)[0].size_open(), 2u);
    // Endpoints stay exact.
    EXPECT_NEAR((*fitted)[0].initialPoint().x(), pts.front().x(), 1e-9);
    EXPECT_NEAR((*fitted)[0].initialPoint().y(), pts.front().y(), 1e-9);
    EXPECT_NEAR((*fitted)[0].finalPoint().x(), pts.back().x(), 1e-9);
    EXPECT_NEAR((*fitted)[0].finalPoint().y(), pts.back().y(), 1e-9);

    EXPECT_DOUBLE_EQ(smoothing_to_tolerance_mm(50.0), 2.5);
    EXPECT_DOUBLE_EQ(smoothing_to_tolerance_mm(-10.0), 0.0);
    EXPECT_DOUBLE_EQ(smoothing_to_tolerance_mm(100.0), 5.0);
    EXPECT_DOUBLE_EQ(smoothing_to_tolerance_mm(1000.0), 5.0);
}

TEST(ArtisticMediaHelpers, NodeCountAndLength)
{
    auto const square = square_skeleton(0, 0, 40, 30);
    EXPECT_EQ(node_count(square), 4u);
    EXPECT_NEAR(skeleton_length(square), 140.0, 1e-9);

    auto const polyline = straight_skeleton({0, 0}, {4, 0});
    EXPECT_EQ(node_count(polyline), 2u);
    EXPECT_NEAR(skeleton_length(polyline), 4.0, 1e-9);

    Geom::PathVector three;
    Geom::Path path{Geom::Point(0, 0)};
    path.appendNew<Geom::LineSegment>(Geom::Point(3, 0));
    path.appendNew<Geom::LineSegment>(Geom::Point(3, 4));
    three.push_back(path);
    EXPECT_EQ(node_count(three), 3u);
    EXPECT_NEAR(skeleton_length(three), 7.0, 1e-9);
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
