// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the multipage builder for RVNG drawing imports: page sizes and
 * placement, one layer per page, id de-clashing, duplicated master content
 * and pages that cannot be parsed.
 *
 * The page strings are hand written in the shape the drawing format generator
 * emits, so the test needs neither the import library nor a GUI.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <src/extension/internal/rvng-pages.h>

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <2geom/rect.h>

#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/sp-defs.h"
#include "object/sp-gradient.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-metadata.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-paint-server-reference.h"
#include "object/sp-paint-server.h"
#include "object/sp-root.h"
#include "object/sp-stop.h"
#include "style.h"
#include "util/cast.h"
#include "util/units.h"
#include "xml/node.h"

using Inkscape::Extension::Internal::RvngPages::Result;
using Inkscape::Extension::Internal::RvngPages::document_from_pages;

namespace {

// 2 x 1 inches, viewBox in points: 1 user unit = 1 point.
char const *const PAGE_1 = R"SVG(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg:svg xmlns:svg="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" version="1.1" width="2in" height="1in" viewBox="0 0 144 72">
  <svg:defs>
    <svg:linearGradient id="grad1">
      <svg:stop offset="0" stop-color="#ff0000"/>
    </svg:linearGradient>
  </svg:defs>
  <svg:rect id="r1" x="0" y="0" width="144" height="72" fill="url(#grad1)"/>
  <svg:rect id="m1" x="4" y="4" width="10" height="10" fill="#00ff00"/>
</svg:svg>)SVG";

// Same page with another gradient, so the gradient ids clash and the stops differ.
char const *const PAGE_2 = R"SVG(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg:svg xmlns:svg="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" version="1.1" width="2in" height="1in" viewBox="0 0 144 72">
  <svg:defs>
    <svg:linearGradient id="grad1">
      <svg:stop offset="0" stop-color="#0000ff"/>
    </svg:linearGradient>
  </svg:defs>
  <svg:rect id="r1" x="0" y="0" width="144" height="72" fill="url(#grad1)"/>
  <svg:rect id="m1" x="4" y="4" width="10" height="10" fill="#00ff00"/>
</svg:svg>)SVG";

// No viewBox (older generator output): the fixup turns 72 x 216 into points.
char const *const PAGE_3 = R"SVG(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg:svg xmlns:svg="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" version="1.1" width="72" height="216">
  <svg:rect id="r3" x="0" y="0" width="72" height="216" fill="#000000"/>
</svg:svg>)SVG";

// 2 x 1 inches with twice the user units: page content must be scaled by 0.5.
char const *const PAGE_2_SCALED = R"SVG(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg:svg xmlns:svg="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" version="1.1" width="2in" height="1in" viewBox="0 0 288 144">
  <svg:defs>
    <svg:linearGradient id="grad1">
      <svg:stop offset="0" stop-color="#0000ff"/>
    </svg:linearGradient>
  </svg:defs>
  <svg:rect id="r1" x="0" y="0" width="288" height="144" fill="url(#grad1)"/>
  <svg:rect id="m1" x="4" y="4" width="10" height="10" fill="#00ff00"/>
</svg:svg>)SVG";

// A page without any XML element: the recovering parser gives no document.
char const *const BROKEN_PAGE = "not svg";

Result build(std::vector<std::string> const &page_sources)
{
    return document_from_pages(page_sources);
}

void expect_rect(Geom::Rect const &rect, double x, double y, double width, double height)
{
    EXPECT_NEAR(rect.left(), x, 1e-6);
    EXPECT_NEAR(rect.top(), y, 1e-6);
    EXPECT_NEAR(rect.width(), width, 1e-6);
    EXPECT_NEAR(rect.height(), height, 1e-6);
}

/// Bounds in document user units, as the generator expresses its geometry.
Geom::OptRect user_bounds(SPDocument &doc, SPItem *item)
{
    return item->geometricBounds(item->i2doc_affine() * doc.getRoot()->c2p.inverse());
}

