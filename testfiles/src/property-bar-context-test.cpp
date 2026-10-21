// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure property-bar context model.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/toolbar/property-bar-context.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "document.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "util/cast.h"

using Inkscape::UI::Toolbar::ObjectKind;
using Inkscape::UI::Toolbar::classify_object;
using Inkscape::UI::Toolbar::common_kind;
using Inkscape::UI::Toolbar::object_section_for;
using Inkscape::UI::Toolbar::object_sections_pref_path;

namespace {

// The only upstream toolbar_data keys an object section may ever return.
constexpr std::string_view known_section_keys[] = {
    "/tools/shapes/rect",
    "/tools/shapes/arc",
    "/tools/shapes/star",
    "/tools/shapes/spiral",
    "/tools/text",
};

// Tools with object sections.
constexpr std::string_view section_tools[] = {
    "/tools/select",
    "/tools/nodes",
};

// Tools without object sections, plus one unknown path.
constexpr std::string_view other_tools[] = {
    "/tools/shapes/rect",
    "/tools/text",
    "/tools/freehand/pen",
    "/tools/zoom",
    "/tools/nonexistent",
};

constexpr ObjectKind section_kinds[] = {
    ObjectKind::Rect, ObjectKind::Ellipse, ObjectKind::Star, ObjectKind::Spiral, ObjectKind::Text,
};

char const *SVG =
    "<svg xmlns='http://www.w3.org/2000/svg' "
    "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' "
    "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
    "xmlns:xlink='http://www.w3.org/1999/xlink'>"
    "<rect id='rect' x='0' y='0' width='10' height='10'/>"
    "<circle id='circle' cx='5' cy='5' r='3'/>"
    "<ellipse id='ellipse' cx='5' cy='5' rx='4' ry='2'/>"
    "<path id='arc' sodipodi:type='arc' sodipodi:cx='5' sodipodi:cy='5' "
    "sodipodi:rx='4' sodipodi:ry='2' sodipodi:start='0' "
    "sodipodi:end='1.5707963267948966' sodipodi:open='true'/>"
    "<path id='star' sodipodi:type='star' sodipodi:sides='5' sodipodi:cx='5' "
    "sodipodi:cy='5' sodipodi:r1='4' sodipodi:r2='2' sodipodi:arg1='0' "
    "sodipodi:arg2='1.2566370614359172' inkscape:flatsided='false' "
    "inkscape:rounded='0' inkscape:randomized='0'/>"
    "<path id='polygonstar' sodipodi:type='star' sodipodi:sides='6' "
    "sodipodi:cx='5' sodipodi:cy='5' sodipodi:r1='4' sodipodi:r2='4' "
    "sodipodi:arg1='0' sodipodi:arg2='1.0471975511965976' "
    "inkscape:flatsided='true' inkscape:rounded='0' inkscape:randomized='0'/>"
    "<path id='spiral' sodipodi:type='spiral' sodipodi:cx='5' sodipodi:cy='5' "
    "sodipodi:expansion='1' sodipodi:revolution='3' sodipodi:radius='4' "
    "sodipodi:argument='1' sodipodi:t0='0'/>"
    "<text id='text' x='1' y='2'>A</text>"
    "<text id='textonpath' x='1' y='2'><textPath xlink:href='#plainpath'>A</textPath></text>"
    "<flowRoot id='flowroot'>"
    "<flowRegion><rect x='0' y='0' width='10' height='10'/></flowRegion>"
    "<flowPara>A</flowPara>"
    "</flowRoot>"
    "<path id='plainpath' d='M 0,0 L 1,1'/>"
    "<g id='group'><rect id='groupedrect' x='0' y='0' width='2' height='2'/></g>"
    "<use id='userect' xlink:href='#rect'/>"
    "<image id='image' x='0' y='0' width='1' height='1'/>"
    "</svg>";

class PropertyBarContextTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Inkscape::Application::create(false);
    }

    void TearDown() override
    {
        _doc.reset();
    }

    void load(char const *svg)
    {
        _doc = SPDocument::createNewDocFromMem(std::span<char const>(svg, std::strlen(svg)));
        ASSERT_TRUE(_doc);
        _doc->ensureUpToDate();
    }

    SPItem const *item(char const *id) const
    {
        auto const result = cast<SPItem>(_doc->getObjectById(id));
        EXPECT_TRUE(result) << id;
        return result;
    }

    std::unique_ptr<SPDocument> _doc;
};

