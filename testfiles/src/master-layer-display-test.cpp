// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the display copies of master layers on pages after the first.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>

#include <2geom/point.h>
#include <2geom/transforms.h>

#include <src/actions/actions-page-manager.h>
#include <src/document.h>
#include <src/inkscape.h>
#include <src/object/master-layer-display.h>
#include <src/object/master-layer.h>
#include <src/object/sp-item-group.h>
#include <src/object/sp-item.h>
#include <src/object/sp-page.h>
#include <src/object/sp-rect.h>
#include <src/object/sp-root.h>
#include <src/page-manager.h>
#include <src/util/cast.h>
#include <src/xml/repr.h>

#include "display/drawing-group.h"
#include "display/drawing-item.h"
#include "display/drawing.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

constexpr auto kMasterDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="370mm" height="100mm" viewBox="0 0 370 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="p1" viewBox="0 0 100 100" inkscape:label="One"/>
    <view id="p2" viewBox="110 0 100 100" inkscape:label="Two"/>
    <view id="p3" viewBox="220 0 150 100" inkscape:label="Three"/>
  </defs>
  <g inkscape:groupmode="layer" inkscape:master-layer="true" inkscape:label="Master BG" id="master-bg">
    <rect id="bg" x="5" y="5" width="90" height="90" fill="#33cc33"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="Content" id="content">
    <rect id="c1" x="30" y="30" width="20" height="20" fill="#cc3333"/>
    <rect id="c2" x="140" y="30" width="20" height="20" fill="#3333cc"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:master-layer="true" inkscape:label="Master Top" id="master-top">
    <rect id="hdr" x="10" y="10" width="80" height="8" fill="#222288"/>
    <rect id="badge" x="40" y="40" width="6" height="6" fill="#ffcc00"/>
  </g>
</svg>)A"sv;

} // namespace

class MasterLayerDisplayTest : public ::testing::Test
{
public:
    static void SetUpTestCase()
    {
        Inkscape::Application::create(false);
    }

