// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the Smart Fill document layer: candidate collection (visibility,
 * ordering, transforms, clones, LPE output), creation style precedence,
 * coordinate spaces, error paths and byte-identical one-step undo.
 *
 * Geometry itself is locked by smart-fill-geometry-test.cpp.
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
#include <2geom/rect.h>
#include <2geom/svg-path-parser.h>
#include <2geom/transforms.h>

#include <src/actions/actions-smart-fill.h>
#include <src/document-undo.h>
#include <src/document.h>
#include <src/inkscape.h>
#include <src/object/sp-defs.h>
#include <src/object/sp-item-group.h>
#include <src/object/sp-item.h>
#include <src/object/sp-path.h>
#include <src/object/sp-root.h>
#include <src/path/smart-fill-geometry.h>
#include <src/util/cast.h>
#include <src/xml/node.h>
#include <src/xml/repr.h>

using namespace Inkscape;
using namespace std::literals;
namespace SF = Inkscape::SmartFill;
using Inkscape::SmartFillOps::Candidate;
using Inkscape::SmartFillOps::CreateResult;
using Inkscape::SmartFillOps::Options;

namespace {

// 200 x 100 mm document, viewBox 0 0 200 100: 1 user unit = 1 mm.
constexpr auto kDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="200mm" height="100mm" viewBox="0 0 200 100" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">
  <defs>
    <rect id="tile" width="20" height="20"/>
    <rect id="deftile" x="0" y="0" width="5" height="5" style="fill:#123456"/>
    <inkscape:path-effect id="a1-artistic" effect="artistic_media" mode="preset"
        shape="uniform" shape_d="M 0,-0.5 L 1,-0.5 L 1,0.5 L 0,0.5 Z" width="10" angle="30"
        scale_with_object="true" is_visible="true" lpeversion="1"/>
  </defs>
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1">
    <rect id="r1" x="10" y="10" width="40" height="30" style="fill:#ff0000;stroke:none"/>
    <circle id="c1" cx="50" cy="25" r="10" style="fill:#00ff00;stroke:none"/>
    <path id="l1" d="M 30,5 V 45" style="fill:none;stroke:#000000;stroke-width:0.5"/>
    <path id="d1" d="M 70,10 H 110 V 50 H 70 Z M 80,20 V 40 H 100 V 20 Z"
          style="fill:#0000ff;fill-rule:evenodd;stroke:none"/>
    <g id="g1" transform="translate(130,10)">
      <rect id="g1r" x="0" y="0" width="30" height="30" style="fill:#ffff00"/>
      <rect id="g1s" x="10" y="10" width="10" height="10" style="fill:#00ffff"/>
    </g>
    <rect id="h1" x="10" y="60" width="30" height="30" style="display:none;fill:#808080"/>
    <rect id="k1" x="50" y="60" width="30" height="30" sodipodi:insensitive="true"
          style="fill:#808080"/>
    <use id="u1" xlink:href="#tile" x="90" y="60" style="fill:#ff00ff"/>
    <path id="a1" d="m 120,70 h 60 v 10 h -60 z"
          inkscape:original-d="M 120,75 L 180,75"
          inkscape:path-effect="#a1-artistic"
          style="fill:#000000;stroke:none;fill-rule:nonzero"/>
  </g>
</svg>)A"sv;

// 100 x 50 mm document with a 400 x 200 user-unit viewBox: 1 uu = 0.25 mm.
constexpr auto kScaledDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="50mm" viewBox="0 0 400 200" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1">
    <rect id="r1" x="40" y="40" width="160" height="120" style="fill:#ff0000"/>
  </g>
</svg>)A"sv;

constexpr auto kTextDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="200mm" height="100mm" viewBox="0 0 200 100" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1">
    <text id="text1" x="20" y="80" style="font-size:20px;fill:#00ff00">I</text>
  </g>
</svg>)A"sv;

