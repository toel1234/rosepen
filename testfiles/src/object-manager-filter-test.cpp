// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure ObjectManager page-view filter model.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/dialog/object-manager-filter.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <2geom/rect.h>

#include "document.h"
#include "inkscape.h"
#include "object/master-layer.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-page.h"
#include "object/sp-rect.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "util/cast.h"

using namespace Inkscape;
using namespace Inkscape::UI::Dialog::ObjectManager;
using namespace std::literals;

namespace {

constexpr auto kFilterDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="p1" viewBox="0 0 100 100" inkscape:label="One"/>
    <view id="p2" viewBox="110 0 100 100" inkscape:label="Two"/>
  </defs>
  <g inkscape:groupmode="layer" inkscape:label="L1" id="L1">
    <rect id="r1" x="10" y="10" width="20" height="20"/>
    <rect id="r2" x="120" y="10" width="20" height="20"/>
    <rect id="rs" x="90" y="10" width="30" height="20"/>
    <rect id="rp" x="500" y="10" width="20" height="20"/>
    <g id="g2">
      <rect id="g2c" x="130" y="40" width="10" height="10"/>
    </g>
    <g id="gfar">
      <rect id="gfarc" x="80" y="60" width="50" height="10"/>
    </g>
    <g id="gshared">
      <rect id="gs1" x="30" y="30" width="10" height="10"/>
      <rect id="gs2" x="120" y="30" width="10" height="10"/>
    </g>
    <g id="ge"/>
    <g inkscape:groupmode="layer" inkscape:label="sub" id="sub">
      <rect id="s1" x="125" y="70" width="10" height="10"/>
    </g>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="M" id="M" inkscape:master-layer="true">
    <rect id="m1" x="10" y="80" width="20" height="10"/>
  </g>
</svg>)A"sv;

constexpr auto kZeroPageDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100" version="1.1" id="svg1"
     xmlns="http://www.w3.org/2000/svg">
  <rect id="plain" x="5" y="5" width="10" height="10"/>
</svg>)A"sv;

} // namespace

class ObjectManagerFilterTest : public ::testing::Test
{
public:
    static void SetUpTestCase()
    {
        Inkscape::Application::create(false);
    }

    void SetUp() override
    {
        use(kFilterDoc);
    }

    void TearDown() override { doc.reset(); }

