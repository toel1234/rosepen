// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the master-layer model and its integration with the page operations.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>

#include <2geom/transforms.h>

#include <src/actions/actions-master-layer.h>
#include <src/actions/actions-page-manager.h>
#include <src/document-undo.h>
#include <src/document.h>
#include <src/inkscape.h>
#include <src/object/master-layer.h>
#include <src/object/object-set.h>
#include <src/object/sp-item-group.h>
#include <src/object/sp-item.h>
#include <src/object/sp-page.h>
#include <src/object/sp-rect.h>
#include <src/object/sp-root.h>
#include <src/object/weakptr.h>
#include <src/page-manager.h>
#include <src/xml/repr.h>

using namespace Inkscape;
using namespace std::literals;

namespace {

constexpr auto kMasterDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="p1" viewBox="0 0 100 100" inkscape:label="One"/>
    <view id="p2" viewBox="110 0 100 100" inkscape:label="Two"/>
    <view id="p3" viewBox="220 0 150 100" inkscape:label="Three"/>
  </defs>
  <g inkscape:groupmode="layer" inkscape:label="Master BG" id="master-bg">
    <rect id="bg" x="5" y="5" width="90" height="90" fill="#33cc33"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="Content" id="content">
    <rect id="c1" x="30" y="30" width="20" height="20" fill="#cc3333"/>
    <rect id="c2" x="140" y="30" width="20" height="20" fill="#3333cc"/>
    <g inkscape:groupmode="layer" inkscape:label="Sub" id="content-sub"/>
    <g id="grp"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="Master Top" id="master-top">
    <rect id="hdr" x="10" y="10" width="80" height="8" fill="#222288"/>
    <rect id="badge" x="40" y="40" width="6" height="6" fill="#ffcc00"/>
  </g>
</svg>)A"sv;

constexpr auto kZeroPageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <g inkscape:groupmode="layer" inkscape:label="Master" id="master"/>
</svg>)A"sv;

constexpr auto kOnePageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="only" viewBox="0 0 100 100" inkscape:label="Only"/>
  </defs>
  <g inkscape:groupmode="layer" inkscape:label="Master" id="master"/>
</svg>)A"sv;

} // namespace

class MasterLayerTest : public ::testing::Test
{
public:
    static void SetUpTestCase()
    {
        Inkscape::Application::create(false);
    }

    void SetUp() override
    {
        use(kMasterDoc);
    }

    void TearDown() override { doc.reset(); }

