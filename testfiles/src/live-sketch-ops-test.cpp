// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the Live Sketch document layer: placement, coordinate spaces, the
 * visual millimetre tolerance, style writing (locked style and tool CSS),
 * error paths with no document change, one-step undo, generated ids and the
 * artistic media handoff.
 *
 * Geometry itself is locked by live-sketch-geometry-test.cpp.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>

#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/svg-path-parser.h>

#include <src/actions/actions-artistic-media.h>
#include <src/actions/actions-live-sketch.h>
#include <src/document-undo.h>
#include <src/document.h>
#include <src/inkscape.h>
#include <src/object/object-set.h>
#include <src/object/sp-item-group.h>
#include <src/object/sp-item.h>
#include <src/object/sp-path.h>
#include <src/object/sp-root.h>
#include <src/path/live-sketch-geometry.h>
#include <src/util/cast.h>
#include <src/xml/node.h>
#include <src/xml/repr.h>

using namespace Inkscape;
using namespace std::literals;
using Inkscape::LiveSketchOps::CreateResult;
using Inkscape::LiveSketchOps::Options;

namespace {

// 200 x 100 mm document, viewBox 0 0 200 100: 1 user unit = 1 mm.
constexpr auto kDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="200mm" height="100mm" viewBox="0 0 200 100" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1">
    <rect id="r0" x="5" y="5" width="10" height="10" style="fill:#808080;stroke:none"/>
    <g id="g1" transform="translate(100,50) scale(2)"/>
    <g id="g0" transform="scale(0)"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="L2" id="L2"/>
</svg>)A"sv;

// 100 x 50 mm document with a 400 x 200 user-unit viewBox: 1 uu = 0.25 mm.
constexpr auto kScaledDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="50mm" viewBox="0 0 400 200" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1"/>
</svg>)A"sv;

std::map<std::string, std::string> parse_style(std::string const &style)
{
    std::map<std::string, std::string> result;
    std::istringstream stream(style);
    for (std::string declaration; std::getline(stream, declaration, ';');) {
        auto const colon = declaration.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        result[declaration.substr(0, colon)] = declaration.substr(colon + 1);
    }
    return result;
}

std::string style_value(std::map<std::string, std::string> const &style, std::string const &key)
{
    auto const it = style.find(key);
    return it == style.end() ? std::string() : it->second;
}

/// Classic-locale number (the test process may use a decimal comma locale).
double number_of(std::string const &text)
{
    char *end = nullptr;
    return g_ascii_strtod(text.c_str(), &end);
}

Geom::Point pt(double x, double y)
{
    return Geom::Point(x, y);
}

Geom::PathVector pathvector_of(SPPath const *path)
{
    if (!path) {
        return {};
    }
    auto const *d = path->getAttribute("d");
    return d ? Geom::parse_svg_path(d) : Geom::PathVector();
}

/// Straight zigzag in document user units for the visual-tolerance case.
std::vector<Geom::Point> scaled_zigzag()
{
    std::vector<Geom::Point> points;
    for (int i = 0; i <= 10; ++i) {
        points.emplace_back(40.0 + 16.0 * i, 100.0 + (i % 2 == 0 ? 2.0 : -2.0));
    }
    return points;
}

} // namespace

class LiveSketchOpsTest : public ::testing::Test
{
public:
    static void SetUpTestCase()
    {
        Inkscape::Application::create(false);
    }

    void SetUp() override { use(kDoc); }
    void TearDown() override { doc.reset(); }

    void use(std::string_view source)
    {
        doc.reset();
        doc = SPDocument::createNewDocFromMem(source);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    SPItem *item(char const *id) { return cast<SPItem>(doc->getObjectById(id)); }
    SPGroup *group(char const *id) { return cast<SPGroup>(doc->getObjectById(id)); }

    std::string xml()
    {
        doc->ensureUpToDate();
        std::string const saved = sp_repr_save_buf(doc->getReprDoc()).raw();
        std::string result;
        std::istringstream stream(saved);
        for (std::string line; std::getline(stream, line);) {
            if (line.find("inkscape:version=") != std::string::npos) {
                continue;
            }
            result += line;
            result += '\n';
        }
        return result;
    }

    CreateResult create(std::vector<Geom::Point> const &points, Options const &options = {},
                        SPObject *parent = nullptr)
    {
        return Inkscape::LiveSketchOps::create(doc.get(), points, options, parent);
    }

    std::unique_ptr<SPDocument> doc;
};

// -------------------------------------------------------------------------

TEST_F(LiveSketchOpsTest, DefaultCreateLabelStyleAndOneUndoStep)
{
    std::string const before = xml();

    auto const result = create({pt(10, 10), pt(110, 10)});
    ASSERT_NE(result.path, nullptr) << result.error;
    EXPECT_TRUE(result.error.empty());

    auto *layer = group("L2");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(result.path->parent, layer);
    EXPECT_EQ(layer->getRepr()->lastChild(), result.path->getRepr());
    EXPECT_STREQ(result.path->getAttribute("inkscape:label"), "Live Sketch");

    auto const style = parse_style(result.path->getAttribute("style"));
    EXPECT_EQ(style_value(style, "fill"), "none");
    EXPECT_EQ(style_value(style, "stroke"), "#000000");
    EXPECT_EQ(style_value(style, "stroke-opacity"), "1");
    EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 0.25, 1e-9);
    EXPECT_EQ(style_value(style, "stroke-linecap"), "round");
    EXPECT_EQ(style_value(style, "stroke-linejoin"), "round");