/// Top-level page content in document order (layers), without document furniture.
std::vector<SPObject *> content_children(SPDocument &doc)
{
    std::vector<SPObject *> result;
    for (auto &child : doc.getRoot()->children) {
        if (is<SPDefs>(&child) || is<SPNamedView>(&child) || is<SPMetadata>(&child)) {
            continue;
        }
        result.push_back(&child);
    }
    return result;
}

} // namespace

class RvngPagesTest : public ::testing::Test
{
public:
    static void SetUpTestCase()
    {
        Inkscape::Application::create(false);
    }

    static SPItem *item(SPDocument &doc, char const *id)
    {
        return cast<SPItem>(doc.getObjectById(id));
    }

    static SPGroup *group(SPDocument &doc, std::size_t index)
    {
        auto content = content_children(doc);
        return index < content.size() ? cast<SPGroup>(content[index]) : nullptr;
    }

    static std::string stop_color(SPItem *rect)
    {
        if (!rect || !rect->style || !rect->style->fill.href) {
            return {};
        }
        auto *gradient = cast<SPGradient>(rect->style->fill.href->getObject());
        auto *stop = gradient ? gradient->getFirstStop() : nullptr;
        return stop ? stop->getColor().toString(false) : std::string();
    }
};

TEST_F(RvngPagesTest, SinglePageStaysStructurallyIdentical)
{
    auto result = build({PAGE_1});
    ASSERT_TRUE(result.doc);
    EXPECT_TRUE(result.failed_pages.empty());

    auto &doc = *result.doc;
    EXPECT_EQ(doc.getPageManager().getPageCount(), 0);

    auto *rect = item(doc, "r1");
    ASSERT_TRUE(rect);
    EXPECT_EQ(rect->parent, static_cast<SPObject *>(doc.getRoot()));
    auto *master = item(doc, "m1");
    ASSERT_TRUE(master);
    EXPECT_EQ(master->parent, static_cast<SPObject *>(doc.getRoot()));

    for (auto *child : content_children(doc)) {
        EXPECT_EQ(child->getAttribute("inkscape:groupmode"), nullptr);
    }

    expect_rect(doc.getViewBox(), 0, 0, 144, 72);
    EXPECT_NEAR(doc.getWidth().value("in"), 2.0, 1e-9);
    EXPECT_NEAR(doc.getHeight().value("in"), 1.0, 1e-9);
}

