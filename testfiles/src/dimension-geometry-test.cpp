// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure dimension model: anchor parsing/resolution,
 * per-kind geometry and label formatting.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live_effects/dimension-geometry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <locale>
#include <optional>
#include <string>
#include <vector>

#include <2geom/pathvector.h>

using namespace Inkscape::LivePathEffect::Dimension;

namespace {

Style default_style()
{
    Style style;
    style.offset = 5.0;
    style.arrow_length = 2.5;
    style.ext_gap = 1.0;
    style.ext_overshoot = 2.0;
    style.text_position = TextPosition::Above;
    style.text_horizontal = false;
    style.label_gap = 1.0;
    return style;
}

/// Closed rectangle (0,0)-(10,8), built as three segments + close().
Geom::PathVector closed_rect_pv()
{
    Geom::Path path;
    path.start(Geom::Point(0, 0));
    path.appendNew<Geom::LineSegment>(Geom::Point(10, 0));
    path.appendNew<Geom::LineSegment>(Geom::Point(10, 8));
    path.appendNew<Geom::LineSegment>(Geom::Point(0, 8));
    path.close();
    Geom::PathVector pv;
    pv.push_back(std::move(path));
    return pv;
}

/// Closed rectangle whose four sides are all real segments.
Geom::PathVector closed_rect4_pv()
{
    Geom::Path path;
    path.start(Geom::Point(0, 0));
    path.appendNew<Geom::LineSegment>(Geom::Point(10, 0));
    path.appendNew<Geom::LineSegment>(Geom::Point(10, 8));
    path.appendNew<Geom::LineSegment>(Geom::Point(0, 8));
    path.appendNew<Geom::LineSegment>(Geom::Point(0, 0));
    path.close();
    Geom::PathVector pv;
    pv.push_back(std::move(path));
    return pv;
}

/// Open polyline (0,0)-(4,0)-(4,3), plus a second subpath (10,10)-(12,10).
Geom::PathVector open_polyline_pv()
{
    Geom::Path path;
    path.start(Geom::Point(0, 0));
    path.appendNew<Geom::LineSegment>(Geom::Point(4, 0));
    path.appendNew<Geom::LineSegment>(Geom::Point(4, 3));
    Geom::Path path2;
    path2.start(Geom::Point(10, 10));
    path2.appendNew<Geom::LineSegment>(Geom::Point(12, 10));
    Geom::PathVector pv;
    pv.push_back(std::move(path));
    pv.push_back(std::move(path2));
    return pv;
}

/// 1-, 2- and 3-point spans for build().
std::vector<Geom::Point> pts1(Geom::Point const &a)
{
    return {a};
}

std::vector<Geom::Point> pts2(Geom::Point const &a, Geom::Point const &b)
{
    return {a, b};
}

std::vector<Geom::Point> pts3(Geom::Point const &a, Geom::Point const &b, Geom::Point const &c)
{
    return {a, b, c};
}

/// Sampled shoelace area of one subpath.
double subpath_area(Geom::Path const &path, int samples = 24)
{
    if (path.empty()) {
        return 0.0;
    }
    std::vector<Geom::Point> pts;
    pts.push_back(path.initialPoint());
    for (auto const &curve : path) {
        for (int i = 1; i <= samples; ++i) {
            pts.push_back(curve.pointAt(static_cast<double>(i) / samples));
        }
    }
    double area = 0.0;
    for (size_t i = 0; i < pts.size(); ++i) {
        Geom::Point const &p = pts[i];
        Geom::Point const &q = pts[(i + 1) % pts.size()];
        area += p.x() * q.y() - q.x() * p.y();
    }
    return 0.5 * area;
}

} // namespace

// -------------------------------------------------------------------------

TEST(DimensionKindTest, KeysRoundTrip)
{
    for (Kind kind : {Kind::Aligned, Kind::Horizontal, Kind::Vertical, Kind::Angular, Kind::Segment}) {
        auto const parsed = parse_kind(kind_key(kind));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, kind);
    }
}

TEST(DimensionKindTest, AutoIsNotAStoredKind)
{
    EXPECT_FALSE(parse_kind("auto").has_value());
    EXPECT_FALSE(parse_kind("").has_value());
    EXPECT_FALSE(parse_kind("Aligned").has_value()); // case-sensitive keys
    EXPECT_FALSE(parse_kind("parallel").has_value());
}