constexpr auto kFlowDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="200mm" height="100mm" viewBox="0 0 200 100" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1">
    <flowRoot id="flow1" xml:space="preserve" style="font-size:20px">
      <flowRegion id="flowRegion1">
        <rect id="flowrect" x="10" y="10" width="120" height="60"/>
      </flowRegion>
      <flowPara id="flowpara1">I</flowPara>
    </flowRoot>
  </g>
</svg>)A"sv;

// 1 x 1 opaque red PNG.
constexpr auto kImageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="200mm" height="100mm" viewBox="0 0 200 100" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:xlink="http://www.w3.org/1999/xlink">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1">
    <image id="img1" x="10" y="60" width="20" height="10"
           xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="/>
  </g>
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

/// Classic-locale number, like the action's own parser (the test process may
/// run under a locale with a decimal comma).
double number_of(std::string const &text)
{
    char *end = nullptr;
    double const value = g_ascii_strtod(text.c_str(), &end);
    return value;
}

void expect_bbox(Geom::PathVector const &region, double x0, double y0, double x1, double y1,
                 double tolerance = 0.05)
{
    auto const bounds = region.boundsExact();
    ASSERT_TRUE(bounds) << "empty region";
    EXPECT_NEAR(bounds->min().x(), x0, tolerance);
    EXPECT_NEAR(bounds->min().y(), y0, tolerance);
    EXPECT_NEAR(bounds->max().x(), x1, tolerance);
    EXPECT_NEAR(bounds->max().y(), y1, tolerance);
}

Geom::PathVector pathvector_of(SPPath const *path)
{
    if (!path) {
        return {};
    }
    auto const *d = path->getAttribute("d");
    return d ? Geom::parse_svg_path(d) : Geom::PathVector();
}

Candidate const *find_candidate(std::vector<Candidate> const &candidates, char const *id)
{
    for (auto const &candidate : candidates) {
        if (candidate.item && g_strcmp0(candidate.item->getId(), id) == 0) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

class SmartFillOpsTest : public ::testing::Test
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

    CreateResult create_at(double x, double y, Options const &options = {})
    {
        return Inkscape::SmartFillOps::create(doc.get(), Geom::Point(x, y), options, nullptr);
    }

    std::unique_ptr<SPDocument> doc;
};

// -------------------------------------------------------------------------

TEST_F(SmartFillOpsTest, CandidatesAreTopmostFirstVisibleAndInUserUnits)
{
    auto const candidates = Inkscape::SmartFillOps::collect_candidates(doc.get());
    ASSERT_EQ(candidates.size(), 8u);

    std::vector<std::string> ids;
    for (auto const &candidate : candidates) {
        ids.emplace_back(candidate.item->getId());
    }
    std::vector<std::string> const expected{"a1", "u1", "g1s", "g1r", "d1", "l1", "c1", "r1"};
    EXPECT_EQ(ids, expected);

    auto const *use_candidate = find_candidate(candidates, "u1");
    ASSERT_NE(use_candidate, nullptr);
    auto const use_bounds = use_candidate->path_uu.boundsExact();
    ASSERT_TRUE(use_bounds);
    // Clone of the 20 x 20 tile, translated by x=90 y=60.
    EXPECT_NEAR(use_bounds->min().x(), 90.0, 1e-6);
    EXPECT_NEAR(use_bounds->min().y(), 60.0, 1e-6);
    EXPECT_NEAR(use_bounds->max().x(), 110.0, 1e-6);
    EXPECT_NEAR(use_bounds->max().y(), 80.0, 1e-6);

    auto const *group_rect = find_candidate(candidates, "g1r");
    ASSERT_NE(group_rect, nullptr);
    auto const group_bounds = group_rect->path_uu.boundsExact();
    ASSERT_TRUE(group_bounds);
    // Group translate(130,10) applied.
    EXPECT_NEAR(group_bounds->min().x(), 130.0, 1e-6);
    EXPECT_NEAR(group_bounds->min().y(), 10.0, 1e-6);
    EXPECT_NEAR(group_bounds->max().x(), 160.0, 1e-6);
    EXPECT_NEAR(group_bounds->max().y(), 40.0, 1e-6);

    auto const *artistic = find_candidate(candidates, "a1");
    ASSERT_NE(artistic, nullptr);
    auto const artistic_bounds = artistic->path_uu.boundsExact();
    ASSERT_TRUE(artistic_bounds);
    // curve() (the LPE outline), not the skeleton line y = 75.
    EXPECT_NEAR(artistic_bounds->min().y(), 70.0, 1e-6);
    EXPECT_NEAR(artistic_bounds->max().y(), 80.0, 1e-6);
    EXPECT_NEAR(artistic_bounds->min().x(), 120.0, 1e-6);
    EXPECT_NEAR(artistic_bounds->max().x(), 180.0, 1e-6);

    auto const *donut = find_candidate(candidates, "d1");
    ASSERT_NE(donut, nullptr);
    EXPECT_EQ(donut->rule, SF::Rule::EvenOdd);
    auto const *rect = find_candidate(candidates, "r1");
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->rule, SF::Rule::NonZero);
}