    auto const *d = result.path->getAttribute("d");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(std::string(d).find_first_of("Zz"), std::string::npos);
    auto const geometry = pathvector_of(result.path);
    ASSERT_EQ(geometry.size(), 1u);
    ASSERT_EQ(geometry[0].size_open(), 1u);
    EXPECT_FALSE(geometry[0].closed());
    EXPECT_NEAR((geometry[0].initialPoint() - pt(10, 10)).length(), 0.0, 1e-6);
    EXPECT_NEAR((geometry[0].finalPoint() - pt(110, 10)).length(), 0.0, 1e-6);

    std::string const created_id = result.path->getId();
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get()));
    auto *restored = cast<SPPath>(doc->getObjectById(created_id.c_str()));
    ASSERT_NE(restored, nullptr);
    EXPECT_STREQ(restored->getAttribute("d"), d);
}

TEST_F(LiveSketchOpsTest, ClosedGestureStaysOpen)
{
    Options options;
    options.smoothing = 0.0;
    auto const result = create({pt(20, 20), pt(60, 20), pt(60, 60), pt(20, 60), pt(20, 20)}, options);
    ASSERT_NE(result.path, nullptr) << result.error;

    auto const *d = result.path->getAttribute("d");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(std::string(d).find_first_of("Zz"), std::string::npos);

    auto const geometry = pathvector_of(result.path);
    ASSERT_EQ(geometry.size(), 1u);
    ASSERT_EQ(geometry[0].size_open(), 4u);
    for (auto const &curve : geometry[0]) {
        EXPECT_TRUE(curve.isLineSegment());
    }
    EXPECT_NEAR((geometry[0].initialPoint() - geometry[0].finalPoint()).length(), 0.0, 1e-9);
}

TEST_F(LiveSketchOpsTest, GroupParentUsesLocalCoordinatesAndVisualWidth)
{
    Options options;
    options.parent = std::string("g1");
    options.smoothing = 0.0;
    options.stroke_width_mm = 2.0;
    auto const result = create({pt(110, 60), pt(150, 60)}, options);
    ASSERT_NE(result.path, nullptr) << result.error;

    auto *g1 = item("g1");
    ASSERT_NE(g1, nullptr);
    EXPECT_EQ(result.path->parent, g1);
    EXPECT_EQ(g1->getRepr()->lastChild(), result.path->getRepr());

    auto const geometry = pathvector_of(result.path);
    ASSERT_EQ(geometry.size(), 1u);
    EXPECT_NEAR((geometry[0].initialPoint() - pt(5, 5)).length(), 0.0, 1e-6);
    EXPECT_NEAR((geometry[0].finalPoint() - pt(25, 5)).length(), 0.0, 1e-6);

    // The local -> document user-unit map is i2doc * c2p^-1.
    Geom::Affine const local2doc = g1->i2doc_affine() * doc->getRoot()->c2p.inverse();
    Geom::Point const begin = geometry[0].initialPoint() * local2doc;
    Geom::Point const end = geometry[0].finalPoint() * local2doc;
    EXPECT_NEAR((begin - pt(110, 60)).length(), 0.0, 1e-6);
    EXPECT_NEAR((end - pt(150, 60)).length(), 0.0, 1e-6);

    // 2 mm visual in a 2x group: 1 parent-local unit.
    auto const style = parse_style(result.path->getAttribute("style"));
    EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 1.0, 1e-6);

    // The parent override produces the same geometry.
    Options override_options;
    override_options.smoothing = 0.0;
    override_options.stroke_width_mm = 2.0;
    auto const direct = create({pt(110, 60), pt(150, 60)}, override_options, g1);
    ASSERT_NE(direct.path, nullptr) << direct.error;
    EXPECT_STREQ(direct.path->getAttribute("d"), result.path->getAttribute("d"));
}