TEST(DimensionKindTest, ResolveAuto)
{
    EXPECT_EQ(resolve_auto(Geom::Point(0, 0), Geom::Point(10, 3)), Kind::Horizontal);
    EXPECT_EQ(resolve_auto(Geom::Point(0, 0), Geom::Point(3, 10)), Kind::Vertical);
    EXPECT_EQ(resolve_auto(Geom::Point(0, 0), Geom::Point(10, -10)), Kind::Horizontal); // tie -> horizontal
    EXPECT_EQ(resolve_auto(Geom::Point(5, 5), Geom::Point(5, 9)), Kind::Vertical);
}

// -------------------------------------------------------------------------

TEST(DimensionAnchorTest, BBoxRoundTrip)
{
    char const *const names[9] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};
    for (int i = 0; i < 9; ++i) {
        std::string const spec = std::string("bbox:") + names[i];
        auto const anchor = parse_anchor(spec);
        ASSERT_TRUE(anchor.has_value()) << spec;
        EXPECT_EQ(anchor->type, Anchor::BBox);
        EXPECT_EQ(anchor->handle, i);
        EXPECT_EQ(format_anchor(*anchor), spec);
    }
}

TEST(DimensionAnchorTest, NodeRoundTrip)
{
    for (int k : {0, 1, 42, 123456}) {
        std::string const spec = "node:" + std::to_string(k);
        auto const anchor = parse_anchor(spec);
        ASSERT_TRUE(anchor.has_value()) << spec;
        EXPECT_EQ(anchor->type, Anchor::Node);
        EXPECT_EQ(anchor->node, k);
        EXPECT_EQ(format_anchor(*anchor), spec);
    }
}

TEST(DimensionAnchorTest, RelRoundTrip)
{
    struct Case {
        double u, v;
    };
    for (Case const &c : std::vector<Case>{{0.0, 0.0}, {0.25, 0.75}, {1.0, 1.0}, {-0.5, 0.25}, {0.1, 1e-7}, {1.5, 0.0}}) {
        Anchor anchor;
        anchor.type = Anchor::Rel;
        anchor.u = c.u;
        anchor.v = c.v;
        auto const reparsed = parse_anchor(format_anchor(anchor));
        ASSERT_TRUE(reparsed.has_value()) << format_anchor(anchor);
        ASSERT_EQ(reparsed->type, Anchor::Rel);
        // Exact binary round trip (g_ascii_dtostr preserves the double).
        EXPECT_EQ(reparsed->u, c.u);
        EXPECT_EQ(reparsed->v, c.v);
    }

    // Canonical spelling without trailing zeroes.
    auto const anchor = parse_anchor("rel:0.25/0.75");
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(format_anchor(*anchor), "rel:0.25/0.75");
}

TEST(DimensionAnchorTest, Rejects)
{
    char const *const bad[] = {
        "",              "bbox:",        "bbox:xx",     "bbox:tlx",    "bbox:TL",
        "node:-1",       "node:",        "node:abc",    "node:1x",     "node:+1",
        "rel:0.5",       "rel:nan/0",    "rel:inf/0",   "rel:-inf/0",  "rel:0.5/0.5/0.5",
        "rel:/0.5",      "rel:0.5/",     "rel:abc/0",   "rel:0/abc",   "foo:1",
    };
    for (char const *spec : bad) {
        EXPECT_FALSE(parse_anchor(spec).has_value()) << "spec: " << spec;
    }
}

// -------------------------------------------------------------------------

TEST(DimensionResolveTest, BBoxHandles)
{
    // Rect: x 10..50, y 20..90.
    Geom::OptRect const bbox(Geom::Rect(Geom::Point(10, 20), Geom::Point(50, 90)));

    struct Case {
        char const *spec;
        double x, y;
    };
    Case const cases[9] = {
        {"bbox:tl", 10, 20}, {"bbox:t", 30, 20},  {"bbox:tr", 50, 20},
        {"bbox:l", 10, 55},  {"bbox:c", 30, 55},  {"bbox:r", 50, 55},
        {"bbox:bl", 10, 90}, {"bbox:b", 30, 90},  {"bbox:br", 50, 90},
    };
    for (Case const &c : cases) {
        auto const parsed = parse_anchor(c.spec);
        ASSERT_TRUE(parsed.has_value()) << c.spec;
        auto const point = resolve_anchor(*parsed, bbox, {});
        ASSERT_TRUE(point.has_value()) << c.spec;
        EXPECT_DOUBLE_EQ(point->x(), c.x) << c.spec;
        EXPECT_DOUBLE_EQ(point->y(), c.y) << c.spec;
    }
}