    void use(std::string_view source)
    {
        doc.reset();
        doc = SPDocument::createNewDocFromMem(source);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    SPGroup *layer(char const *id) const { return cast<SPGroup>(doc->getObjectById(id)); }
    SPItem *item(char const *id) const { return cast<SPItem>(doc->getObjectById(id)); }
    SPRect *rect(char const *id) const { return cast<SPRect>(doc->getObjectById(id)); }
    SPPage *page(int index) const { return doc->getPageManager().getPage(index); }

    Geom::Rect bounds(SPItem *item) const
    {
        auto box = item->documentGeometricBounds();
        EXPECT_TRUE(box.has_value()) << "item has no bounding box";
        return box ? *box : Geom::Rect();
    }

    void flag_masters()
    {
        ASSERT_TRUE(MasterLayer::set_master(layer("master-bg"), true));
        ASSERT_TRUE(MasterLayer::set_master(layer("master-top"), true));
        doc->ensureUpToDate();
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(MasterLayerTest, setAndClearMaster)
{
    auto *bg = layer("master-bg");
    ASSERT_NE(bg, nullptr);
    EXPECT_FALSE(MasterLayer::is_master(bg));

    EXPECT_TRUE(MasterLayer::set_master(bg, true));
    EXPECT_TRUE(MasterLayer::is_master(bg));
    EXPECT_STREQ(bg->getAttribute("inkscape:master-layer"), "true");

    // Setting an already-set layer reports no change (and does not undo).
    EXPECT_FALSE(MasterLayer::set_master(bg, true));

    EXPECT_TRUE(MasterLayer::set_master(bg, false));
    EXPECT_FALSE(MasterLayer::is_master(bg));
    EXPECT_EQ(bg->getAttribute("inkscape:master-layer"), nullptr);

    EXPECT_FALSE(MasterLayer::set_master(bg, false));
}

TEST_F(MasterLayerTest, setMasterRefusesNonLayers)
{
    EXPECT_FALSE(MasterLayer::set_master(nullptr, true));
    EXPECT_FALSE(MasterLayer::set_master(layer("content-sub"), true));
    EXPECT_FALSE(MasterLayer::set_master(layer("grp"), true));

    EXPECT_FALSE(MasterLayer::is_master(doc->getObjectById("content-sub")));
    EXPECT_FALSE(MasterLayer::is_master(doc->getObjectById("grp")));
    EXPECT_FALSE(MasterLayer::is_master(rect("c1")));
}

TEST_F(MasterLayerTest, saveReloadKeepsFlag)
{
    ASSERT_TRUE(MasterLayer::set_master(layer("master-bg"), true));

    auto const saved = sp_repr_save_buf(doc->getReprDoc());
    auto reloaded = SPDocument::createNewDocFromMem(std::span<char const>(saved.data(), saved.bytes()));
    ASSERT_TRUE(reloaded);
    reloaded->ensureUpToDate();

    EXPECT_TRUE(MasterLayer::is_master(reloaded->getObjectById("master-bg")));
    EXPECT_FALSE(MasterLayer::is_master(reloaded->getObjectById("master-top")));
}

TEST_F(MasterLayerTest, masterLayersInDocumentOrder)
{
    flag_masters();

    auto layers = MasterLayer::master_layers(doc.get());
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_STREQ(layers[0]->getId(), "master-bg");
    EXPECT_STREQ(layers[1]->getId(), "master-top");

    EXPECT_TRUE(MasterLayer::in_master(item("badge")));
    EXPECT_TRUE(MasterLayer::in_master(layer("master-bg")));
    EXPECT_FALSE(MasterLayer::in_master(item("c1")));
}

TEST_F(MasterLayerTest, withoutMasterDropsWholeLayers)
{
    flag_masters();

    auto items = doc->getRoot()->item_list();
    ASSERT_EQ(items.size(), 3u);
    auto filtered = MasterLayer::without_master(items);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0], item("content"));
    EXPECT_FALSE(MasterLayer::in_master(filtered[0]));
}

TEST_F(MasterLayerTest, pageOffsets)
{
    auto offsets = MasterLayer::page_offsets(doc.get());
    ASSERT_EQ(offsets.size(), 2u);
    EXPECT_EQ(offsets[0].first, page(1));
    EXPECT_EQ(offsets[1].first, page(2));
    EXPECT_NEAR(offsets[0].second.x(), 110.0, 1e-6);
    EXPECT_NEAR(offsets[0].second.y(), 0.0, 1e-6);
    EXPECT_NEAR(offsets[1].second.x(), 220.0, 1e-6);
    EXPECT_NEAR(offsets[1].second.y(), 0.0, 1e-6);

    use(kZeroPageDoc);
    EXPECT_TRUE(MasterLayer::page_offsets(doc.get()).empty());

    use(kOnePageDoc);
    EXPECT_TRUE(MasterLayer::page_offsets(doc.get()).empty());
}

TEST_F(MasterLayerTest, deleteOtherPageKeepsMasters)
{
    flag_masters();
    auto &pm = doc->getPageManager();

    pm.deletePage(page(1), true);
    doc->ensureUpToDate();

    EXPECT_EQ(doc->getObjectById("c2"), nullptr);
    EXPECT_NE(doc->getObjectById("c1"), nullptr);
    EXPECT_NE(doc->getObjectById("bg"), nullptr);
    EXPECT_NE(doc->getObjectById("hdr"), nullptr);
    EXPECT_NE(doc->getObjectById("badge"), nullptr);
    EXPECT_EQ(pm.getPageCount(), 2);
}

TEST_F(MasterLayerTest, deleteFirstPageReanchorsMasters)
{
    flag_masters();
    auto &pm = doc->getPageManager();

    auto *first = page(0);
    auto *bg = layer("master-bg");
    auto *top = layer("master-top");
    auto const bg_before = bounds(bg);
    auto const top_before = bounds(top);
    auto const first_before = first->getDesktopRect();

    pm.deletePage(first, true);
    doc->ensureUpToDate();

    EXPECT_EQ(doc->getObjectById("c1"), nullptr);
    EXPECT_NE(doc->getObjectById("c2"), nullptr);

    auto *new_first = pm.getFirstPage();
    ASSERT_NE(new_first, nullptr);
    EXPECT_EQ(new_first, page(0));
    auto const first_after = new_first->getDesktopRect();

    // The re-anchored bbox relative to the new first page must equal the old
    // relative bbox. The tolerance is 0.01 user units, converted to the
    // desktop units used by the bbox query.
    double const tolerance = 0.01 * doc->getDocumentScale().vector()[0];

    auto const bg_after = bounds(bg);
    auto const top_after = bounds(top);

    auto const bg_rel_before = bg_before.min() - first_before.min();
    auto const bg_rel_after = bg_after.min() - first_after.min();
    EXPECT_NEAR(bg_rel_after.x(), bg_rel_before.x(), tolerance);
    EXPECT_NEAR(bg_rel_after.y(), bg_rel_before.y(), tolerance);

    auto const top_rel_before = top_before.min() - first_before.min();
    auto const top_rel_after = top_after.min() - first_after.min();
    EXPECT_NEAR(top_rel_after.x(), top_rel_before.x(), tolerance);
    EXPECT_NEAR(top_rel_after.y(), top_rel_before.y(), tolerance);

    // The content that lived on the old first page is gone; the surviving page
    // content keeps its page-relative position (30 user units, in desktop units).
    EXPECT_NEAR((bounds(rect("c2")).min() - first_after.min()).x(),
                30.0 * doc->getDocumentScale().vector()[0], tolerance);
}

TEST_F(MasterLayerTest, duplicatePageCopiesContentOnly)
{
    flag_masters();
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    auto *new_page = pm.duplicatePage();
    ASSERT_NE(new_page, nullptr);
    doc->ensureUpToDate();

    EXPECT_EQ(layer("master-bg")->getItemCount(), 1);
    EXPECT_EQ(layer("master-top")->getItemCount(), 2);
    EXPECT_EQ(MasterLayer::master_layers(doc.get()).size(), 2u);

    auto items = new_page->getOverlappingItems();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_NE(items[0], item("c1"));
    EXPECT_TRUE(is<SPRect>(items[0]));
    EXPECT_FALSE(MasterLayer::in_master(items[0]));
}

TEST_F(MasterLayerTest, reorderKeepsMastersAtPositionOne)
{
    flag_masters();

    auto *p1 = page(0);
    auto *bg = layer("master-bg");
    auto *c1 = rect("c1");
    auto const bg_before = bounds(bg);
    auto const c1_before = bounds(c1);

    ASSERT_TRUE(PageOps::move_to(doc.get(), p1, 2));
    doc->ensureUpToDate();

    EXPECT_EQ(doc->getPageManager().getPage(1), p1);
    EXPECT_EQ(doc->getPageManager().getPageFor(c1, false), p1);

    auto const bg_after = bounds(bg);
    EXPECT_NEAR(bg_after.min().x(), bg_before.min().x(), 1e-6);
    EXPECT_NEAR(bg_after.min().y(), bg_before.min().y(), 1e-6);
    EXPECT_NEAR(bg_after.max().x(), bg_before.max().x(), 1e-6);

    // The content on page 1 follows the page to its new location.
    auto const c1_after = bounds(c1);
    EXPECT_NEAR(c1_after.min().x() - c1_before.min().x(), 110.0 * doc->getDocumentScale().vector()[0], 1e-6);
}

TEST_F(MasterLayerTest, dragFirstPageCarriesMasters)
{
    flag_masters();

    auto *p1 = page(0);
    auto *bg = layer("master-bg");
    auto const bg_before = bounds(bg);

    p1->movePage(Geom::Translate(5, 0), true);
    doc->ensureUpToDate();

    auto const bg_after = bounds(bg);
    EXPECT_NEAR(bg_after.min().x() - bg_before.min().x(), 5.0, 1e-6);
    EXPECT_NEAR(bg_after.min().y() - bg_before.min().y(), 0.0, 1e-6);
}

TEST_F(MasterLayerTest, dragOtherPageLeavesMasters)
{
    flag_masters();

    auto *p2 = page(1);
    auto *bg = layer("master-bg");
    auto *c2 = rect("c2");
    auto const bg_before = bounds(bg);
    auto const c2_before = bounds(c2);

    p2->movePage(Geom::Translate(5, 0), true);
    doc->ensureUpToDate();

    auto const bg_after = bounds(bg);
    EXPECT_NEAR(bg_after.min().x(), bg_before.min().x(), 1e-6);
    EXPECT_NEAR(bg_after.min().y(), bg_before.min().y(), 1e-6);

    auto const c2_after = bounds(c2);
    EXPECT_NEAR(c2_after.min().x() - c2_before.min().x(), 5.0, 1e-6);
}

TEST_F(MasterLayerTest, rotatePageLeavesMasters)
{
    flag_masters();
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    auto *bg = layer("master-bg");
    auto *top = layer("master-top");
    auto const bg_before = bounds(bg);
    auto const top_before = bounds(top);

    pm.rotatePage(1);
    doc->ensureUpToDate();

    auto const bg_after = bounds(bg);
    EXPECT_NEAR(bg_after.min().x(), bg_before.min().x(), 1e-6);
    EXPECT_NEAR(bg_after.min().y(), bg_before.min().y(), 1e-6);
    EXPECT_NEAR(bg_after.max().x(), bg_before.max().x(), 1e-6);

    auto const top_after = bounds(top);
    EXPECT_NEAR(top_after.min().x(), top_before.min().x(), 1e-6);
    EXPECT_NEAR(top_after.max().y(), top_before.max().y(), 1e-6);
}

TEST_F(MasterLayerTest, fitToSelectionIgnoresMasters)
{
    flag_masters();
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    ObjectSet selection(doc.get());
    pm.fitToSelection(&selection, false);
    doc->ensureUpToDate();

    auto *p1 = pm.getPage(0);
    ASSERT_NE(p1, nullptr);
    // The page fits c1 (20 x 20), not the master rectangles (90 x 90).
    EXPECT_NEAR(p1->getRect().width(), 20.0, 0.01);
    EXPECT_NEAR(p1->getRect().height(), 20.0, 0.01);
}

TEST_F(MasterLayerTest, listFormatterMatchesFixture)
{
    flag_masters();

    EXPECT_EQ(Inkscape::master_layer_list(doc.get()),
              "layer\tmaster-bg\tMaster BG\t1\tvisible\n"
              "layer\tmaster-top\tMaster Top\t3\tvisible\n"
              "offset\t2\t110.000\t0.000\n"
              "offset\t3\t220.000\t0.000\n");
}

TEST_F(MasterLayerTest, undoRestoresMasterFlag)
{
    auto *bg = layer("master-bg");
    ASSERT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));
    ASSERT_FALSE(MasterLayer::is_master(bg));

    ASSERT_TRUE(MasterLayer::set_master(bg, true));
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Set master layer"), "");
    ASSERT_TRUE(MasterLayer::is_master(bg));

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_FALSE(MasterLayer::is_master(bg));
}

