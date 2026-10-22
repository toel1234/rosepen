// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the headless page operations used by the page manager actions.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <locale>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/i18n.h>

#include <src/actions/actions-page-manager.h>
#include <src/document-undo.h>
#include <src/document.h>
#include <src/inkscape.h>
#include <src/object/sp-page.h>
#include <src/object/sp-rect.h>
#include <src/page-manager.h>
#include <src/util/units.h>
#include <src/xml/repr.h>

#include "geom-predicates.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

constexpr auto kTwoPageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="p1" viewBox="0 0 100 100" inkscape:label="Front"/>
    <view id="p2" viewBox="110 0 100 100"/>
  </defs>
  <rect id="rect1" x="10" y="10" width="20" height="20" fill="red"/>
  <rect id="rect2" x="120" y="10" width="20" height="20" fill="green"/>
</svg>)A"sv;

constexpr auto kZeroPageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <rect id="rect1" x="10" y="10" width="20" height="20" fill="red"/>
</svg>)A"sv;

constexpr auto kOnePageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="only" viewBox="0 0 100 100" inkscape:label="Only"/>
  </defs>
  <rect id="rect1" x="10" y="10" width="20" height="20" fill="red"/>
</svg>)A"sv;

constexpr auto kThreePageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="p1" viewBox="0 0 100 100"/>
    <view id="p2" viewBox="110 0 100 100"/>
    <view id="p3" viewBox="220 0 100 100"/>
  </defs>
  <rect id="rect1" x="10" y="10" width="20" height="20" fill="red"/>
  <rect id="rect2" x="120" y="10" width="20" height="20" fill="green"/>
  <rect id="rect3" x="230" y="10" width="20" height="20" fill="blue"/>
</svg>)A"sv;

} // namespace

class PageOpsTest : public ::testing::Test
{
public:
    static void SetUpTestCase()
    {
        Inkscape::Application::create(false);
    }

    void SetUp() override
    {
        use(kTwoPageDoc);
    }

    void TearDown() override { doc.reset(); }