TEST(DimensionResolveTest, RelPoint)
{
    Geom::OptRect const bbox(Geom::Rect(Geom::Point(10, 20), Geom::Point(50, 90)));
    auto const anchor = parse_anchor("rel:0.25/0.75");
    ASSERT_TRUE(anchor.has_value());
    auto const point = resolve_anchor(*anchor, bbox, {});
    ASSERT_TRUE(point.has_value());
    EXPECT_DOUBLE_EQ(point->x(), 20.0);  // 10 + 0.25 * 40
    EXPECT_DOUBLE_EQ(point->y(), 72.5);  // 20 + 0.75 * 70
}

TEST(DimensionResolveTest, NodeClosedRectHasNoDuplicateClosingNode)
{
    auto const curve = closed_rect_pv();
    auto const anchor = parse_anchor("node:0");
    ASSERT_TRUE(anchor.has_value());

    struct Case {
        int k;
        double x, y;
    };
    Case const cases[4] = {{0, 0, 0}, {1, 10, 0}, {2, 10, 8}, {3, 0, 8}};
    for (Case const &c : cases) {
        Anchor a;
        a.type = Anchor::Node;
        a.node = c.k;
        auto const point = resolve_anchor(a, Geom::OptRect(), curve);
        ASSERT_TRUE(point.has_value()) << "node:" << c.k;
        EXPECT_DOUBLE_EQ(point->x(), c.x) << "node:" << c.k;
        EXPECT_DOUBLE_EQ(point->y(), c.y) << "node:" << c.k;
    }

    // 4 nodes only; the wrap-around point is not a separate node.
    Anchor out_of_range;
    out_of_range.type = Anchor::Node;
    out_of_range.node = 4;
    EXPECT_FALSE(resolve_anchor(out_of_range, Geom::OptRect(), curve).has_value());

    // Same node set when all four sides are real segments.
    auto const curve4 = closed_rect4_pv();
    auto const p0 = resolve_anchor(*parse_anchor("node:0"), Geom::OptRect(), curve4);
    auto const p3 = resolve_anchor(*parse_anchor("node:3"), Geom::OptRect(), curve4);
    ASSERT_TRUE(p0.has_value());
    ASSERT_TRUE(p3.has_value());
    EXPECT_DOUBLE_EQ(p0->x(), 0.0);
    EXPECT_DOUBLE_EQ(p3->y(), 8.0);
    EXPECT_FALSE(resolve_anchor(out_of_range, Geom::OptRect(), curve4).has_value());
}

TEST(DimensionResolveTest, NodeOpenPolylineAndSubpathOrder)
{
    auto const curve = open_polyline_pv();
    struct Case {
        int k;
        double x, y;
    };
    Case const cases[5] = {{0, 0, 0}, {1, 4, 0}, {2, 4, 3}, {3, 10, 10}, {4, 12, 10}};
    for (Case const &c : cases) {
        Anchor a;
        a.type = Anchor::Node;
        a.node = c.k;
        auto const point = resolve_anchor(a, Geom::OptRect(), curve);
        ASSERT_TRUE(point.has_value()) << "node:" << c.k;
        EXPECT_DOUBLE_EQ(point->x(), c.x) << "node:" << c.k;
        EXPECT_DOUBLE_EQ(point->y(), c.y) << "node:" << c.k;
    }

    Anchor out_of_range;
    out_of_range.type = Anchor::Node;
    out_of_range.node = 5;
    EXPECT_FALSE(resolve_anchor(out_of_range, Geom::OptRect(), curve).has_value());
}

TEST(DimensionResolveTest, MissingBBox)
{
    Geom::OptRect const empty;
    auto const bbox = parse_anchor("bbox:c");
    auto const rel = parse_anchor("rel:0.5/0.5");
    ASSERT_TRUE(bbox.has_value());
    ASSERT_TRUE(rel.has_value());
    EXPECT_FALSE(resolve_anchor(*bbox, empty, {}).has_value());
    EXPECT_FALSE(resolve_anchor(*rel, empty, {}).has_value());

    // Node anchors do not need a bbox.
    auto const node = parse_anchor("node:1");
    ASSERT_TRUE(node.has_value());
    auto const point = resolve_anchor(*node, empty, closed_rect_pv());
    ASSERT_TRUE(point.has_value());
    EXPECT_DOUBLE_EQ(point->x(), 10.0);
}

// -------------------------------------------------------------------------