TEST_F(MasterLayerTest, pageExportItemsIncludesMasters)
{
    flag_masters();

    // Page 2 overlaps none of the master content, so both master layers are
    // appended (in document order) behind the page's own items.
    auto p2_items = MasterLayer::page_export_items(doc.get(), page(1));
    ASSERT_EQ(p2_items.size(), 3u);
    EXPECT_EQ(p2_items[0], item("c2"));
    EXPECT_EQ(p2_items[1], layer("master-bg"));
    EXPECT_EQ(p2_items[2], layer("master-top"));
    EXPECT_EQ(std::find(p2_items.begin(), p2_items.end(), item("c1")), p2_items.end());

    // Page 1 overlaps master content: the layers themselves are not listed
    // again (their overlapping items already are), so no duplicates appear.
    auto p1_items = MasterLayer::page_export_items(doc.get(), page(0));
    ASSERT_EQ(p1_items.size(), 4u);
    EXPECT_NE(std::find(p1_items.begin(), p1_items.end(), item("c1")), p1_items.end());
    for (auto *layer_id : {"master-bg", "master-top"}) {
        EXPECT_EQ(std::find(p1_items.begin(), p1_items.end(), layer(layer_id)), p1_items.end())
            << layer_id;
    }

    EXPECT_TRUE(MasterLayer::page_export_items(nullptr, page(0)).empty());
    EXPECT_TRUE(MasterLayer::page_export_items(doc.get(), nullptr).empty());
}