TEST_F(LiveSketchOpsTest, ToleranceIsVisualMillimetres)
{
    use(kScaledDoc);
    auto const points = scaled_zigzag();

    Options smooth;
    smooth.smoothing = 20.0; // 1 mm, above the 0.5 mm amplitude
    auto const one = create(points, smooth);
    ASSERT_NE(one.path, nullptr) << one.error;
    auto const one_geometry = pathvector_of(one.path);
    ASSERT_EQ(one_geometry.size(), 1u);
    EXPECT_LE(one_geometry[0].size_open(), 2u);

    Options tight;
    tight.smoothing = 5.0; // 0.25 mm, below the 0.5 mm amplitude
    auto const many = create(points, tight);
    ASSERT_NE(many.path, nullptr) << many.error;
    auto const many_geometry = pathvector_of(many.path);
    ASSERT_EQ(many_geometry.size(), 1u);
    EXPECT_GE(many_geometry[0].size_open(), 2u);

    // 0.25 mm default width in this 0.25 mm/uu document is 1 uu.
    auto const default_style = parse_style(one.path->getAttribute("style"));
    EXPECT_NEAR(number_of(style_value(default_style, "stroke-width")), 1.0, 1e-6);

    Options wide;
    wide.stroke_width_mm = 1.0;
    auto const wide_result = create({pt(40, 100), pt(240, 100)}, wide);
    ASSERT_NE(wide_result.path, nullptr) << wide_result.error;
    auto const wide_style = parse_style(wide_result.path->getAttribute("style"));
    EXPECT_NEAR(number_of(style_value(wide_style, "stroke-width")), 4.0, 1e-6);
}

TEST_F(LiveSketchOpsTest, ToolStyleRewritesAbsoluteWidthsOnly)
{
    {
        Options options;
        options.style = std::string("fill:none;stroke:#00ff00;stroke-width:1px");
        auto const result = create({pt(10, 10), pt(110, 10)}, options);
        ASSERT_NE(result.path, nullptr) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "fill"), "none");
        EXPECT_EQ(style_value(style, "stroke"), "#00ff00");
        EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 1.0 / (96.0 / 25.4), 1e-5);
    }

    {
        Options options;
        options.style = std::string("fill:none;stroke:#00ff00;stroke-width:2mm");
        auto const result = create({pt(10, 10), pt(110, 10)}, options);
        ASSERT_NE(result.path, nullptr) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 2.0, 1e-6);
    }

    {
        Options options;
        options.style = std::string("fill:none;stroke:#00ff00;stroke-width:50%");
        auto const result = create({pt(10, 10), pt(110, 10)}, options);
        ASSERT_NE(result.path, nullptr) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "stroke-width"), "50%");
    }

    {
        Options options;
        options.style = std::string("fill:none;stroke:#00ff00");
        auto const result = create({pt(10, 10), pt(110, 10)}, options);
        ASSERT_NE(result.path, nullptr) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_TRUE(style.find("stroke-width") == style.end());
    }

    {
        // stroke/stroke-width options are ignored when a tool style is set.
        Options options;
        options.stroke = std::string("#ff0000");
        options.stroke_width_mm = 5.0;
        options.style = std::string("fill:none;stroke:#00ff00;stroke-width:1px");
        auto const result = create({pt(10, 10), pt(110, 10)}, options);
        ASSERT_NE(result.path, nullptr) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "stroke"), "#00ff00");
        EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 1.0 / (96.0 / 25.4), 1e-5);
    }
}