TEST(DimensionBuildTest, Aligned345)
{
    Style const style = default_style();
    auto const result = build(Kind::Aligned, pts2(Geom::Point(10, 40), Geom::Point(13, 44)), style);
    ASSERT_TRUE(result.valid);
    EXPECT_DOUBLE_EQ(result.value_uu_or_deg, 5.0);

    // 5 subpaths: 2 extension lines, dimension line, 2 arrows.
    ASSERT_EQ(result.path.size(), 5u);

    // Left of (0.6, 0.8) in y-down is (0.8, -0.6); dimension line at distance 5.
    Geom::Point const d1(14, 37);
    Geom::Point const d2(17, 41);
    EXPECT_EQ(result.path[2].initialPoint(), d1);
    EXPECT_EQ(result.path[2].finalPoint(), d2);

    // Extension lines: from 1 before the line to 2 past it.
    Geom::Point const side(0.8, -0.6);
    EXPECT_TRUE(Geom::are_near(result.path[0].initialPoint(), Geom::Point(10, 40) + side * 1.0));
    EXPECT_TRUE(Geom::are_near(result.path[0].finalPoint(), Geom::Point(10, 40) + side * 7.0));
    EXPECT_TRUE(Geom::are_near(result.path[1].initialPoint(), Geom::Point(13, 44) + side * 1.0));
    EXPECT_TRUE(Geom::are_near(result.path[1].finalPoint(), Geom::Point(13, 44) + side * 7.0));

    // Label above the line midpoint.
    EXPECT_TRUE(Geom::are_near(result.label_pos, (d1 + d2) / 2.0 + side * 1.0));
    // Direction (0.6, 0.8) in y-down screen coords reads as +53.13 degrees.
    EXPECT_NEAR(result.label_angle_deg, 53.13010235415598, 1e-9);

    // Negative offset flips the dimension line to the other side.
    Style flipped = style;
    flipped.offset = -5.0;
    auto const flipped_result = build(Kind::Aligned, pts2(Geom::Point(10, 40), Geom::Point(13, 44)), flipped);
    ASSERT_TRUE(flipped_result.valid);
    EXPECT_EQ(flipped_result.path[2].initialPoint(), Geom::Point(6, 43));
    EXPECT_EQ(flipped_result.path[2].finalPoint(), Geom::Point(9, 47));
}

TEST(DimensionBuildTest, Horizontal)
{
    Style const style = default_style();
    auto const result = build(Kind::Horizontal, pts2(Geom::Point(10, 40), Geom::Point(80, 60)), style);
    ASSERT_TRUE(result.valid);
    EXPECT_DOUBLE_EQ(result.value_uu_or_deg, 70.0);

    // Dimension line at y = min(y1, y2) - offset.
    EXPECT_EQ(result.path[2].initialPoint(), Geom::Point(10, 35));
    EXPECT_EQ(result.path[2].finalPoint(), Geom::Point(80, 35));

    // Extension lines rise from the points toward the line, 2 past it.
    EXPECT_TRUE(Geom::are_near(result.path[0].initialPoint(), Geom::Point(10, 39)));
    EXPECT_TRUE(Geom::are_near(result.path[0].finalPoint(), Geom::Point(10, 33)));
    EXPECT_TRUE(Geom::are_near(result.path[1].initialPoint(), Geom::Point(80, 59)));
    EXPECT_TRUE(Geom::are_near(result.path[1].finalPoint(), Geom::Point(80, 33)));

    // Label above the line, horizontal text angle.
    EXPECT_TRUE(Geom::are_near(result.label_pos, Geom::Point(45, 34)));
    EXPECT_DOUBLE_EQ(result.label_angle_deg, 0.0);

    // Negative offset puts the line below the lower point.
    Style flipped = style;
    flipped.offset = -5.0;
    auto const flipped_result = build(Kind::Horizontal, pts2(Geom::Point(10, 40), Geom::Point(80, 60)), flipped);
    ASSERT_TRUE(flipped_result.valid);
    EXPECT_EQ(flipped_result.path[2].initialPoint(), Geom::Point(10, 65)); // 60 + |offset|
    EXPECT_EQ(flipped_result.path[2].finalPoint(), Geom::Point(80, 65));
}

TEST(DimensionBuildTest, Vertical)
{
    Style const style = default_style();
    auto const result = build(Kind::Vertical, pts2(Geom::Point(10, 40), Geom::Point(30, 60)), style);
    ASSERT_TRUE(result.valid);
    EXPECT_DOUBLE_EQ(result.value_uu_or_deg, 20.0);

    // Dimension line at x = max(x1, x2) + offset.
    EXPECT_EQ(result.path[2].initialPoint(), Geom::Point(35, 40));
    EXPECT_EQ(result.path[2].finalPoint(), Geom::Point(35, 60));

    // Extension lines run toward the line.
    EXPECT_TRUE(Geom::are_near(result.path[0].initialPoint(), Geom::Point(11, 40)));
    EXPECT_TRUE(Geom::are_near(result.path[0].finalPoint(), Geom::Point(37, 40)));
    EXPECT_TRUE(Geom::are_near(result.path[1].initialPoint(), Geom::Point(31, 60)));
    EXPECT_TRUE(Geom::are_near(result.path[1].finalPoint(), Geom::Point(37, 60)));

    EXPECT_TRUE(Geom::are_near(result.label_pos, Geom::Point(36, 50)));
    EXPECT_NEAR(std::abs(result.label_angle_deg), 90.0, 1e-9);

    // Negative offset puts the line left of the leftmost point.
    Style flipped = style;
    flipped.offset = -5.0;
    auto const flipped_result = build(Kind::Vertical, pts2(Geom::Point(10, 40), Geom::Point(30, 60)), flipped);
    ASSERT_TRUE(flipped_result.valid);
    EXPECT_EQ(flipped_result.path[2].initialPoint(), Geom::Point(5, 40)); // 10 - |offset|
    EXPECT_EQ(flipped_result.path[2].finalPoint(), Geom::Point(5, 60));
}