TEST_F(SmartFillOpsTest, HiddenAndLockedObjectsNeverBound)
{
    // display:none on the layer hides everything.
    group("L1")->setAttribute("style", "display:none");
    doc->ensureUpToDate();
    EXPECT_TRUE(Inkscape::SmartFillOps::collect_candidates(doc.get()).empty());

    // visibility:hidden on one item removes only that item.
    use(kDoc);
    item("r1")->setAttribute("style", "fill:#ff0000;stroke:none;visibility:hidden");
    doc->ensureUpToDate();
    auto candidates = Inkscape::SmartFillOps::collect_candidates(doc.get());
    EXPECT_EQ(candidates.size(), 7u);
    EXPECT_EQ(find_candidate(candidates, "r1"), nullptr);

    // A locked layer locks its whole subtree.
    use(kDoc);
    group("L1")->setAttribute("sodipodi:insensitive", "true");
    doc->ensureUpToDate();
    EXPECT_TRUE(Inkscape::SmartFillOps::collect_candidates(doc.get()).empty());

    // A hidden group hides its children.
    use(kDoc);
    auto *xml_doc = doc->getReprDoc();
    auto *hidden_group = xml_doc->createElement("svg:g");
    hidden_group->setAttribute("id", "hiddenGroup");
    hidden_group->setAttribute("style", "display:none");
    auto *hidden_rect = xml_doc->createElement("svg:rect");
    hidden_rect->setAttribute("id", "hgRect");
    hidden_rect->setAttribute("x", "120");
    hidden_rect->setAttribute("y", "10");
    hidden_rect->setAttribute("width", "10");
    hidden_rect->setAttribute("height", "10");
    hidden_group->appendChild(hidden_rect);
    Inkscape::GC::release(hidden_rect);
    group("L1")->getRepr()->appendChild(hidden_group);
    Inkscape::GC::release(hidden_group);
    doc->ensureUpToDate();
    candidates = Inkscape::SmartFillOps::collect_candidates(doc.get());
    EXPECT_EQ(candidates.size(), 8u);
    EXPECT_EQ(find_candidate(candidates, "hgRect"), nullptr);

    // Items in defs are never boundaries (the fixture already has deftile).
    use(kDoc);
    candidates = Inkscape::SmartFillOps::collect_candidates(doc.get());
    EXPECT_EQ(candidates.size(), 8u);
    EXPECT_EQ(find_candidate(candidates, "deftile"), nullptr);
    EXPECT_EQ(find_candidate(candidates, "tile"), nullptr);
}

// -------------------------------------------------------------------------