TEST_F(RvngPagesTest, ThreePagesGetSizesPlacementAndLayers)
{
    auto result = build({PAGE_1, PAGE_2, PAGE_3});
    ASSERT_TRUE(result.doc);
    EXPECT_TRUE(result.failed_pages.empty());

    auto &doc = *result.doc;
    auto &page_manager = doc.getPageManager();
    ASSERT_EQ(page_manager.getPageCount(), 3);

    expect_rect(page_manager.getPage(0)->getRect(), 0, 0, 144, 72);
    expect_rect(page_manager.getPage(1)->getRect(), 154, 0, 144, 72);
    expect_rect(page_manager.getPage(2)->getRect(), 308, 0, 72, 216);

    auto content = content_children(doc);
    ASSERT_EQ(content.size(), 3u);
    char const *const labels[] = {"Page 1", "Page 2", "Page 3"};
    for (std::size_t i = 0; i < content.size(); ++i) {
        auto *layer = cast<SPGroup>(content[i]);
        ASSERT_TRUE(layer);
        EXPECT_STREQ(layer->getAttribute("inkscape:groupmode"), "layer");
        EXPECT_STREQ(layer->getAttribute("inkscape:label"), labels[i]);
    }

    auto *layer1 = cast<SPGroup>(content[0]);
    auto *layer2 = cast<SPGroup>(content[1]);
    auto *layer3 = cast<SPGroup>(content[2]);
    ASSERT_TRUE(layer1 && layer2 && layer3);
    EXPECT_EQ(layer1->getAttribute("transform"), nullptr);
    EXPECT_STREQ(layer2->getAttribute("transform"), "translate(154,0)");
    EXPECT_STREQ(layer3->getAttribute("transform"), "translate(308,0)");

    auto *rect1 = item(doc, "r1");
    auto *master1 = item(doc, "m1");
    ASSERT_TRUE(rect1 && master1);
    EXPECT_EQ(rect1->parent, static_cast<SPObject *>(layer1));
    EXPECT_EQ(master1->parent, static_cast<SPObject *>(layer1));

    ASSERT_FALSE(layer2->children.empty());
    auto *rect2 = cast<SPItem>(&layer2->children.front());
    ASSERT_TRUE(rect2);
    auto box2 = user_bounds(doc, rect2);
    ASSERT_TRUE(box2);
    EXPECT_NEAR(box2->left(), 154, 1e-6);
    EXPECT_NEAR(box2->top(), 0, 1e-6);
    EXPECT_NEAR(box2->right(), 298, 1e-6);
    EXPECT_NEAR(box2->bottom(), 72, 1e-6);

    ASSERT_FALSE(layer3->children.empty());
    auto *rect3 = cast<SPItem>(&layer3->children.front());
    ASSERT_TRUE(rect3);
    auto box3 = user_bounds(doc, rect3);
    ASSERT_TRUE(box3);
    EXPECT_NEAR(box3->left(), 308, 1e-6);
    EXPECT_NEAR(box3->right(), 380, 1e-6);
    EXPECT_NEAR(box3->bottom(), 216, 1e-6);

    // Master content is inlined on every page that uses it, with its own gradient.
    EXPECT_EQ(stop_color(rect1), "#ff0000");
    EXPECT_EQ(stop_color(rect2), "#0000ff");

    std::size_t green_rects = 0;
    for (auto *layer : {layer1, layer2}) {
        for (auto &child : layer->children) {
            auto *rect = cast<SPItem>(&child);
            if (rect && rect->style && rect->style->fill.isColor() &&
                rect->style->fill.getColor().toString() == "#00ff00") {
                ++green_rects;
            }
        }
    }
    EXPECT_EQ(green_rects, 2u);
}

TEST_F(RvngPagesTest, GradientsAndIdsStayDistinct)
{
    auto result = build({PAGE_1, PAGE_2, PAGE_3});
    ASSERT_TRUE(result.doc);

    int gradients = 0;
    std::set<std::string> gradient_ids;
    for (auto &child : result.doc->getDefs()->children) {
        if (std::strcmp(child.getRepr()->name(), "svg:linearGradient") == 0) {
            ++gradients;
            gradient_ids.insert(child.getId());
        }
    }
    EXPECT_EQ(gradients, 2);
    EXPECT_EQ(gradient_ids.size(), 2u);

    std::size_t total_ids = 0;
    std::set<std::string> ids;
    auto visit = [&](auto &&self, Inkscape::XML::Node *node) -> void {
        if (auto id = node->attribute("id")) {
            ++total_ids;
            ids.insert(id);
        }
        for (auto *child = node->firstChild(); child; child = child->next()) {
            self(self, child);
        }
    };
    visit(visit, result.doc->getReprRoot());
    EXPECT_EQ(ids.size(), total_ids);
}

TEST_F(RvngPagesTest, EquivalentGradientsAreSharedNotDuplicated)
{
    // Repeated (master) content repeats its gradient verbatim; the id clash
    // resolver keeps the id of an equivalent gradient, so the copy must not
    // land in the defs a second time under the same id.
    auto result = build({PAGE_1, PAGE_1, PAGE_1});
    ASSERT_TRUE(result.doc);
    auto &doc = *result.doc;

    int gradients = 0;
    for (auto &child : doc.getDefs()->children) {
        if (std::strcmp(child.getRepr()->name(), "svg:linearGradient") == 0) {
            ++gradients;
        }
    }
    EXPECT_EQ(gradients, 1);

    std::size_t total_ids = 0;
    std::set<std::string> ids;
    auto visit = [&](auto &&self, Inkscape::XML::Node *node) -> void {
        if (auto id = node->attribute("id")) {
            ++total_ids;
            ids.insert(id);
        }
        for (auto *child = node->firstChild(); child; child = child->next()) {
            self(self, child);
        }
    };
    visit(visit, doc.getReprRoot());
    EXPECT_EQ(ids.size(), total_ids);

    for (std::size_t index = 0; index < 3; ++index) {
        auto *layer = group(doc, index);
        ASSERT_TRUE(layer);
        auto *rect = cast<SPItem>(layer->firstChild());
        ASSERT_TRUE(rect);
        EXPECT_EQ(stop_color(rect), "#ff0000") << "page " << index + 1;
    }
}