TEST(DimensionBuildTest, RightToLeftLineStaysUpright)
{
    Style const style = default_style();
    auto const result = build(Kind::Horizontal, pts2(Geom::Point(80, 40), Geom::Point(10, 60)), style);
    ASSERT_TRUE(result.valid);
    EXPECT_DOUBLE_EQ(result.value_uu_or_deg, 70.0);
    // A right-to-left line must not turn the label upside down.
    EXPECT_DOUBLE_EQ(result.label_angle_deg, 0.0);

    auto const aligned = build(Kind::Aligned, pts2(Geom::Point(0, 0), Geom::Point(-30, -40)), style);
    ASSERT_TRUE(aligned.valid);
    EXPECT_NEAR(aligned.label_angle_deg, 53.13010235415598, 1e-9); // -126.87 + 180
    EXPECT_NEAR(std::abs(aligned.label_angle_deg), 53.13010235415598, 1e-9);
    EXPECT_LE(aligned.label_angle_deg, 90.0);
    EXPECT_GE(aligned.label_angle_deg, -90.0);
}

TEST(DimensionBuildTest, TextHorizontalForcesAngleZero)
{
    Style style = default_style();
    style.text_horizontal = true;
    auto const result = build(Kind::Aligned, pts2(Geom::Point(0, 0), Geom::Point(30, 40)), style);
    ASSERT_TRUE(result.valid);
    EXPECT_DOUBLE_EQ(result.label_angle_deg, 0.0);
}

TEST(DimensionBuildTest, TextPositionsShiftAlongTheLine)
{
    Style style = default_style();
    auto const above = build(Kind::Horizontal, pts2(Geom::Point(10, 40), Geom::Point(80, 60)), style);
    ASSERT_TRUE(above.valid);
    EXPECT_TRUE(Geom::are_near(above.label_pos, Geom::Point(45, 34)));

    style.text_position = TextPosition::Inline;
    auto const inline_result = build(Kind::Horizontal, pts2(Geom::Point(10, 40), Geom::Point(80, 60)), style);
    ASSERT_TRUE(inline_result.valid);
    EXPECT_TRUE(Geom::are_near(inline_result.label_pos, Geom::Point(45, 35)));

    style.text_position = TextPosition::Below;
    auto const below = build(Kind::Horizontal, pts2(Geom::Point(10, 40), Geom::Point(80, 60)), style);
    ASSERT_TRUE(below.valid);
    EXPECT_TRUE(Geom::are_near(below.label_pos, Geom::Point(45, 36)));
}

TEST(DimensionBuildTest, SegmentMatchesAligned)
{
    auto const curve = closed_rect_pv();
    auto const n0 = resolve_anchor(*parse_anchor("node:0"), Geom::OptRect(), curve);
    auto const n1 = resolve_anchor(*parse_anchor("node:1"), Geom::OptRect(), curve);
    auto const n2 = resolve_anchor(*parse_anchor("node:2"), Geom::OptRect(), curve);
    auto const n3 = resolve_anchor(*parse_anchor("node:3"), Geom::OptRect(), curve);
    ASSERT_TRUE(n0 && n1 && n2 && n3);

    Style const style = default_style();
    // k = 1: nodes (10,0) -> (10,8).
    auto const segment = build(Kind::Segment, pts2(*n1, *n2), style);
    auto const aligned = build(Kind::Aligned, pts2(*n1, *n2), style);
    ASSERT_TRUE(segment.valid);
    ASSERT_TRUE(aligned.valid);
    EXPECT_EQ(segment.value_uu_or_deg, aligned.value_uu_or_deg);
    EXPECT_EQ(segment.path.size(), aligned.path.size());
    for (size_t i = 0; i < segment.path.size(); ++i) {
        EXPECT_TRUE(Geom::are_near(segment.path[i].initialPoint(), aligned.path[i].initialPoint()));
        EXPECT_TRUE(Geom::are_near(segment.path[i].finalPoint(), aligned.path[i].finalPoint()));
    }

    // Closed wrap: last node wraps to the subpath start.
    auto const wrap = build(Kind::Segment, pts2(*n3, *n0), style);
    ASSERT_TRUE(wrap.valid);
    EXPECT_DOUBLE_EQ(wrap.value_uu_or_deg, 8.0);
}