TEST_F(LiveSketchOpsTest, ParsePointsAndOptions)
{
    std::string error;

    auto const points = Inkscape::LiveSketchOps::parse_points(" 10 10 110\t10 ", error);
    ASSERT_TRUE(points.has_value()) << error;
    ASSERT_EQ(points->size(), 2u);
    EXPECT_EQ((*points)[0], pt(10, 10));
    EXPECT_EQ((*points)[1], pt(110, 10));

    EXPECT_TRUE(Inkscape::LiveSketchOps::parse_points("", error).has_value());

    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_points("abc 1", error));
    EXPECT_EQ(error, "bad number 'abc'");
    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_points("10 10 20", error));
    EXPECT_EQ(error, "odd number of coordinates");

    auto const options = Inkscape::LiveSketchOps::parse_options(
        "smoothing=10,stroke=#ABCDEF,stroke-width=1.5,id=ls1,parent=g1", error);
    ASSERT_TRUE(options.has_value()) << error;
    EXPECT_DOUBLE_EQ(options->smoothing, 10.0);
    EXPECT_EQ(options->stroke, "#ABCDEF");
    EXPECT_DOUBLE_EQ(options->stroke_width_mm, 1.5);
    EXPECT_EQ(options->id.value_or(""), "ls1");
    EXPECT_EQ(options->parent.value_or(""), "g1");
    EXPECT_FALSE(options->style.has_value());

    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_options("smooth=5", error));
    EXPECT_EQ(error, "unknown option 'smooth'");
    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_options("smoothing=150", error));
    EXPECT_EQ(error, "smoothing must be between 0 and 100");
    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_options("smoothing=x", error));
    EXPECT_EQ(error, "bad number 'x'");
    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_options("stroke=blue", error));
    EXPECT_EQ(error, "bad color 'blue'");
    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_options("stroke=none", error));
    EXPECT_EQ(error, "bad color 'none'");
    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_options("stroke-width=0", error));
    EXPECT_EQ(error, "stroke-width must be > 0 and <= 100 mm");
    EXPECT_FALSE(Inkscape::LiveSketchOps::parse_options("stroke-width=101", error));
    EXPECT_EQ(error, "stroke-width must be > 0 and <= 100 mm");
}

TEST_F(LiveSketchOpsTest, CreateErrorsLeaveTheDocumentUntouched)
{
    auto const expect_failure = [&](std::vector<Geom::Point> const &points, Options const &options,
                                    SPObject *parent, std::string const &message) {
        std::string const before = xml();
        auto const result = Inkscape::LiveSketchOps::create(doc.get(), points, options, parent);
        EXPECT_EQ(result.path, nullptr) << message;
        EXPECT_EQ(result.error, message);
        EXPECT_EQ(xml(), before) << message;
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get())) << message;
    };

    expect_failure({pt(1e19, 0), pt(20, 20)}, {}, nullptr,
                   "live-sketch: coordinates out of range");

    Options wrong_parent;
    wrong_parent.parent = std::string("r0");
    expect_failure({pt(10, 10), pt(20, 20)}, wrong_parent, nullptr,
                   "live-sketch: parent 'r0' is not a group");

    Options unknown_parent;
    unknown_parent.parent = std::string("nosuch");
    expect_failure({pt(10, 10), pt(20, 20)}, unknown_parent, nullptr,
                   "live-sketch: parent 'nosuch' is not a group");

    Options degenerate_parent;
    degenerate_parent.parent = std::string("g0");
    expect_failure({pt(10, 10), pt(20, 20)}, degenerate_parent, nullptr,
                   "live-sketch: parent 'g0' has a degenerate transform");

    Options duplicate_id;
    duplicate_id.id = std::string("r0");
    expect_failure({pt(10, 10), pt(20, 20)}, duplicate_id, nullptr,
                   "live-sketch: id 'r0' already exists");

    expect_failure({pt(10, 10), pt(10, 10)}, {}, nullptr,
                   "live-sketch: needs at least two distinct points");
    expect_failure({}, {}, nullptr, "live-sketch: needs at least two distinct points");
}

TEST_F(LiveSketchOpsTest, GeneratedIdsAndSelectionHandoff)
{
    Options options;
    options.id = std::string("ls1");
    auto const named = create({pt(10, 10), pt(110, 10)}, options);
    ASSERT_NE(named.path, nullptr) << named.error;
    EXPECT_EQ(doc->getObjectById("ls1"), named.path);

    auto const first = create({pt(10, 20), pt(110, 20)});
    ASSERT_NE(first.path, nullptr) << first.error;
    auto const second = create({pt(10, 30), pt(110, 30)});
    ASSERT_NE(second.path, nullptr) << second.error;
    EXPECT_NE(first.path->getId(), second.path->getId());

    std::vector<std::string> ids;
    for (auto *child : group("L2")->childList(false)) {
        ids.emplace_back(child->getId());
    }
    ASSERT_GE(ids.size(), 3u);
    EXPECT_EQ(ids[ids.size() - 2], first.path->getId());
    EXPECT_EQ(ids[ids.size() - 1], second.path->getId());

    ObjectSet set(doc.get());
    set.set(named.path);
    std::string errors;
    int const applied = ArtisticOps::apply(set, ArtisticOps::Options{}, errors);
    EXPECT_EQ(applied, 1);
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(ArtisticOps::is_artistic(named.path));
    EXPECT_EQ(doc->getObjectById("ls1"), named.path);
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