TEST_F(RvngPagesTest, PageWithoutViewBoxIsFixedUpAsFirstPage)
{
    auto result = build({PAGE_3});
    ASSERT_TRUE(result.doc);
    EXPECT_TRUE(result.failed_pages.empty());

    auto &doc = *result.doc;
    EXPECT_EQ(doc.getPageManager().getPageCount(), 0);
    expect_rect(doc.getViewBox(), 0, 0, 72, 216);
    EXPECT_NEAR(doc.getWidth().value("pt"), 72.0, 1e-9);
    EXPECT_NEAR(doc.getHeight().value("pt"), 216.0, 1e-9);
}

TEST_F(RvngPagesTest, PageOfAnotherDocumentScaleIsScaled)
{
    auto result = build({PAGE_1, PAGE_2_SCALED});
    ASSERT_TRUE(result.doc);
    EXPECT_TRUE(result.failed_pages.empty());

    auto &doc = *result.doc;
    auto &page_manager = doc.getPageManager();
    ASSERT_EQ(page_manager.getPageCount(), 2);
    expect_rect(page_manager.getPage(1)->getRect(), 154, 0, 144, 72);

    auto *layer2 = group(doc, 1);
    ASSERT_TRUE(layer2);
    EXPECT_STREQ(layer2->getAttribute("transform"), "matrix(0.5,0,0,0.5,154,0)");

    ASSERT_FALSE(layer2->children.empty());
    auto *rect2 = cast<SPItem>(&layer2->children.front());
    ASSERT_TRUE(rect2);
    auto box = user_bounds(doc, rect2);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 154, 1e-6);
    EXPECT_NEAR(box->right(), 298, 1e-6);
    EXPECT_NEAR(box->bottom(), 72, 1e-6);
}

TEST_F(RvngPagesTest, UnreadablePageLeavesAnEmptyPage)
{
    auto result = build({PAGE_1, BROKEN_PAGE, PAGE_3});
    ASSERT_TRUE(result.doc);
    ASSERT_EQ(result.failed_pages.size(), 1u);
    EXPECT_EQ(result.failed_pages.front(), 2);

    auto &doc = *result.doc;
    auto &page_manager = doc.getPageManager();
    ASSERT_EQ(page_manager.getPageCount(), 3);
    expect_rect(page_manager.getPage(1)->getRect(), 154, 0, 144, 72);
    expect_rect(page_manager.getPage(2)->getRect(), 308, 0, 72, 216);

    auto *layer2 = group(doc, 1);
    ASSERT_TRUE(layer2);
    EXPECT_STREQ(layer2->getAttribute("inkscape:label"), "Page 2");
    EXPECT_TRUE(layer2->children.empty());
}

TEST_F(RvngPagesTest, UnreadableFirstPageFailsTheImport)
{
    auto result = build({BROKEN_PAGE, PAGE_1});
    EXPECT_FALSE(result.doc);
    ASSERT_EQ(result.failed_pages.size(), 1u);
    EXPECT_EQ(result.failed_pages.front(), 1);
}

TEST_F(RvngPagesTest, NoPagesGivesNoDocument)
{
    auto result = build({});
    EXPECT_FALSE(result.doc);
    EXPECT_TRUE(result.failed_pages.empty());
}

TEST_F(RvngPagesTest, MultipageBuildLeavesNoUndoHistory)
{
    auto result = build({PAGE_1, PAGE_2, PAGE_3});
    ASSERT_TRUE(result.doc);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(result.doc.get()));
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