TEST(DimensionBuildTest, ArrowsOutsideOnShortLines)
{
    Style style = default_style();
    style.arrow_length = 2.5;

    // Line length 10 >= 2.5 * 2.5: arrows inside, bodies within the line span.
    auto const inside = build(Kind::Horizontal, pts2(Geom::Point(0, 0), Geom::Point(10, 0)), style);
    ASSERT_TRUE(inside.valid);
    EXPECT_TRUE(Geom::are_near(inside.path[3].initialPoint(), Geom::Point(0, -5)));  // arrow tip at D1
    EXPECT_GE(inside.path[3].boundsFast()->max().x(), -1e-9);

    // Line length 5 < 6.25: arrows flipped outside, bodies beyond the extension lines.
    auto const outside = build(Kind::Horizontal, pts2(Geom::Point(0, 0), Geom::Point(5, 0)), style);
    ASSERT_TRUE(outside.valid);
    EXPECT_TRUE(Geom::are_near(outside.path[3].initialPoint(), Geom::Point(0, -5))); // tip stays at D1
    ASSERT_TRUE(outside.path[3].boundsFast());
    EXPECT_LE(outside.path[3].boundsFast()->max().x(), 1e-9); // whole arrow left of x = 0
    ASSERT_TRUE(outside.path[4].boundsFast());
    EXPECT_GE(outside.path[4].boundsFast()->min().x(), 5.0 - 1e-9); // whole arrow right of x = 5
}

TEST(DimensionBuildTest, Angular90)
{
    Style const style = default_style();
    auto const result = build(Kind::Angular, pts3(Geom::Point(10, 0), Geom::Point(0, 0), Geom::Point(0, 10)), style);
    ASSERT_TRUE(result.valid);
    EXPECT_DOUBLE_EQ(result.value_uu_or_deg, 90.0);

    // Arc + reversed arc ring, then two arrows.
    ASSERT_EQ(result.path.size(), 3u);
    EXPECT_TRUE(result.path[0].closed());

    // Arc radius = offset: ring starts on arm A at distance 5 from the vertex.
    EXPECT_TRUE(Geom::are_near(result.path[0].initialPoint(), Geom::Point(5, 0)));

    // Ring encloses zero area (non-arrow subpath).
    EXPECT_LT(std::abs(subpath_area(result.path[0])), 1e-9);

    // Label outside the arc, on the bisector.
    double const isqrt2 = std::sqrt(2.0) / 2.0;
    EXPECT_TRUE(Geom::are_near(result.label_pos, Geom::Point(6.0 * isqrt2, 6.0 * isqrt2), 1e-9));
    // Tangent at the arc midpoint of a 90 degree arc: upright -45 degrees.
    EXPECT_NEAR(result.label_angle_deg, -45.0, 1e-9);

    // Arrows are tangent to the arc at their own end (arc length 7.85 >= 6.25,
    // so inside): start tip (5,0) with its body along +y up to (5,2.5); end
    // tip (0,5) with its body along +x up to (2.5,5). Wing half-width 2.5/3.
    double const wing = 2.5 / 3.0;
    auto const start_box = result.path[1].boundsFast();
    auto const end_box = result.path[2].boundsFast();
    ASSERT_TRUE(start_box);
    ASSERT_TRUE(end_box);
    EXPECT_TRUE(Geom::are_near(start_box->min(), Geom::Point(5.0 - wing, 0.0), 1e-9));
    EXPECT_TRUE(Geom::are_near(start_box->max(), Geom::Point(5.0 + wing, 2.5), 1e-9));
    EXPECT_TRUE(Geom::are_near(end_box->min(), Geom::Point(0.0, 5.0 - wing), 1e-9));
    EXPECT_TRUE(Geom::are_near(end_box->max(), Geom::Point(2.5, 5.0 + wing), 1e-9));
}

TEST(DimensionBuildTest, Angular45)
{
    Style const style = default_style();
    Geom::Point const b(10.0 * std::cos(M_PI / 4.0), 10.0 * std::sin(M_PI / 4.0));
    auto const result = build(Kind::Angular, pts3(Geom::Point(10, 0), Geom::Point(0, 0), b), style);
    ASSERT_TRUE(result.valid);
    EXPECT_NEAR(result.value_uu_or_deg, 45.0, 1e-9);
    EXPECT_TRUE(Geom::are_near(result.path[0].initialPoint(), Geom::Point(5, 0)));
}