    void use(std::string_view source)
    {
        doc.reset();
        doc = SPDocument::createNewDocFromMem(source);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    SPRect *rect(char const *id) const
    {
        return cast<SPRect>(doc->getObjectById(id));
    }

    int page_position(SPItem *item) const
    {
        auto *page = doc->getPageManager().getPageFor(item, false);
        return page ? page->getPagePosition() : -1;
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(PageOpsTest, parseSizePresets)
{
    auto const *unit = Inkscape::Util::UnitTable::get().getUnit("px");

    auto const a4 = Inkscape::PageOps::parse_size(doc.get(), "A4");
    ASSERT_TRUE(a4);
    double const a4_w = Inkscape::Util::Quantity::convert(210.0, "mm", "px");
    double const a4_h = Inkscape::Util::Quantity::convert(297.0, "mm", "px");
    EXPECT_NEAR(a4->x(), a4_w, std::abs(a4_w) * 1e-6);
    EXPECT_NEAR(a4->y(), a4_h, std::abs(a4_h) * 1e-6);

    // Preset matching is case-insensitive.
    auto const letter = Inkscape::PageOps::parse_size(doc.get(), "us letter");
    ASSERT_TRUE(letter);
    double const letter_w = Inkscape::Util::Quantity::convert(8.5, "in", "px");
    double const letter_h = Inkscape::Util::Quantity::convert(11.0, "in", "px");
    EXPECT_NEAR(letter->x(), letter_w, std::abs(letter_w) * 1e-6);
    EXPECT_NEAR(letter->y(), letter_h, std::abs(letter_h) * 1e-6);

    // The short form of a US preset is accepted too.
    auto const short_letter = Inkscape::PageOps::parse_size(doc.get(), "Letter");
    ASSERT_TRUE(short_letter);
    EXPECT_NEAR(short_letter->x(), letter_w, std::abs(letter_w) * 1e-6);
    EXPECT_NEAR(short_letter->y(), letter_h, std::abs(letter_h) * 1e-6);

    // A preset name that contains an "x" and digits is still a preset.
    auto const banner = Inkscape::PageOps::parse_size(doc.get(), "Banner 468x60");
    ASSERT_TRUE(banner);
    EXPECT_NEAR(banner->x(), 468.0, 468.0 * 1e-6);
    EXPECT_NEAR(banner->y(), 60.0, 60.0 * 1e-6);

    EXPECT_NE(unit, nullptr);
}

TEST_F(PageOpsTest, parseSizeDimensions)
{
    auto const dims = Inkscape::PageOps::parse_size(doc.get(), "100x50mm");
    ASSERT_TRUE(dims);
    EXPECT_NEAR(dims->x(), Inkscape::Util::Quantity::convert(100.0, "mm", "px"),
                Inkscape::Util::Quantity::convert(100.0, "mm", "px") * 1e-6);
    EXPECT_NEAR(dims->y(), Inkscape::Util::Quantity::convert(50.0, "mm", "px"),
                Inkscape::Util::Quantity::convert(50.0, "mm", "px") * 1e-6);

    // A comma is the decimal separator in the WxH form.
    auto const comma = Inkscape::PageOps::parse_size(doc.get(), "100,5x50cm");
    ASSERT_TRUE(comma);
    EXPECT_NEAR(comma->x(), Inkscape::Util::Quantity::convert(100.5, "cm", "px"),
                Inkscape::Util::Quantity::convert(100.5, "cm", "px") * 1e-6);
    EXPECT_NEAR(comma->y(), Inkscape::Util::Quantity::convert(50.0, "cm", "px"),
                Inkscape::Util::Quantity::convert(50.0, "cm", "px") * 1e-6);

    auto const inches = Inkscape::PageOps::parse_size(doc.get(), "4x6in");
    ASSERT_TRUE(inches);
    EXPECT_NEAR(inches->x(), Inkscape::Util::Quantity::convert(4.0, "in", "px"),
                Inkscape::Util::Quantity::convert(4.0, "in", "px") * 1e-6);
    EXPECT_NEAR(inches->y(), Inkscape::Util::Quantity::convert(6.0, "in", "px"),
                Inkscape::Util::Quantity::convert(6.0, "in", "px") * 1e-6);

    // Comma as separator (not decimal).
    auto const separated = Inkscape::PageOps::parse_size(doc.get(), "100,50mm");
    ASSERT_TRUE(separated);
    EXPECT_NEAR(separated->x(), Inkscape::Util::Quantity::convert(100.0, "mm", "px"), 1e-3);
    EXPECT_NEAR(separated->y(), Inkscape::Util::Quantity::convert(50.0, "mm", "px"), 1e-3);

    // The "x" in the unit "px" is not the WxH separator.
    auto const px_comma = Inkscape::PageOps::parse_size(doc.get(), "300,200px");
    ASSERT_TRUE(px_comma);
    EXPECT_NEAR(px_comma->x(), 300.0, 1e-6);
    EXPECT_NEAR(px_comma->y(), 200.0, 1e-6);
    auto const px_x = Inkscape::PageOps::parse_size(doc.get(), "300x200 px");
    ASSERT_TRUE(px_x);
    EXPECT_NEAR(px_x->x(), 300.0, 1e-6);
    EXPECT_NEAR(px_x->y(), 200.0, 1e-6);

    // '"' is accepted as inches.
    auto const quote = Inkscape::PageOps::parse_size(doc.get(), "4x6\"");
    ASSERT_TRUE(quote);
    EXPECT_NEAR(quote->y(), Inkscape::Util::Quantity::convert(6.0, "in", "px"), 1e-6 * 576.0);
}

TEST_F(PageOpsTest, parseSizeRejectsInvalid)
{
    for (char const *spec : {"0x5mm", "-1x5mm", "abc", "1e999x1mm", "5mm", "axb", "10x0mm", "nanx5mm",
                              "infx5mm", "10x5furlong", "10x5x5mm"}) {
        EXPECT_FALSE(Inkscape::PageOps::parse_size(doc.get(), spec)) << spec;
    }
    EXPECT_FALSE(Inkscape::PageOps::parse_size(doc.get(), ""));
}

TEST_F(PageOpsTest, parseSizeIsLocaleIndependent)
{
    std::string locale_name;
    for (char const *candidate : {"de_DE.UTF-8", "de_DE.utf8", "de_DE"}) {
        try {
            std::locale probe(candidate);
            locale_name = candidate;
            break;
        } catch (std::exception const &) {
        }
    }
    if (locale_name.empty()) {
        GTEST_SKIP() << "de_DE locale is not available";
    }

    auto const previous = std::locale();
    std::locale::global(std::locale(locale_name));
    auto const decimal = Inkscape::PageOps::parse_size(doc.get(), "100,5x50cm");
    auto const preset = Inkscape::PageOps::parse_size(doc.get(), "A4");
    std::locale::global(previous);

    ASSERT_TRUE(decimal);
    EXPECT_NEAR(decimal->x(), Inkscape::Util::Quantity::convert(100.5, "cm", "px"), 1e-6 * 3797.0);
    EXPECT_NEAR(decimal->y(), Inkscape::Util::Quantity::convert(50.0, "cm", "px"), 1e-6 * 1889.0);
    ASSERT_TRUE(preset);
    EXPECT_NEAR(preset->x(), Inkscape::Util::Quantity::convert(210.0, "mm", "px"), 1e-6 * 793.7);
    EXPECT_NEAR(preset->y(), Inkscape::Util::Quantity::convert(297.0, "mm", "px"), 1e-6 * 1122.5);
}

TEST_F(PageOpsTest, gotoOutOfRangeKeepsSelection)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    EXPECT_FALSE(Inkscape::PageOps::goto_page(doc.get(), 0));
    EXPECT_FALSE(Inkscape::PageOps::goto_page(doc.get(), 3));
    EXPECT_FALSE(Inkscape::PageOps::goto_page(doc.get(), 99));
    EXPECT_EQ(pm.getSelectedPageIndex(), 0);

    EXPECT_TRUE(Inkscape::PageOps::goto_page(doc.get(), 2));
    EXPECT_EQ(pm.getSelectedPageIndex(), 1);
}

TEST_F(PageOpsTest, gotoImplicitPageDoesNotMaterializePages)
{
    use(kZeroPageDoc);
    auto &pm = doc->getPageManager();
    ASSERT_EQ(pm.getPageCount(), 0);

    EXPECT_TRUE(Inkscape::PageOps::goto_page(doc.get(), 1));
    EXPECT_EQ(pm.getPageCount(), 0);
    EXPECT_FALSE(Inkscape::PageOps::goto_page(doc.get(), 2));
    EXPECT_EQ(pm.getPageCount(), 0);
}

TEST_F(PageOpsTest, insertBefore)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    auto *page = Inkscape::PageOps::insert_page(doc.get(), Inkscape::PageOps::Where::Before);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(pm.getPageCount(), 3);
    EXPECT_EQ(pm.getPage(0), page);
    EXPECT_EQ(pm.getSelected(), page);
    EXPECT_EQ(page_position(rect("rect1")), 2);
    EXPECT_EQ(page_position(rect("rect2")), 3);
}

TEST_F(PageOpsTest, insertAfter)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    auto *page = Inkscape::PageOps::insert_page(doc.get(), Inkscape::PageOps::Where::After);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(pm.getPageCount(), 3);
    EXPECT_EQ(pm.getPage(1), page);
    EXPECT_EQ(page_position(rect("rect1")), 1);
    EXPECT_EQ(page_position(rect("rect2")), 3);
}