TEST_F(SmartFillOpsTest, CreateDefaultStyleAndOneUndoStep)
{
    // Evaluating the artistic media effect rewrites a1's d once. Warm it up,
    // so the undo comparison measures only the Smart Fill step.
    Inkscape::SmartFillOps::collect_candidates(doc.get());
    std::string const before = xml();

    auto const result = create_at(45, 25);
    ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
    ASSERT_NE(result.path, nullptr);
    EXPECT_TRUE(result.error.empty());

    auto *layer = group("L1");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(result.path->parent, layer);
    EXPECT_EQ(layer->getRepr()->lastChild(), result.path->getRepr());
    EXPECT_STREQ(result.path->getAttribute("inkscape:label"), "Smart Fill");

    auto const style = parse_style(result.path->getAttribute("style"));
    EXPECT_EQ(style_value(style, "fill"), "#00ff00"); // c1, the topmost source
    EXPECT_EQ(style_value(style, "fill-opacity"), "1");
    EXPECT_EQ(style_value(style, "fill-rule"), "evenodd");
    EXPECT_EQ(style_value(style, "stroke"), "none");

    auto const *d = result.path->getAttribute("d");
    ASSERT_NE(d, nullptr);
    std::string const data(d);
    EXPECT_EQ(data.find_first_of("Aa"), std::string::npos) << "arcs must be written as beziers";
    auto const region = Geom::parse_svg_path(data.c_str());
    expect_bbox(region, 40, 15, 50, 35);
    EXPECT_NEAR(SF::region_area(region), 157.080, 1.5708); // pi * 100 / 2, 1 %

    std::string const created_id = result.path->getId();
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get()));
    EXPECT_NE(doc->getObjectById(created_id.c_str()), nullptr);
}

TEST_F(SmartFillOpsTest, StylePrecedence)
{
    // No source covers the donut hole: the action fallback is used.
    {
        auto const result = create_at(90, 30);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "fill"), "#000000");
    }

    // A configured fallback wins over the default.
    {
        use(kDoc);
        Options options;
        options.fallback_fill = "#123456";
        auto const result = create_at(90, 30, options);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "fill"), "#123456");
    }

    // Explicit fill colours win over the source.
    {
        use(kDoc);
        Options options;
        options.fill = "#abcdef";
        auto const result = create_at(45, 25, options);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        EXPECT_EQ(style_value(parse_style(result.path->getAttribute("style")), "fill"), "#abcdef");
    }

    // The source's fill-opacity travels with its colour.
    {
        use(kDoc);
        item("c1")->setAttribute("style", "fill:#00ff00;fill-opacity:0.5;stroke:none");
        doc->ensureUpToDate();
        auto const result = create_at(45, 25);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "fill"), "#00ff00");
        EXPECT_EQ(style_value(style, "fill-opacity"), "0.5");
    }

    // fill=none is literal.
    {
        use(kDoc);
        Options options;
        options.fill = "none";
        auto const result = create_at(45, 25, options);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        EXPECT_EQ(style_value(parse_style(result.path->getAttribute("style")), "fill"), "none");
    }

    // A paint-server source falls back.
    {
        use(kDoc);
        auto *gradient = doc->getReprDoc()->createElement("svg:linearGradient");
        gradient->setAttribute("id", "grad1");
        doc->getDefs()->getRepr()->appendChild(gradient);
        Inkscape::GC::release(gradient);
        item("c1")->setAttribute("style", "fill:url(#grad1);stroke:none");
        doc->ensureUpToDate();
        auto const result = create_at(45, 25);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        EXPECT_EQ(style_value(parse_style(result.path->getAttribute("style")), "fill"), "#000000");
    }

    // The source has no stroke: stroke=source copies "none" faithfully.
    {
        use(kDoc);
        Options options;
        options.stroke = "source";
        auto const result = create_at(45, 25, options);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "stroke"), "none");
        EXPECT_TRUE(style.find("stroke-width") == style.end());
    }

    // A flat source stroke is copied with its visual width.
    {
        use(kDoc);
        item("c1")->setAttribute("style", "fill:#00ff00;stroke:#ff0000;stroke-width:2");
        doc->ensureUpToDate();
        Options options;
        options.stroke = "source";
        auto const result = create_at(45, 25, options);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        auto const style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "stroke"), "#ff0000");
        EXPECT_EQ(style_value(style, "stroke-opacity"), "1");
        EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 2.0, 1e-6);
    }

    // Explicit outlines default to 0.25 mm, and stroke-width overrides it.
    {
        use(kDoc);
        Options options;
        options.stroke = "#0000ff";
        auto result = create_at(45, 25, options);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        auto style = parse_style(result.path->getAttribute("style"));
        EXPECT_EQ(style_value(style, "stroke"), "#0000ff");
        EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 0.25, 1e-6);

        use(kDoc);
        options.stroke_width_mm = 3.0;
        result = create_at(45, 25, options);
        ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
        style = parse_style(result.path->getAttribute("style"));
        EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 3.0, 1e-6);
    }
}