TEST(DimensionBuildTest, AngularSweepDirectionDoesNotChangeValue)
{
    Style const style = default_style();
    auto const cw = build(Kind::Angular, pts3(Geom::Point(0, 10), Geom::Point(0, 0), Geom::Point(10, 0)), style);
    ASSERT_TRUE(cw.valid);
    EXPECT_DOUBLE_EQ(cw.value_uu_or_deg, 90.0);
}

TEST(DimensionBuildTest, AngularRadiusFallbackHalfShorterArm)
{
    Style style = default_style();
    // Arms of length 10 and 6; offset 0 -> radius 3.
    style.offset = 0.0;
    auto const result = build(Kind::Angular, pts3(Geom::Point(10, 0), Geom::Point(0, 0), Geom::Point(0, 6)), style);
    ASSERT_TRUE(result.valid);
    EXPECT_TRUE(Geom::are_near(result.path[0].initialPoint(), Geom::Point(3, 0)));

    // Negative offset falls back the same way.
    style.offset = -2.0;
    auto const negative = build(Kind::Angular, pts3(Geom::Point(10, 0), Geom::Point(0, 0), Geom::Point(0, 6)), style);
    ASSERT_TRUE(negative.valid);
    EXPECT_TRUE(Geom::are_near(negative.path[0].initialPoint(), Geom::Point(3, 0)));
}

TEST(DimensionBuildTest, NonArrowSubpathsEncloseZeroArea)
{
    Style const style = default_style();

    // Linear: 2 extension lines + dimension line + 2 closed arrows.
    auto const linear = build(Kind::Aligned, pts2(Geom::Point(10, 40), Geom::Point(13, 44)), style);
    ASSERT_TRUE(linear.valid);
    size_t filled = 0;
    for (auto const &path : linear.path) {
        if (std::abs(subpath_area(path)) > 1e-6) {
            ++filled;
            EXPECT_TRUE(path.closed());
        } else {
            EXPECT_LT(std::abs(subpath_area(path)), 1e-9);
        }
    }
    EXPECT_EQ(filled, 2u);

    // Angular: zero-area ring + 2 closed arrows.
    auto const angular = build(Kind::Angular, pts3(Geom::Point(10, 0), Geom::Point(0, 0), Geom::Point(0, 10)), style);
    ASSERT_TRUE(angular.valid);
    filled = 0;
    for (auto const &path : angular.path) {
        if (std::abs(subpath_area(path)) > 1e-6) {
            ++filled;
            EXPECT_TRUE(path.closed());
        } else {
            EXPECT_LT(std::abs(subpath_area(path)), 1e-9);
        }
    }
    EXPECT_EQ(filled, 2u);
}

TEST(DimensionBuildTest, DegenerateInputs)
{
    Style const style = default_style();

    auto expect_invalid = [](Result const &result) {
        EXPECT_FALSE(result.valid);
        EXPECT_TRUE(result.path.empty());
        EXPECT_FALSE(std::isnan(result.label_angle_deg));
        EXPECT_FALSE(std::isnan(result.value_uu_or_deg));
        EXPECT_TRUE(std::isfinite(result.label_pos.x()));
        EXPECT_TRUE(std::isfinite(result.label_pos.y()));
    };

    // Coincident points.
    expect_invalid(build(Kind::Aligned, pts2(Geom::Point(1, 1), Geom::Point(1, 1)), style));
    // Horizontal with equal x / vertical with equal y.
    expect_invalid(build(Kind::Horizontal, pts2(Geom::Point(5, 0), Geom::Point(5, 40)), style));
    expect_invalid(build(Kind::Vertical, pts2(Geom::Point(0, 5), Geom::Point(40, 5)), style));
    // Angular with a zero arm and with coincident arms.
    expect_invalid(build(Kind::Angular, pts3(Geom::Point(0, 0), Geom::Point(0, 0), Geom::Point(0, 10)), style));
    expect_invalid(build(Kind::Angular, pts3(Geom::Point(5, 0), Geom::Point(0, 0), Geom::Point(9, 0)), style));
    // Wrong point counts.
    expect_invalid(build(Kind::Angular, pts2(Geom::Point(0, 0), Geom::Point(1, 1)), style));
    expect_invalid(build(Kind::Aligned, pts1(Geom::Point(0, 0)), style));
    // Non-finite input.
    expect_invalid(build(Kind::Aligned, pts2(Geom::Point(0, 0), Geom::Point(std::numeric_limits<double>::quiet_NaN(), 1)), style));
    expect_invalid(build(Kind::Horizontal, pts2(Geom::Point(0, 0), Geom::Point(std::numeric_limits<double>::infinity(), 1)), style));
}