TEST_F(PageOpsTest, insertEnd)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    auto *page = Inkscape::PageOps::insert_page(doc.get(), Inkscape::PageOps::Where::End);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(pm.getPageCount(), 3);
    EXPECT_EQ(pm.getPage(2), page);
    EXPECT_EQ(page_position(rect("rect1")), 1);
    EXPECT_EQ(page_position(rect("rect2")), 2);
}

TEST_F(PageOpsTest, duplicateAfterKeepsCopyOnTheNewPage)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));
    auto *rect1 = rect("rect1");
    auto *rect2 = rect("rect2");
    ASSERT_NE(rect1, nullptr);
    ASSERT_NE(rect2, nullptr);

    auto *page = Inkscape::PageOps::duplicate_after(doc.get());
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(pm.getPageCount(), 3);
    EXPECT_EQ(pm.getPage(1), page);
    EXPECT_EQ(page_position(rect1), 1);
    EXPECT_EQ(page_position(rect2), 3);

    bool copied = false;
    for (auto *item : page->getOverlappingItems()) {
        if (item != rect1 && item != rect2 && is<SPRect>(item)) {
            copied = true;
        }
    }
    EXPECT_TRUE(copied) << "the duplicate should carry a copy of rect1";
}

TEST_F(PageOpsTest, moveToPositionBringsContents)
{
    auto &pm = doc->getPageManager();
    auto *p1 = pm.getPage(0);
    auto *p2 = pm.getPage(1);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    EXPECT_TRUE(Inkscape::PageOps::move_to(doc.get(), p2, 1));
    EXPECT_EQ(pm.getPage(0), p2);
    EXPECT_EQ(pm.getPage(1), p1);
    EXPECT_EQ(pm.getPageFor(rect("rect2"), false), p2);
    EXPECT_EQ(page_position(rect("rect2")), 1);
    EXPECT_EQ(page_position(rect("rect1")), 2);

    // Already in place: no move is reported.
    EXPECT_FALSE(Inkscape::PageOps::move_to(doc.get(), p2, 1));
}