// -------------------------------------------------------------------------

TEST_F(SmartFillOpsTest, CoordinatesUseDocumentUserUnits)
{
    use(kScaledDoc);
    auto const result = create_at(100, 100);
    ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
    expect_bbox(pathvector_of(result.path), 40, 40, 200, 160);

    use(kScaledDoc);
    Options options;
    options.stroke = "#000000";
    options.stroke_width_mm = 1.0;
    auto const stroked = create_at(100, 100, options);
    ASSERT_EQ(stroked.status, SF::Status::Ok) << stroked.error;
    auto const style = parse_style(stroked.path->getAttribute("style"));
    // 1 mm = 4 user units in this 0.25 mm/uu document.
    EXPECT_NEAR(number_of(style_value(style, "stroke-width")), 4.0, 1e-6);
}

TEST_F(SmartFillOpsTest, ParentTransformIsApplied)
{
    Options options;
    options.parent = std::string("g1");
    auto const result = create_at(145, 25, options);
    ASSERT_EQ(result.status, SF::Status::Ok) << result.error;

    auto *g1 = item("g1");
    ASSERT_NE(g1, nullptr);
    EXPECT_EQ(result.path->parent, g1);
    EXPECT_EQ(g1->getRepr()->lastChild(), result.path->getRepr());

    auto const local = pathvector_of(result.path);
    expect_bbox(local, 10, 10, 20, 20); // g1-local
    expect_bbox(local * Geom::Translate(130, 10), 140, 20, 150, 30); // page
}

// -------------------------------------------------------------------------

TEST_F(SmartFillOpsTest, ParseOptionsErrors)
{
    std::string error;

    EXPECT_FALSE(Inkscape::SmartFillOps::parse_options("shape=x", error));
    EXPECT_EQ(error, "unknown option 'shape'");
    EXPECT_FALSE(Inkscape::SmartFillOps::parse_options("fill=blue", error));
    EXPECT_EQ(error, "bad color 'blue'");
    EXPECT_FALSE(Inkscape::SmartFillOps::parse_options("stroke=#12345", error));
    EXPECT_EQ(error, "bad color '#12345'");
    EXPECT_FALSE(Inkscape::SmartFillOps::parse_options("stroke-width=abc", error));
    EXPECT_EQ(error, "bad number 'abc'");
    EXPECT_FALSE(Inkscape::SmartFillOps::parse_options("stroke-width=0", error));
    EXPECT_EQ(error, "stroke-width must be > 0 and <= 100 mm");
    EXPECT_FALSE(Inkscape::SmartFillOps::parse_options("stroke-width=101", error));
    EXPECT_EQ(error, "stroke-width must be > 0 and <= 100 mm");

    auto const options = Inkscape::SmartFillOps::parse_options(
        "fill=#ABCDEF,stroke=none,stroke-width=1.5,id=sf1,parent=g1", error);
    ASSERT_TRUE(options.has_value()) << error;
    EXPECT_EQ(options->fill, "#ABCDEF");
    EXPECT_EQ(options->stroke, "none");
    EXPECT_DOUBLE_EQ(options->stroke_width_mm.value_or(0.0), 1.5);
    EXPECT_EQ(options->id.value_or(""), "sf1");
    EXPECT_EQ(options->parent.value_or(""), "g1");
}