TEST_F(MasterLayerTest, pageExportCropKeepsMastersOnRemainingPage)
{
    // Headless simulation of Export::exportVector for page 2: collect the
    // per-page items, delete the other pages, crop to the collected set.
    flag_masters();
    auto &pm = doc->getPageManager();

    auto *p2 = page(1);
    auto const objects = MasterLayer::page_export_items(doc.get(), p2);
    ASSERT_EQ(objects.size(), 3u);

    auto const bg_before = bounds(layer("master-bg"));
    auto const top_before = bounds(layer("master-top"));
    auto const c2_before = bounds(rect("c2"));
    auto const p1_rect = page(0)->getDesktopRect();
    auto const p2_rect = p2->getDesktopRect();

    // Make weak pointers to pages, since deletePage() can delete more than
    // just the requested page.
    std::vector<SPWeakPtr<SPPage>> copy_pages;
    copy_pages.reserve(pm.getPageCount());
    for (auto *delete_page : pm.getPages()) {
        copy_pages.emplace_back(delete_page);
    }
    for (auto &delete_page : copy_pages) {
        if (delete_page && delete_page->getId() != p2->getId()) {
            pm.deletePage(delete_page.get(), false);
        }
    }
    doc->ensureUpToDate();
    ASSERT_EQ(pm.getPageCount(), 1);

    std::vector<SPObject *> objects_to_export;
    for (auto *obj : objects) {
        objects_to_export.push_back(doc->getObjectById(obj->getId()));
    }
    doc->getRoot()->cropToObjects(objects_to_export);
    doc->ensureUpToDate();

    // Page-1 content is cropped away, both master layers survive with all
    // their children.
    EXPECT_EQ(doc->getObjectById("c1"), nullptr);
    EXPECT_NE(doc->getObjectById("c2"), nullptr);
    EXPECT_NE(doc->getObjectById("bg"), nullptr);
    EXPECT_NE(doc->getObjectById("hdr"), nullptr);
    EXPECT_NE(doc->getObjectById("badge"), nullptr);
    EXPECT_EQ(layer("master-bg")->getItemCount(), 1);
    EXPECT_EQ(layer("master-top")->getItemCount(), 2);
    EXPECT_EQ(MasterLayer::master_layers(doc.get()).size(), 2u);

    // The re-anchored master bboxes sit at the same position relative to the
    // remaining page as they did relative to page 1 (03-02 re-anchor).
    auto *new_first = pm.getFirstPage();
    ASSERT_NE(new_first, nullptr);
    EXPECT_EQ(new_first, p2);

    double const tolerance = 0.01 * doc->getDocumentScale().vector()[0];
    auto const bg_after = bounds(layer("master-bg"));
    auto const bg_rel_before = bg_before.min() - p1_rect.min();
    auto const bg_rel_after = bg_after.min() - new_first->getDesktopRect().min();
    EXPECT_NEAR(bg_rel_after.x(), bg_rel_before.x(), tolerance);
    EXPECT_NEAR(bg_rel_after.y(), bg_rel_before.y(), tolerance);

    auto const top_after = bounds(layer("master-top"));
    auto const top_rel_before = top_before.min() - p1_rect.min();
    auto const top_rel_after = top_after.min() - new_first->getDesktopRect().min();
    EXPECT_NEAR(top_rel_after.x(), top_rel_before.x(), tolerance);
    EXPECT_NEAR(top_rel_after.y(), top_rel_before.y(), tolerance);

    // The surviving page keeps its own content at its page-relative position
    // (deletePage refits the document to the one remaining page, so absolute
    // document coordinates move with it).
    auto *c2 = rect("c2");
    ASSERT_NE(c2, nullptr);
    auto const c2_rel_before = c2_before.min() - p2_rect.min();
    auto const c2_rel_after = bounds(c2).min() - new_first->getDesktopRect().min();
    EXPECT_NEAR(c2_rel_after.x(), c2_rel_before.x(), tolerance);
    EXPECT_NEAR(c2_rel_after.y(), c2_rel_before.y(), tolerance);
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