TEST_F(PageOpsTest, removeWithContents)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    EXPECT_TRUE(Inkscape::PageOps::remove_page(doc.get(), true));
    doc->ensureUpToDate();

    EXPECT_EQ(doc->getObjectById("rect1"), nullptr);
    EXPECT_NE(doc->getObjectById("rect2"), nullptr);
}

TEST_F(PageOpsTest, removeKeepingContents)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));

    EXPECT_TRUE(Inkscape::PageOps::remove_page(doc.get(), false));
    doc->ensureUpToDate();

    EXPECT_NE(doc->getObjectById("rect1"), nullptr);
    EXPECT_NE(doc->getObjectById("rect2"), nullptr);
}

TEST_F(PageOpsTest, removeRefusesLastPage)
{
    use(kOnePageDoc);
    auto &pm = doc->getPageManager();
    ASSERT_EQ(pm.getPageCount(), 1);

    EXPECT_FALSE(Inkscape::PageOps::remove_page(doc.get(), true));
    EXPECT_EQ(pm.getPageCount(), 1);
    EXPECT_NE(doc->getObjectById("rect1"), nullptr);

    use(kZeroPageDoc);
    ASSERT_EQ(doc->getPageManager().getPageCount(), 0);
    EXPECT_FALSE(Inkscape::PageOps::remove_page(doc.get(), true));
    EXPECT_EQ(doc->getPageManager().getPageCount(), 0);
    EXPECT_NE(doc->getObjectById("rect1"), nullptr);
}

TEST_F(PageOpsTest, renameAndClearLabel)
{
    auto &pm = doc->getPageManager();
    pm.selectPage(0);
    ASSERT_TRUE(pm.selectPage(1));

    EXPECT_TRUE(Inkscape::PageOps::rename_page(doc.get(), "Back"));
    EXPECT_EQ(pm.getPage(1)->getLabel(), "Back");

    EXPECT_TRUE(Inkscape::PageOps::rename_page(doc.get(), ""));
    EXPECT_EQ(pm.getPage(1)->getLabel(), "Page 2");
}

TEST_F(PageOpsTest, renameImplicitPage)
{
    use(kZeroPageDoc);
    auto &pm = doc->getPageManager();
    ASSERT_EQ(pm.getPageCount(), 0);

    // An empty label on an implicit page cannot clear anything and must not
    // materialize pages.
    EXPECT_FALSE(Inkscape::PageOps::rename_page(doc.get(), ""));
    EXPECT_EQ(pm.getPageCount(), 0);

    EXPECT_TRUE(Inkscape::PageOps::rename_page(doc.get(), "Cover"));
    ASSERT_EQ(pm.getPageCount(), 1);
    EXPECT_EQ(pm.getPage(0)->getLabel(), "Cover");
}

TEST_F(PageOpsTest, resizeOtherPageUsesPresetAndOrientation)
{
    auto &pm = doc->getPageManager();
    auto *p1 = pm.getPage(0);
    auto *p2 = pm.getPage(1);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    auto const size = Inkscape::PageOps::parse_size(doc.get(), "A5");
    ASSERT_TRUE(size);
    EXPECT_TRUE(Inkscape::PageOps::resize_page(doc.get(), p2, *size,
                                               Inkscape::PageOps::Orientation::Landscape, "A5"));
    doc->ensureUpToDate();

    EXPECT_NEAR(p2->getRect().width(), 210.0, 0.01);
    EXPECT_NEAR(p2->getRect().height(), 148.0, 0.01);
    EXPECT_EQ(p2->getSizeLabel(), "A5");
    EXPECT_NEAR(p1->getRect().width(), 100.0, 0.01);
    EXPECT_NEAR(p1->getRect().height(), 100.0, 0.01);
}