    void SetUp() override
    {
        doc = SPDocument::createNewDocFromMem(kMasterDoc);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    void TearDown() override
    {
        display.reset();
        if (shown) {
            root->invoke_hide(key);
        }
        drawing.reset();
        doc.reset();
    }

    void show()
    {
        doc->ensureUpToDate();
        root = doc->getRoot();
        drawing = std::make_unique<Drawing>();
        key = SPItem::display_key_new(1);
        auto *rootitem = root->invoke_show(*drawing, key, SP_ITEM_SHOW_DISPLAY);
        ASSERT_NE(rootitem, nullptr);
        drawing->setRoot(rootitem);
        drawing->update();
        shown = true;

        display = std::make_unique<MasterLayerDisplay>(*doc, key);
        drawing->update();
    }

    SPGroup *layer(char const *id) const { return cast<SPGroup>(doc->getObjectById(id)); }
    SPItem *item(char const *id) const { return cast<SPItem>(doc->getObjectById(id)); }
    SPPage *page(int index) const { return doc->getPageManager().getPage(index); }

    DrawingGroup *layer_group(char const *id) const
    {
        auto *obj = cast<SPItem>(doc->getObjectById(id));
        auto *arena = obj ? obj->get_arenaitem(key) : nullptr;
        return dynamic_cast<DrawingGroup *>(arena);
    }

    std::size_t direct_item_count(char const *id) const
    {
        std::size_t count = 0;
        for (auto &child : layer(id)->children) {
            if (is<SPItem>(&child)) {
                ++count;
            }
        }
        return count;
    }

    bool is_last_child(const DrawingGroup *group, const DrawingItem *child) const
    {
        testing::internal::CaptureStdout();
        group->recursivePrintTree(0);
        auto const tree = testing::internal::GetCapturedStdout();

        std::string const prefix = "DI:   ";
        std::string last;
        std::istringstream stream(tree);
        for (std::string line; std::getline(stream, line);) {
            if (line.rfind(prefix, 0) == 0 && line.rfind("DI:     ", 0) != 0) {
                last = line;
            }
        }
        return last.find(child->name().raw()) != std::string::npos;
    }

    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<Drawing> drawing;
    std::unique_ptr<MasterLayerDisplay> display;
    SPRoot *root = nullptr;
    unsigned key = 0;
    bool shown = false;
};

TEST_F(MasterLayerDisplayTest, copiesForEveryPage)
{
    show();

    auto const &copies = display->copies();
    ASSERT_EQ(copies.size(), 4u);

    char const *const expected_layers[] = {"master-bg", "master-bg", "master-top", "master-top"};
    Geom::Point const expected_offsets[] = {{110, 0}, {220, 0}, {110, 0}, {220, 0}};

    for (std::size_t i = 0; i < copies.size(); ++i) {
        auto const &copy = copies[i];
        ASSERT_NE(copy.layer, nullptr);
        ASSERT_NE(copy.wrapper, nullptr);
        EXPECT_STREQ(copy.layer->getId(), expected_layers[i]);
        EXPECT_EQ(copy.wrapper->parent(), layer_group(expected_layers[i]));
        EXPECT_FALSE(copy.wrapper->sensitive());
        EXPECT_EQ(copy.items.size(), direct_item_count(expected_layers[i]));

        // The wrapper is the last child of the layer's drawing group.
        EXPECT_TRUE(is_last_child(layer_group(expected_layers[i]), copy.wrapper));

        // Identity layer transform: the wrapper maps (10, 20) by the page offset.
        auto const mapped = Geom::Point(10, 20) * copy.wrapper->transform();
        EXPECT_NEAR(mapped.x(), 10 + expected_offsets[i].x(), 1e-9);
        EXPECT_NEAR(mapped.y(), 20 + expected_offsets[i].y(), 1e-9);

        // Every copied child got its own view with the wrapper's key.
        for (auto *child : copy.items) {
            EXPECT_NE(child->get_arenaitem(copy.key), nullptr);
        }
    }
}

TEST_F(MasterLayerDisplayTest, wrapperTransformUsesLayerTransform)
{
    layer("master-bg")->setAttribute("transform", "translate(3,4)");
    doc->ensureUpToDate();
    show();

    auto const &copies = display->copies();
    ASSERT_EQ(copies.size(), 4u);
    ASSERT_STREQ(copies[0].layer->getId(), "master-bg");

    // W = L * T(offset) * L^-1 with a pure translation L commutes, so the
    // wrapper is the page offset and NOT L * T(offset) (which would add 3,4).
    auto const mapped = Geom::Point(10, 20) * copies[0].wrapper->transform();
    EXPECT_NEAR(mapped.x(), 120.0, 1e-9);
    EXPECT_NEAR(mapped.y(), 20.0, 1e-9);
}

TEST_F(MasterLayerDisplayTest, refreshAfterPageInsertAndMasterClear)
{
    show();
    ASSERT_EQ(display->copies().size(), 4u);

    ASSERT_NE(PageOps::insert_page(doc.get(), PageOps::Where::End), nullptr);
    doc->ensureUpToDate();
    display->refresh();

    ASSERT_EQ(display->copies().size(), 6u);
    EXPECT_EQ(display->copies()[4].layer, layer("master-top"));
    for (auto const &copy : display->copies()) {
        EXPECT_EQ(copy.wrapper->parent(), layer_group(copy.layer->getId()));
    }

    ASSERT_TRUE(MasterLayer::set_master(layer("master-top"), false));
    doc->ensureUpToDate();
    display->refresh();

    ASSERT_EQ(display->copies().size(), 3u);
    for (auto const &copy : display->copies()) {
        EXPECT_EQ(copy.layer, layer("master-bg"));
    }
}

TEST_F(MasterLayerDisplayTest, appendedChildGetsCopied)
{
    show();
    ASSERT_EQ(display->copies().size(), 4u);

    auto *repr = doc->getReprDoc()->createElement("svg:rect");
    repr->setAttribute("id", "extra");
    repr->setAttribute("x", "60");
    repr->setAttribute("y", "60");
    repr->setAttribute("width", "10");
    repr->setAttribute("height", "10");
    repr->setAttribute("fill", "#000000");
    layer("master-top")->appendChild(repr);
    doc->ensureUpToDate();
    display->refresh();

    auto const *extra = cast<SPItem>(doc->getObjectById("extra"));
    ASSERT_NE(extra, nullptr);

    ASSERT_EQ(display->copies().size(), 4u);
    auto const &copy = display->copies()[2];
    EXPECT_EQ(copy.layer, layer("master-top"));
    EXPECT_EQ(copy.items.size(), 3u);
    EXPECT_EQ(copy.items.back(), extra);
    EXPECT_NE(extra->get_arenaitem(copy.key), nullptr);
}

TEST_F(MasterLayerDisplayTest, deletedMasterLayerLeavesOtherCopies)
{
    show();
    ASSERT_EQ(display->copies().size(), 4u);

    layer("master-top")->deleteObject();
    doc->ensureUpToDate();
    display->refresh();

    ASSERT_EQ(display->copies().size(), 2u);
    for (auto const &copy : display->copies()) {
        EXPECT_EQ(copy.layer, layer("master-bg"));
        EXPECT_EQ(copy.wrapper->parent(), layer_group("master-bg"));
    }
}

TEST_F(MasterLayerDisplayTest, pageMoveUpdatesWrappersInPlaceEvenWithSkippedLayer)
{
    show();
    // master-bg has no drawing item for this key (as after invoke_hide_except),
    // so only master-top gets copies. An offset-only change must still update them.
    display.reset();
    layer("master-bg")->invoke_hide(key);
    display = std::make_unique<MasterLayerDisplay>(*doc, key);

    ASSERT_EQ(display->copies().size(), 2u);
    auto const *wrapper = display->copies()[0].wrapper;
    ASSERT_EQ(display->copies()[0].layer, layer("master-top"));

    page(1)->movePage(Geom::Translate(10, 0), false);
    doc->ensureUpToDate();
    display->refresh();

    auto const offsets = MasterLayer::page_offsets(doc.get());
    ASSERT_EQ(offsets.size(), 2u);
    ASSERT_GT(offsets[0].second.x(), 110.0 + 1e-6);
    ASSERT_EQ(display->copies().size(), 2u);
    EXPECT_EQ(display->copies()[0].wrapper, wrapper) << "offset-only change must not rebuild";
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        auto const mapped = Geom::Point(10, 20) * display->copies()[i].wrapper->transform();
        EXPECT_NEAR(mapped.x(), 10 + offsets[i].second.x(), 1e-9);
        EXPECT_NEAR(mapped.y(), 20 + offsets[i].second.y(), 1e-9);
    }
}

TEST_F(MasterLayerDisplayTest, pickOnPageTwoCopyFindsNothing)
{
    show();

    auto const scale = doc->getDocumentScale().vector()[0];
    // Page 2 header: page offset (110, 0) + (43, 14); nothing else is there.
    auto const point = Geom::Point(153, 14) * scale;
    auto const area = Geom::IntRect::from_xywh(-100000, -100000, 200000, 200000);

    DrawingItem *picked = drawing->pick(point, 1.0, area, 0);
    EXPECT_EQ(picked, nullptr);
}

TEST_F(MasterLayerDisplayTest, teardownBeforeRootHide)
{
    show();
    ASSERT_EQ(display->copies().size(), 4u);

    display.reset();
    root->invoke_hide(key);
    shown = false;
    SUCCEED();
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