TEST_F(PropertyBarContextTest, selectToolMapsEveryKind)
{
    EXPECT_EQ(object_section_for("/tools/select", ObjectKind::Rect, true), "/tools/shapes/rect");
    EXPECT_EQ(object_section_for("/tools/select", ObjectKind::Ellipse, true), "/tools/shapes/arc");
    EXPECT_EQ(object_section_for("/tools/select", ObjectKind::Star, true), "/tools/shapes/star");
    EXPECT_EQ(object_section_for("/tools/select", ObjectKind::Spiral, true), "/tools/shapes/spiral");
    EXPECT_EQ(object_section_for("/tools/select", ObjectKind::Text, true), "/tools/text");
}

TEST_F(PropertyBarContextTest, nodeToolMapsShapesOnly)
{
    EXPECT_EQ(object_section_for("/tools/nodes", ObjectKind::Rect, true), "/tools/shapes/rect");
    EXPECT_EQ(object_section_for("/tools/nodes", ObjectKind::Ellipse, true), "/tools/shapes/arc");
    EXPECT_EQ(object_section_for("/tools/nodes", ObjectKind::Star, true), "/tools/shapes/star");
    EXPECT_EQ(object_section_for("/tools/nodes", ObjectKind::Spiral, true), "/tools/shapes/spiral");
    EXPECT_EQ(object_section_for("/tools/nodes", ObjectKind::Text, true), "");
}

TEST_F(PropertyBarContextTest, otherToolsMapNothing)
{
    for (auto const tool : other_tools) {
        for (auto const kind : section_kinds) {
            EXPECT_EQ(object_section_for(tool, kind, true), std::string_view{""}) << tool;
        }
    }
}

TEST_F(PropertyBarContextTest, noCommonKindMapsNothing)
{
    for (auto const tool : section_tools) {
        EXPECT_EQ(object_section_for(tool, std::nullopt, true), std::string_view{""}) << tool;
    }
    EXPECT_EQ(object_section_for("/tools/shapes/rect", std::nullopt, true), std::string_view{""});
}

TEST_F(PropertyBarContextTest, disabledGivesNothing)
{
    for (auto const tool : section_tools) {
        for (auto const kind : section_kinds) {
            EXPECT_EQ(object_section_for(tool, kind, false), std::string_view{""}) << tool;
        }
    }
}

TEST_F(PropertyBarContextTest, otherKindMapsNothing)
{
    for (auto const tool : section_tools) {
        EXPECT_EQ(object_section_for(tool, ObjectKind::Other, true), std::string_view{""}) << tool;
    }
}

TEST_F(PropertyBarContextTest, resultsAreKnownToolbarDataKeys)
{
    for (auto const tool : section_tools) {
        for (auto const kind : section_kinds) {
            auto const section = object_section_for(tool, kind, true);
            if (section.empty()) {
                continue;
            }
            bool const known = std::any_of(std::begin(known_section_keys), std::end(known_section_keys),
                                           [&](auto const key) { return key == section; });
            EXPECT_TRUE(known) << tool << " -> " << section;
        }
    }
}

TEST_F(PropertyBarContextTest, commonKindRules)
{
    using K = ObjectKind;
    EXPECT_EQ(common_kind({}), std::nullopt);
    EXPECT_EQ(common_kind({K::Rect}), K::Rect);
    EXPECT_EQ(common_kind({K::Rect, K::Rect}), K::Rect);
    EXPECT_EQ(common_kind({K::Rect, K::Ellipse}), std::nullopt);
    EXPECT_EQ(common_kind({K::Other}), std::nullopt);
    EXPECT_EQ(common_kind({K::Text, K::Other}), std::nullopt);
}

TEST_F(PropertyBarContextTest, classifiesDocumentObjects)
{
    load(SVG);

    std::vector<std::pair<char const *, ObjectKind>> const expected = {
        {"rect", ObjectKind::Rect},         //
        {"circle", ObjectKind::Ellipse},    //
        {"ellipse", ObjectKind::Ellipse},   //
        {"arc", ObjectKind::Ellipse},       //
        {"star", ObjectKind::Star},         //
        {"polygonstar", ObjectKind::Star},  //
        {"spiral", ObjectKind::Spiral},     //
        {"text", ObjectKind::Text},         //
        {"textonpath", ObjectKind::Text},   //
        {"flowroot", ObjectKind::Text},     //
        {"plainpath", ObjectKind::Other},   //
        {"group", ObjectKind::Other},       //
        {"userect", ObjectKind::Other},     //
        {"image", ObjectKind::Other},       //
    };

    for (auto const &[id, kind] : expected) {
        EXPECT_EQ(classify_object(item(id)), kind) << id;
    }
    EXPECT_EQ(classify_object(nullptr), ObjectKind::Other);
}

TEST_F(PropertyBarContextTest, prefPathValue)
{
    EXPECT_STREQ(object_sections_pref_path, "/toolbox/propertybar/object-sections");
}

} // namespace