TEST_F(PageOpsTest, resizeViewportPageResizesTheDocument)
{
    auto &pm = doc->getPageManager();
    auto *p1 = pm.getPage(0);
    auto *p2 = pm.getPage(1);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_TRUE(p1->isViewportPage());

    auto const size = Inkscape::PageOps::parse_size(doc.get(), "A4");
    ASSERT_TRUE(size);
    EXPECT_TRUE(Inkscape::PageOps::resize_page(doc.get(), p1, *size, std::nullopt, "A4"));
    doc->ensureUpToDate();

    auto const viewbox = doc->getViewBox();
    EXPECT_NEAR(viewbox.width(), 210.0, 0.01);
    EXPECT_NEAR(viewbox.height(), 297.0, 0.01);
    EXPECT_NEAR(doc->getWidth().value("mm"), 210.0, 0.01);
    EXPECT_NEAR(doc->getHeight().value("mm"), 297.0, 0.01);

    EXPECT_NEAR(p2->getRect().width(), 100.0, 0.01);
    EXPECT_NEAR(p2->getRect().height(), 100.0, 0.01);

    // The deferred viewport sync must not wipe the preset label.
    EXPECT_EQ(p1->getSizeLabel(), "A4");
}

TEST_F(PageOpsTest, moveAndRemoveKeepPresetLabels)
{
    auto &pm = doc->getPageManager();
    auto *p1 = pm.getPage(0);
    auto *p2 = pm.getPage(1);
    auto const a5 = Inkscape::PageOps::parse_size(doc.get(), "A5");
    ASSERT_TRUE(a5);
    ASSERT_TRUE(Inkscape::PageOps::resize_page(doc.get(), p2, *a5, Inkscape::PageOps::Orientation::Landscape, "A5"));
    doc->ensureUpToDate();
    ASSERT_EQ(p2->getSizeLabel(), "A5");

    // Reordering swaps rects through SPPage::setRect, which clears labels.
    ASSERT_TRUE(Inkscape::PageOps::move_to(doc.get(), p2, 1));
    doc->ensureUpToDate();
    EXPECT_EQ(p2->getSizeLabel(), "A5");
    EXPECT_EQ(p1->getSizeLabel(), "");

    // Deleting refits the document onto the remaining page.
    ASSERT_TRUE(pm.selectPage(p1));
    ASSERT_TRUE(Inkscape::PageOps::remove_page(doc.get(), true));
    doc->ensureUpToDate();
    ASSERT_EQ(pm.getPageCount(), 1);
    EXPECT_EQ(pm.getPage(0), p2);
    EXPECT_EQ(p2->getSizeLabel(), "A5");
}

TEST_F(PageOpsTest, listPagesFormat)
{
    EXPECT_EQ(Inkscape::PageOps::list_pages(doc.get()),
              "1\tp1\tFront\t100.000x100.000\t-\n"
              "2\tp2\tPage 2\t100.000x100.000\t-\n");

    use(kZeroPageDoc);
    EXPECT_EQ(Inkscape::PageOps::list_pages(doc.get()), "1\t-\tPage 1\t100.000x100.000\t-\n");
}