TEST_F(SmartFillOpsTest, CreateErrorsLeaveTheDocumentUntouched)
{
    auto const expect_failure = [&](Options const &options, Geom::Point const &point,
                                    std::string const &message) {
        std::string const before = xml();
        auto const result = Inkscape::SmartFillOps::create(doc.get(), point, options, nullptr);
        EXPECT_EQ(result.path, nullptr) << message;
        EXPECT_EQ(result.error, message);
        EXPECT_EQ(xml(), before) << message;
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get())) << message;
    };

    Options wrong_parent;
    wrong_parent.parent = std::string("r1");
    expect_failure(wrong_parent, {45, 25}, "smart-fill: parent 'r1' is not a group");

    Options unknown_parent;
    unknown_parent.parent = std::string("nosuch");
    expect_failure(unknown_parent, {45, 25}, "smart-fill: parent 'nosuch' is not a group");

    Options duplicate_id;
    duplicate_id.id = std::string("c1");
    expect_failure(duplicate_id, {45, 25}, "smart-fill: id 'c1' already exists");

    expect_failure({}, {10, 25},
                   "smart-fill: point (10.000, 25.000) lies on an object boundary");
    expect_failure({}, {25, 75}, "smart-fill: no enclosed area at (25.000, 75.000)");
    expect_failure({}, {65, 75}, "smart-fill: no enclosed area at (65.000, 75.000)");
    expect_failure({}, {190, 95}, "smart-fill: no enclosed area at (190.000, 95.000)");
}

// -------------------------------------------------------------------------

TEST_F(SmartFillOpsTest, TextGlyphsAreBoundaries)
{
    use(kTextDoc);
    auto const candidates = Inkscape::SmartFillOps::collect_candidates(doc.get());
    ASSERT_EQ(candidates.size(), 1u);
    ASSERT_EQ(std::string(candidates[0].item->getId()), "text1");
    auto const glyph_bounds = candidates[0].path_uu.boundsExact();
    if (!glyph_bounds || glyph_bounds->width() <= 0.0) {
        GTEST_SKIP() << "no glyph outlines (font unavailable)";
    }

    auto const centre = glyph_bounds->midpoint();
    auto const result = create_at(centre.x(), centre.y());
    ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
    auto const region_bounds = pathvector_of(result.path).boundsExact();
    ASSERT_TRUE(region_bounds);
    EXPECT_GE(region_bounds->min().x(), glyph_bounds->min().x() - 0.1);
    EXPECT_GE(region_bounds->min().y(), glyph_bounds->min().y() - 0.1);
    EXPECT_LE(region_bounds->max().x(), glyph_bounds->max().x() + 0.1);
    EXPECT_LE(region_bounds->max().y(), glyph_bounds->max().y() + 0.1);
    // The text is the source (its flat fill is copied).
    EXPECT_EQ(style_value(parse_style(result.path->getAttribute("style")), "fill"), "#00ff00");
}

TEST_F(SmartFillOpsTest, FlowtextIsOneBoundary)
{
    use(kFlowDoc);
    auto const candidates = Inkscape::SmartFillOps::collect_candidates(doc.get());
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(std::string(candidates[0].item->getId()), "flow1");
    if (candidates[0].path_uu.empty()) {
        GTEST_SKIP() << "no flowtext glyph outlines (font unavailable)";
    }
    // The flowRegion rectangle is not a candidate of its own.
    EXPECT_EQ(find_candidate(candidates, "flowrect"), nullptr);
}

TEST_F(SmartFillOpsTest, ImageRectangleIsABoundary)
{
    use(kImageDoc);
    auto const candidates = Inkscape::SmartFillOps::collect_candidates(doc.get());
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(std::string(candidates[0].item->getId()), "img1");

    auto const result = create_at(20, 65);
    ASSERT_EQ(result.status, SF::Status::Ok) << result.error;
    expect_bbox(pathvector_of(result.path), 10, 60, 30, 70);
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