    void use(std::string_view source)
    {
        doc.reset();
        doc = SPDocument::createNewDocFromMem(source);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    SPItem *item(char const *id) const { return cast<SPItem>(doc->getObjectById(id)); }
    SPGroup *layer(char const *id) const { return cast<SPGroup>(doc->getObjectById(id)); }
    SPPage *page(int index) const { return doc->getPageManager().getPage(index); }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(ObjectManagerFilterTest, prefPath)
{
    EXPECT_STREQ(current_page_only_pref_path, "/dialogs/objects/current_page_only");
}

TEST_F(ObjectManagerFilterTest, disabledOrNoPageShowsEverything)
{
    auto *p1 = page(0);
    ASSERT_TRUE(p1);

    for (char const *id : {"r1", "r2", "rs", "rp", "g2", "g2c", "gfar", "gfarc",
                           "ge", "s1", "m1", "L1", "M", "sub"}) {
        auto *object = item(id);
        ASSERT_TRUE(object) << id;
        EXPECT_TRUE(page_filter_shows(object, nullptr, true)) << id;
        EXPECT_TRUE(page_filter_shows(object, p1, false)) << id;
        EXPECT_TRUE(page_filter_shows(nullptr, p1, true));
    }
}

TEST_F(ObjectManagerFilterTest, filterPage)
{
    // No document at all.
    EXPECT_EQ(filter_page(nullptr), nullptr);

    // An implicit single page (a document without <view> pages).
    use(kZeroPageDoc);
    EXPECT_EQ(filter_page(doc.get()), nullptr);
    EXPECT_FALSE(page_filter_active(doc.get(), true));
    EXPECT_FALSE(page_filter_active(doc.get(), false));

    // A two-page document: upstream auto-selects the last page on load.
    use(kFilterDoc);
    EXPECT_EQ(filter_page(doc.get()), page(1));
    EXPECT_TRUE(page_filter_active(doc.get(), true));

    // With no page selected the model falls back to page 1.
    EXPECT_TRUE(doc->getPageManager().selectPage(nullptr));
    EXPECT_EQ(filter_page(doc.get()), page(0));

    // The selected page wins.
    EXPECT_TRUE(doc->getPageManager().selectPage(page(1)));
    EXPECT_EQ(filter_page(doc.get()), page(1));
}

TEST_F(ObjectManagerFilterTest, pageOneView)
{
    auto *p1 = page(0);
    ASSERT_TRUE(p1);

    // Page-1 objects, the straddler, the pasteboard object, master content
    // and the empty group are all listed.
    for (char const *id : {"r1", "rs", "rp", "m1", "ge"}) {
        EXPECT_TRUE(page_filter_shows(item(id), p1, true)) << id;
    }
    // Layers are document-wide and always listed.
    for (char const *id : {"L1", "M", "sub"}) {
        EXPECT_TRUE(page_filter_shows(layer(id), p1, true)) << id;
    }
    // Page-2 objects are not.
    for (char const *id : {"r2", "g2", "g2c", "s1"}) {
        EXPECT_FALSE(page_filter_shows(item(id), p1, true)) << id;
    }
    // gfar/gfarc decide by the top-level group: both give the group's result.
    auto const gfar_shows = page_filter_shows(item("gfar"), p1, true);
    EXPECT_EQ(page_filter_shows(item("gfarc"), p1, true), gfar_shows);
    EXPECT_EQ(gfar_shows, p1->itemOnPage(item("gfar")));
}

TEST_F(ObjectManagerFilterTest, pageTwoView)
{
    auto *p2 = page(1);
    ASSERT_TRUE(p2);

    for (char const *id : {"r2", "rs", "rp", "m1", "g2", "g2c", "s1", "ge"}) {
        EXPECT_TRUE(page_filter_shows(item(id), p2, true)) << id;
    }
    for (char const *id : {"L1", "M", "sub"}) {
        EXPECT_TRUE(page_filter_shows(layer(id), p2, true)) << id;
    }
    EXPECT_FALSE(page_filter_shows(item("r1"), p2, true));
}

TEST_F(ObjectManagerFilterTest, masterAndPasteboardStayVisibleOnEveryPage)
{
    for (int index = 0; index < 2; ++index) {
        auto *p = page(index);
        ASSERT_TRUE(p);
        EXPECT_TRUE(page_filter_shows(item("m1"), p, true)) << "page " << index;
        EXPECT_TRUE(page_filter_shows(item("rp"), p, true)) << "page " << index;
        // The straddler lists on both pages it touches.
        EXPECT_TRUE(page_filter_shows(item("rs"), p, true)) << "page " << index;
    }
}

TEST_F(ObjectManagerFilterTest, decisionIsUniformAcrossDescendants)
{
    auto *p2 = page(1);
    ASSERT_TRUE(p2);
    // g2 is hidden on page 1, so g2c must be hidden with it there.
    auto *p1 = page(0);
    ASSERT_TRUE(p1);
    EXPECT_FALSE(page_filter_shows(item("g2"), p1, true));
    EXPECT_FALSE(page_filter_shows(item("g2c"), p1, true));
    // ... and shown with it on page 2.
    EXPECT_TRUE(page_filter_shows(item("g2"), p2, true));
    EXPECT_TRUE(page_filter_shows(item("g2c"), p2, true));
    // The same holds for the sublayer item.
    EXPECT_FALSE(page_filter_shows(item("s1"), p1, true));
    EXPECT_TRUE(page_filter_shows(item("s1"), p2, true));
}

TEST_F(ObjectManagerFilterTest, decidesAtTheTopLevelGroup)
{
    auto *p1 = page(0);
    auto *p2 = page(1);
    ASSERT_TRUE(p1);
    ASSERT_TRUE(p2);

    // gs1 lies only on page 1 and gs2 only on page 2, but they share a group
    // whose bounding box touches both pages. The decision is taken at the
    // top-level group, so each child follows the group, not its own bounds.
    EXPECT_FALSE(p1->itemOnPage(item("gs2")));
    EXPECT_FALSE(p2->itemOnPage(item("gs1")));
    EXPECT_TRUE(page_filter_shows(item("gs1"), p1, true));
    EXPECT_TRUE(page_filter_shows(item("gs2"), p1, true));
    EXPECT_TRUE(page_filter_shows(item("gs1"), p2, true));
    EXPECT_TRUE(page_filter_shows(item("gs2"), p2, true));
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
