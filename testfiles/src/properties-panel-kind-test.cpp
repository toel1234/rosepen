// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure Object Properties panel-kind model.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/dialog/properties-panel-kind.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "document.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "util/cast.h"

using Inkscape::UI::Dialog::PropertiesPanelKind;
using Inkscape::UI::Dialog::properties_panel_for;

namespace {

// The exact sodipodi namespace is required for sodipodi:type dispatch
// (NodeTraits::get_type_string reads the raw attribute value and looks it up
// in SPFactory's element-name map).
constexpr std::string_view SVG =
    "<svg xmlns='http://www.w3.org/2000/svg' "
    "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' "
    "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
    "xmlns:xlink='http://www.w3.org/1999/xlink'>"
    "<defs>"
    "<inkscape:persp3d id='persp1'/>"
    "</defs>"
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
    "<polygon id='polygon' points='0,0 4,0 2,3'/>"
    "<polyline id='polyline' points='0,0 1,1 2,0'/>"
    "<path id='plainpath' d='M 0,0 L 1,1'/>"
    "<a id='anchor'><rect id='anchored' x='0' y='0' width='2' height='2'/></a>"
    "<g id='group'><rect id='groupedrect' x='0' y='0' width='2' height='2'/></g>"
    "<g inkscape:groupmode='layer' id='layer'>"
    "<rect id='layeredrect' x='0' y='0' width='2' height='2'/>"
    "</g>"
    "<text id='text' x='1' y='2'>A</text>"
    "<text id='textonpath' x='1' y='2'><textPath xlink:href='#plainpath'>A</textPath></text>"
    "<use id='use' xlink:href='#rect'/>"
    "<path id='spiral' sodipodi:type='spiral' sodipodi:cx='5' sodipodi:cy='5' "
    "sodipodi:expansion='1' sodipodi:revolution='3' sodipodi:radius='4' "
    "sodipodi:argument='1' sodipodi:t0='0'/>"
    "<flowRoot id='flowroot'>"
    "<flowRegion><rect x='0' y='0' width='10' height='10'/></flowRegion>"
    "<flowPara>A</flowPara>"
    "</flowRoot>"
    "<line id='line' x1='0' y1='0' x2='5' y2='5'/>"
    "<path id='offset' sodipodi:type='inkscape:offset' "
    "inkscape:original='M 0,0 L 10,0 L 10,10 Z' inkscape:radius='2'/>"
    "<switch id='switch'><rect id='switched' x='0' y='0' width='2' height='2'/></switch>"
    "<path id='box3d' sodipodi:type='inkscape:box3d' inkscape:perspectiveID='#persp1'/>"
    "<image id='image' x='0' y='0' width='1' height='1'/>"
    "</svg>";

} // namespace

class PropertiesPanelKindTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Inkscape::Application::create(false);
        load(SVG);
    }

    void TearDown() override { doc.reset(); }

    void load(std::string_view svg)
    {
        doc = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    SPItem const *item(char const *id) const
    {
        auto const result = cast<SPItem>(doc->getObjectById(id));
        EXPECT_TRUE(result) << id;
        return result;
    }

    void expect_kind(char const *id, PropertiesPanelKind kind)
    {
        EXPECT_EQ(properties_panel_for(item(id)), kind) << id;
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(PropertiesPanelKindTest, upstreamKinds)
{
    expect_kind("image", PropertiesPanelKind::Image);
    expect_kind("rect", PropertiesPanelKind::Rect);
    expect_kind("circle", PropertiesPanelKind::Ellipse);
    expect_kind("ellipse", PropertiesPanelKind::Ellipse);
    expect_kind("arc", PropertiesPanelKind::Ellipse);
    expect_kind("star", PropertiesPanelKind::Star);
    expect_kind("polygon", PropertiesPanelKind::Polygon);
    expect_kind("polyline", PropertiesPanelKind::Polyline);
    expect_kind("plainpath", PropertiesPanelKind::Path);
    expect_kind("anchor", PropertiesPanelKind::Anchor);
    expect_kind("group", PropertiesPanelKind::Group);
    expect_kind("layer", PropertiesPanelKind::Group);
    expect_kind("text", PropertiesPanelKind::Text);
    expect_kind("textonpath", PropertiesPanelKind::Text);
    expect_kind("use", PropertiesPanelKind::Clone);
}

TEST_F(PropertiesPanelKindTest, genericKinds)
{
    // Every single-item type upstream has no specific panel for.
    expect_kind("spiral", PropertiesPanelKind::Generic);
    expect_kind("flowroot", PropertiesPanelKind::Generic);
    expect_kind("line", PropertiesPanelKind::Generic);
    expect_kind("offset", PropertiesPanelKind::Generic);
    expect_kind("switch", PropertiesPanelKind::Generic);
    expect_kind("box3d", PropertiesPanelKind::Generic);
}

TEST_F(PropertiesPanelKindTest, nullAndSelectionSizes)
{
    EXPECT_EQ(properties_panel_for(nullptr), PropertiesPanelKind::None);
    EXPECT_EQ(properties_panel_for(0, nullptr), PropertiesPanelKind::Empty);
    EXPECT_EQ(properties_panel_for(3, nullptr), PropertiesPanelKind::Multi);
    EXPECT_EQ(properties_panel_for(1, item("rect")), PropertiesPanelKind::Rect);
    // A single non-item object: the upstream nullptr-panel case.
    EXPECT_EQ(properties_panel_for(1, nullptr), PropertiesPanelKind::None);
}

TEST_F(PropertiesPanelKindTest, tableCoversExactlyTheElevenUpstreamKinds)
{
    // One representative of each of the 11 upstream panel kinds: the model
    // must map them to 11 distinct, non-degenerate kinds.
    std::set<PropertiesPanelKind> kinds;
    for (char const *id : {"image", "rect", "circle", "star", "anchor", "plainpath",
                           "polyline", "polygon", "group", "text", "use"}) {
        auto const kind = properties_panel_for(item(id));
        EXPECT_NE(kind, PropertiesPanelKind::Generic) << id;
        EXPECT_NE(kind, PropertiesPanelKind::None) << id;
        EXPECT_NE(kind, PropertiesPanelKind::Empty) << id;
        EXPECT_NE(kind, PropertiesPanelKind::Multi) << id;
        kinds.insert(kind);
    }
    EXPECT_EQ(kinds.size(), 11u);
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