// -------------------------------------------------------------------------

TEST(DimensionLabelTest, ClassicDefaultLook)
{
    EXPECT_EQ(format_label(50.0, 2, "mm", true, "", "", false), "50.00 mm");
}

TEST(DimensionLabelTest, PrecisionVariants)
{
    EXPECT_EQ(format_label(50.4, 0, "mm", true, "", "", false), "50 mm");
    EXPECT_EQ(format_label(50.4, 1, "mm", true, "", "", false), "50.4 mm");
    EXPECT_EQ(format_label(50.4, 6, "mm", true, "", "", false), "50.400000 mm");
    // Precision clamped to 0..6.
    EXPECT_EQ(format_label(50.4, 99, "mm", true, "", "", false), "50.400000 mm");
    EXPECT_EQ(format_label(50.4, -3, "mm", true, "", "", false), "50 mm");
}

TEST(DimensionLabelTest, UnitsPrefixSuffix)
{
    EXPECT_EQ(format_label(50.0, 2, "mm", false, "", "", false), "50.00");
    EXPECT_EQ(format_label(50.0, 2, "mm", true, "[", "]", false), "[50.00 mm]");
    EXPECT_EQ(format_label(50.0, 2, "", true, "", "", false), "50.00");
    EXPECT_EQ(format_label(90.0, 2, "", true, "", "", true), "90.00°");
    EXPECT_EQ(format_label(90.0, 2, "°", true, "", "", true), "90.00°");
    EXPECT_EQ(format_label(90.0, 2, "°", false, "", "", true), "90.00");
}

TEST(DimensionLabelTest, NegativeZeroNormalised)
{
    EXPECT_EQ(format_label(-0.0001, 2, "mm", true, "", "", false), "0.00 mm");
    EXPECT_EQ(format_label(-0.0, 2, "mm", true, "", "", false), "0.00 mm");
    EXPECT_EQ(format_label(-0.0001, 3, "", false, "", "", true), "0.000");
    EXPECT_EQ(format_label(-1.0, 2, "mm", true, "", "", false), "-1.00 mm");
}

TEST(DimensionLabelTest, ClassicLocaleDot)
{
    try {
        std::locale const saved = std::locale::global(std::locale("de_DE.UTF-8"));
        EXPECT_EQ(format_label(50.0, 2, "mm", true, "", "", false), "50.00 mm");
        std::locale::global(saved);
    } catch (std::runtime_error const &) {
        GTEST_SKIP() << "de_DE.UTF-8 locale not installed";
    }
}

// -------------------------------------------------------------------------

TEST(DimensionUnitTest, Conversions)
{
    // 1 user unit = 1 px: 70 px in mm/cm/in/px.
    double const px_per_mm = 96.0 / 25.4;
    EXPECT_NEAR(to_unit(70.0, 1.0, "mm"), 70.0 * 25.4 / 96.0, 1e-9);
    EXPECT_NEAR(to_unit(70.0, 1.0, "cm"), 70.0 * 25.4 / 960.0, 1e-9);
    EXPECT_NEAR(to_unit(70.0, 1.0, "in"), 70.0 / 96.0, 1e-9);
    EXPECT_DOUBLE_EQ(to_unit(70.0, 1.0, "px"), 70.0);

    // 1 user unit = 1 mm (doc scale 96/25.4): 70 user units read as 70 mm.
    EXPECT_NEAR(to_unit(70.0, px_per_mm, "mm"), 70.0, 1e-6);
    EXPECT_NEAR(to_unit(70.0, px_per_mm, "cm"), 7.0, 1e-7);

    // Drawing scale multiplies the reported value.
    EXPECT_DOUBLE_EQ(to_unit(70.0, 1.0, "px", 2.0), 140.0);
}

// -------------------------------------------------------------------------

TEST(DimensionCurveNodesTest, MultiSubpathIndicesContinue)
{
    // Already covered via DimensionResolveTest.NodeOpenPolylineAndSubpathOrder;
    // this locks the curve_nodes() surface itself.
    auto const nodes = curve_nodes(open_polyline_pv());
    ASSERT_EQ(nodes.size(), 5u);
    EXPECT_TRUE(Geom::are_near(nodes[2], Geom::Point(4, 3)));
    EXPECT_TRUE(Geom::are_near(nodes[4], Geom::Point(12, 10)));
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