TEST_F(PageOpsTest, saveReloadRoundTrip)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(pm.selectPage(0));
    ASSERT_TRUE(Inkscape::PageOps::rename_page(doc.get(), "Cover"));
    ASSERT_TRUE(pm.selectPage(1));
    ASSERT_TRUE(Inkscape::PageOps::rename_page(doc.get(), "Back"));

    auto const size = Inkscape::PageOps::parse_size(doc.get(), "A5");
    ASSERT_TRUE(size);
    ASSERT_TRUE(Inkscape::PageOps::resize_page(doc.get(), pm.getPage(1), *size,
                                               Inkscape::PageOps::Orientation::Landscape, "A5"));

    GError *error = nullptr;
    gchar *temp = g_dir_make_tmp("rosepen-page-ops-XXXXXX", &error);
    if (error) {
        g_error_free(error);
    }
    ASSERT_NE(temp, nullptr);
    std::string const temp_dir = temp;
    g_free(temp);
    std::string const path = temp_dir + "/pages.svg";

    ASSERT_TRUE(sp_repr_save_rebased_file(doc->getReprDoc(), path.c_str(), nullptr,
                                          doc->getDocumentBase(), path.c_str()));
    auto reloaded = SPDocument::createNewDoc(path.c_str());
    ASSERT_TRUE(reloaded);
    reloaded->ensureUpToDate();

    auto &rpm = reloaded->getPageManager();
    ASSERT_EQ(rpm.getPageCount(), 2);
    EXPECT_EQ(rpm.getPage(0)->getLabel(), "Cover");
    EXPECT_EQ(rpm.getPage(1)->getLabel(), "Back");
    EXPECT_EQ(rpm.getPage(1)->getSizeLabel(), "A5");
    EXPECT_NEAR(rpm.getPage(0)->getRect().width(), 100.0, 0.01);
    EXPECT_NEAR(rpm.getPage(0)->getRect().height(), 100.0, 0.01);
    EXPECT_NEAR(rpm.getPage(1)->getRect().width(), 210.0, 0.01);
    EXPECT_NEAR(rpm.getPage(1)->getRect().height(), 148.0, 0.01);

    reloaded.reset();
    g_remove(path.c_str());
    g_rmdir(temp_dir.c_str());
}

// Upstream deviation lock (sp-namedview.cpp updateViewPort): loading a document
// re-syncs the viewport page from the root, and SPPage::setRect cleared its
// size label even when the rect was unchanged.
TEST_F(PageOpsTest, viewportPresetSurvivesReload)
{
    auto &pm = doc->getPageManager();
    auto *p1 = pm.getPage(0);
    ASSERT_TRUE(p1->isViewportPage());
    auto const a4 = Inkscape::PageOps::parse_size(doc.get(), "A4");
    ASSERT_TRUE(a4);
    ASSERT_TRUE(Inkscape::PageOps::resize_page(doc.get(), p1, *a4, std::nullopt, "A4"));
    doc->ensureUpToDate();
    ASSERT_EQ(p1->getSizeLabel(), "A4");

    auto const saved = sp_repr_save_buf(doc->getReprDoc());
    auto reloaded = SPDocument::createNewDocFromMem(std::span<char const>(saved.data(), saved.bytes()));
    ASSERT_TRUE(reloaded);
    reloaded->ensureUpToDate();
    auto &rpm = reloaded->getPageManager();
    ASSERT_EQ(rpm.getPageCount(), 2);
    ASSERT_TRUE(rpm.getPage(0)->isViewportPage());
    EXPECT_EQ(rpm.getPage(0)->getSizeLabel(), "A4");
    EXPECT_NEAR(rpm.getPage(0)->getRect().width(), 210.0, 0.01);
}

TEST_F(PageOpsTest, undoInsertPage)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));
    ASSERT_TRUE(pm.selectPage(0));

    ASSERT_NE(Inkscape::PageOps::insert_page(doc.get(), Inkscape::PageOps::Where::Before), nullptr);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Insert Page"), "");
    ASSERT_EQ(pm.getPageCount(), 3);

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_EQ(pm.getPageCount(), 2);
}

TEST_F(PageOpsTest, undoDuplicatePage)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));
    ASSERT_TRUE(pm.selectPage(0));

    ASSERT_NE(Inkscape::PageOps::duplicate_after(doc.get()), nullptr);
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Duplicate Page"), "");
    ASSERT_EQ(pm.getPageCount(), 3);

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_EQ(pm.getPageCount(), 2);
}

TEST_F(PageOpsTest, undoRenamePage)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));
    pm.selectPage(0);
    ASSERT_TRUE(pm.selectPage(1));
    std::string const before = pm.getPage(1)->getLabel();

    ASSERT_TRUE(Inkscape::PageOps::rename_page(doc.get(), "Back"));
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Rename Page"), "");
    ASSERT_EQ(pm.getPage(1)->getLabel(), "Back");

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_EQ(pm.getPage(1)->getLabel(), before);
}

TEST_F(PageOpsTest, undoResizePage)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));

    auto const size = Inkscape::PageOps::parse_size(doc.get(), "A5");
    ASSERT_TRUE(size);
    ASSERT_TRUE(Inkscape::PageOps::resize_page(doc.get(), pm.getPage(1), *size,
                                               Inkscape::PageOps::Orientation::Landscape, "A5"));
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Resize Page"), "");
    ASSERT_NEAR(pm.getPage(1)->getRect().width(), 210.0, 0.01);

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_NEAR(pm.getPage(1)->getRect().width(), 100.0, 0.01);
    EXPECT_NEAR(pm.getPage(1)->getRect().height(), 100.0, 0.01);
    EXPECT_EQ(pm.getPage(1)->getSizeLabel(), "");
}

TEST_F(PageOpsTest, undoRemovePage)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));
    ASSERT_TRUE(pm.selectPage(0));

    ASSERT_TRUE(Inkscape::PageOps::remove_page(doc.get(), true));
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Remove Page"), "");
    ASSERT_EQ(doc->getObjectById("rect1"), nullptr);

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(pm.getPageCount(), 2);
    EXPECT_NE(doc->getObjectById("rect1"), nullptr);
}

TEST_F(PageOpsTest, undoMovePage)
{
    auto &pm = doc->getPageManager();
    ASSERT_TRUE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));
    auto *p1 = pm.getPage(0);
    auto *p2 = pm.getPage(1);

    ASSERT_TRUE(Inkscape::PageOps::move_to(doc.get(), p2, 1));
    Inkscape::DocumentUndo::done(doc.get(), RC_("Undo", "Move Page"), "");
    ASSERT_EQ(pm.getPage(0), p2);

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_EQ(pm.getPage(0), p1);
    EXPECT_EQ(pm.getPage(1), p2);
}

TEST_F(PageOpsTest, stepPageNullDocument)
{
    EXPECT_FALSE(Inkscape::PageOps::step_page(nullptr, 1));
    EXPECT_FALSE(Inkscape::PageOps::step_page(nullptr, -1));
}

TEST_F(PageOpsTest, stepPageImplicitPageDoesNotMaterialize)
{
    use(kZeroPageDoc);
    auto &pm = doc->getPageManager();
    ASSERT_EQ(pm.getPageCount(), 0);

    EXPECT_FALSE(Inkscape::PageOps::step_page(doc.get(), 1));
    EXPECT_FALSE(Inkscape::PageOps::step_page(doc.get(), -1));
    EXPECT_EQ(pm.getPageCount(), 0);
}

TEST_F(PageOpsTest, stepPageForwardStopsAtLastPage)
{
    use(kThreePageDoc);
    auto &pm = doc->getPageManager();
    ASSERT_EQ(pm.getPageCount(), 3);
    pm.selectPage(0);
    ASSERT_EQ(pm.getSelectedPageIndex(), 0);

    EXPECT_TRUE(Inkscape::PageOps::step_page(doc.get(), 1));
    EXPECT_EQ(pm.getSelectedPageIndex(), 1);
    EXPECT_TRUE(Inkscape::PageOps::step_page(doc.get(), 1));
    EXPECT_EQ(pm.getSelectedPageIndex(), 2);

    // No wrap: the step past the last page is refused and the selection stays.
    EXPECT_FALSE(Inkscape::PageOps::step_page(doc.get(), 1));
    EXPECT_EQ(pm.getSelectedPageIndex(), 2);
}

TEST_F(PageOpsTest, stepPageBackStopsAtFirstPage)
{
    use(kThreePageDoc);
    auto &pm = doc->getPageManager();
    ASSERT_EQ(pm.getPageCount(), 3);
    pm.selectPage(0);
    ASSERT_EQ(pm.getSelectedPageIndex(), 0);

    // No wrap: the step before the first page is refused and the selection stays.
    EXPECT_FALSE(Inkscape::PageOps::step_page(doc.get(), -1));
    EXPECT_EQ(pm.getSelectedPageIndex(), 0);

    pm.selectPage(2);
    ASSERT_EQ(pm.getSelectedPageIndex(), 2);
    EXPECT_TRUE(Inkscape::PageOps::step_page(doc.get(), -1));
    EXPECT_EQ(pm.getSelectedPageIndex(), 1);
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