// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the PowerClip document model: placement, absolute positions,
 * transform following, duplicate repair, refusals, nesting, master layers
 * and byte-identical undo.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>

#include <2geom/affine.h>
#include <2geom/point.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

#include <src/document-undo.h>
#include <src/document.h>
#include <src/enums.h>                          // for SP_CLONE_COMPENSATION_*
#include <src/inkscape.h>
#include <src/object/master-layer-display.h>
#include <src/object/master-layer.h>
#include <src/object/object-set.h>
#include <src/object/powerclip.h>
#include <src/object/sp-clippath.h>
#include <src/object/sp-item-group.h>
#include <src/object/sp-item.h>
#include <src/object/sp-rect.h>
#include <src/object/sp-root.h>
#include <src/object/sp-use.h>
#include <src/page-manager.h>
#include <src/preferences.h>
#include <src/util/cast.h>
#include <src/xml/repr.h>

#include "display/drawing.h"
#include "display/drawing-item.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

// 1 user unit = 1 px: the document is 220 x 100 px with a matching viewBox.
constexpr auto kDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="220" height="100" viewBox="0 0 220 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     xmlns="http://www.w3.org/2000/svg">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="layer1">
    <rect id="box" x="20" y="20" width="40" height="40" fill="#0000ff"/>
    <rect id="red" x="40" y="40" width="40" height="40" fill="#ff0000"/>
    <rect id="green" x="10" y="30" width="20" height="20" fill="#00ff00"/>
    <g id="grp"><rect id="gred" x="120" y="10" width="10" height="10" fill="#00ffff"/></g>
    <g id="cgrp"><rect id="cgred" x="140" y="50" width="10" height="10" fill="#ffff00"/></g>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="L2" id="layer2">
    <text id="txt" x="150" y="70" fill="#000000">Hi</text>
    <g id="gframe"><rect id="grect" x="150" y="20" width="20" height="20" fill="#00ffff"/></g>
  </g>
</svg>)A"sv;

// The contents sit inside a translated + rotated group in another layer, the
// container inside a scaled group. Both must keep their page position.
constexpr auto kTransformDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="220" height="100" viewBox="0 0 220 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     xmlns="http://www.w3.org/2000/svg">
  <g inkscape:groupmode="layer" inkscape:label="L1" id="layer1">
    <g id="scaleGroup" transform="scale(2,0.5)">
      <rect id="box" x="20" y="20" width="40" height="40" fill="#0000ff"/>
    </g>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="L2" id="layer2">
    <g id="moveGroup" transform="translate(30,10) rotate(20)">
      <rect id="red" x="40" y="40" width="40" height="40" fill="#ff0000"/>
      <rect id="green" x="10" y="30" width="20" height="20" fill="#00ff00"/>
    </g>
  </g>
</svg>)A"sv;

constexpr auto kMasterDoc = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="220" height="100" viewBox="0 0 220 100" version="1.1" id="svg1"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     xmlns="http://www.w3.org/2000/svg">
  <defs>
    <view id="p1" viewBox="0 0 100 100" inkscape:label="One"/>
    <view id="p2" viewBox="110 0 100 100" inkscape:label="Two"/>
  </defs>
  <g inkscape:groupmode="layer" inkscape:master-layer="true" inkscape:label="M" id="master">
    <rect id="mbox" x="5" y="2" width="30" height="14" fill="#0000ff"/>
    <rect id="mred" x="25" y="5" width="30" height="10" fill="#ff0000"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="L1" id="layer1"/>
</svg>)A"sv;

Geom::Rect transformed_bounds(Geom::Rect const &rect, Geom::Affine const &affine)
{
    Geom::Point points[4] = {
        rect.corner(0) * affine,
        rect.corner(1) * affine,
        rect.corner(2) * affine,
        rect.corner(3) * affine,
    };
    Geom::Rect result(points[0], points[0]);
    for (int i = 1; i < 4; ++i) {
        result.expandTo(points[i]);
    }
    return result;
}

} // namespace

class PowerClipTest : public ::testing::Test
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
    SPRect *rect(char const *id) { return cast<SPRect>(doc->getObjectById(id)); }

    Geom::Rect bounds(SPItem *object)
    {
        doc->ensureUpToDate();
        auto box = object->documentVisualBounds();
        EXPECT_TRUE(box.has_value()) << "item has no bounding box";
        return box ? *box : Geom::Rect();
    }

    std::string xml()
    {
        doc->ensureUpToDate();
        std::string const saved = sp_repr_save_buf(doc->getReprDoc()).raw();
        // SPDocument installs inkscape:version on the root after the initial
        // undo baseline is captured, so a document-wide undo legitimately
        // drops it. It is metadata, not document content; ignore that line.
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

    // Add items without ObjectSet's ancestor/descendant filtering, to build
    // selections that a GUI would normally collapse.
    static void add_raw(ObjectSet &set, std::vector<SPItem *> const &items)
    {
        set.add(items.begin(), items.end());
    }

    static void expect_rect(Geom::Rect const &actual, Geom::Rect const &expected, double tolerance = 1e-6)
    {
        EXPECT_NEAR(actual.left(), expected.left(), tolerance);
        EXPECT_NEAR(actual.top(), expected.top(), tolerance);
        EXPECT_NEAR(actual.right(), expected.right(), tolerance);
        EXPECT_NEAR(actual.bottom(), expected.bottom(), tolerance);
    }

    PowerClip::PlaceResult place(std::vector<char const *> ids)
    {
        ObjectSet set(doc.get());
        for (auto *id : ids) {
            set.add(item(id));
        }
        return PowerClip::place_inside(set);
    }

    // The single child of a clipPath, or nullptr.
    static SPItem *clip_child(SPClipPath *clip)
    {
        if (!clip) {
            return nullptr;
        }
        SPItem *only = nullptr;
        for (auto &child : clip->children) {
            if (!is<SPItem>(&child) || only) {
                return nullptr;
            }
            only = cast<SPItem>(&child);
        }
        return only;
    }

    static std::string clip_href(SPClipPath *clip)
    {
        auto *child = clip_child(clip);
        auto const *href = child ? child->getAttribute("xlink:href") : nullptr;
        return href ? std::string(href) : std::string();
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(PowerClipTest, placeBasic)
{
    auto *red = rect("red");
    auto *box = rect("box");
    ASSERT_NE(red, nullptr);
    ASSERT_NE(box, nullptr);
    auto const red_before = bounds(red);
    auto const box_before = bounds(box);

    ObjectSet set(doc.get());
    set.add(red);
    set.add(box);
    auto const result = PowerClip::place_inside(set);

    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    ASSERT_NE(result.frame, nullptr);
    auto *frame = result.frame;
    EXPECT_TRUE(PowerClip::is_frame(frame));

    // Structure: frame > container, contents (clipped).
    auto items = frame->item_list();
    ASSERT_EQ(items.size(), 2u);
    auto *container = PowerClip::container_of(frame);
    auto *contents = PowerClip::contents_of(frame);
    ASSERT_EQ(container, item("box"));
    ASSERT_NE(contents, nullptr);
    EXPECT_EQ(items[0], container);
    EXPECT_EQ(items[1], contents);
    EXPECT_EQ(std::string(container->getAttribute(PowerClip::attr)), "container");
    EXPECT_EQ(std::string(contents->getAttribute(PowerClip::attr)), "contents");

    // The clip is a clipPath with exactly one <use> of the container.
    auto *clip = contents->getClipObject();
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(std::string(contents->getAttribute("clip-path")), "url(#" + std::string(clip->getId()) + ")");
    ASSERT_NE(clip_child(clip), nullptr);
    EXPECT_TRUE(is<SPUse>(clip_child(clip)));
    EXPECT_EQ(clip_href(clip), "#box");

    // The content moved inside and kept its id.
    auto contents_items = PowerClip::content_items(frame);
    ASSERT_EQ(contents_items.size(), 1u);
    EXPECT_EQ(contents_items[0], item("red"));
    EXPECT_STREQ(contents_items[0]->getId(), "red");

    // The frame is selected, the document has exactly one frame.
    EXPECT_EQ(set.single(), frame);
    EXPECT_EQ(PowerClip::frames(doc.get()).size(), 1u);
    EXPECT_EQ(PowerClip::frame_of(item("red")), frame);
    EXPECT_EQ(PowerClip::edit_target(item("red")), contents);
    EXPECT_EQ(PowerClip::finish_target(contents), frame);

    // Neither object moved on the page.
    expect_rect(bounds(item("red")), red_before);
    expect_rect(bounds(item("box")), box_before);
}

TEST_F(PowerClipTest, placeKeepsAbsolutePositionAcrossTransforms)
{
    use(kTransformDoc);

    auto *red = item("red");
    auto *box = item("box");
    auto const red_before = bounds(red);
    auto const box_before = bounds(box);

    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    ASSERT_NE(result.frame, nullptr);

    expect_rect(bounds(item("red")), red_before);
    expect_rect(bounds(item("box")), box_before);
    ASSERT_NE(PowerClip::contents_of(result.frame), nullptr);
    EXPECT_TRUE(PowerClip::contents_of(result.frame)->transform.isIdentity(1e-9));
}

TEST_F(PowerClipTest, placeIsOneUndoStep)
{
    auto const before = xml();

    ObjectSet set(doc.get());
    set.add(item("red"));
    set.add(item("box"));
    auto const result = PowerClip::place_inside(set);
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    DocumentUndo::done(doc.get(), RC_("Undo", "Place inside PowerClip"), "");

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(xml(), before);

    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_EQ(PowerClip::frames(doc.get()).size(), 1u);
    EXPECT_EQ(PowerClip::content_items(PowerClip::frames(doc.get())[0]).size(), 1u);
}

TEST_F(PowerClipTest, refusalsLeaveDocumentUntouched)
{
    auto const before = xml();

    // A single item is never enough.
    {
        ObjectSet set(doc.get());
        set.add(item("box"));
        EXPECT_EQ(PowerClip::place_inside(set).refusal, PowerClip::Refusal::TooFewItems);
    }

    // A layer cannot be the frame.
    {
        ObjectSet set(doc.get());
        set.add(item("box"));
        set.add(group("layer2"));
        EXPECT_EQ(PowerClip::place_inside(set).refusal, PowerClip::Refusal::ContainerIsLayer);
    }

    // A layer cannot be placed inside.
    {
        ObjectSet set(doc.get());
        set.add(group("layer2"));
        set.add(item("box"));
        EXPECT_EQ(PowerClip::place_inside(set).refusal, PowerClip::Refusal::ContentIsLayer);
    }

    // The target is a descendant of a content. ObjectSet normally collapses
    // ancestor/descendant selections, so force the state the model guards.
    {
        ObjectSet set(doc.get());
        add_raw(set, {group("grp"), item("gred")});
        EXPECT_EQ(PowerClip::place_inside(set).refusal, PowerClip::Refusal::ContainerInsideContent);
    }

    // The other selection order: a content inside the target.
    {
        ObjectSet set(doc.get());
        add_raw(set, {item("gred"), group("grp")});
        EXPECT_EQ(PowerClip::place_inside(set).refusal, PowerClip::Refusal::ContentContainsContainer);
    }

    EXPECT_EQ(xml(), before);
    EXPECT_TRUE(PowerClip::frames(doc.get()).empty());
}

TEST_F(PowerClipTest, frameAndItsOwnContentRefuses)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto const before = xml();

    ObjectSet set(doc.get());
    add_raw(set, {item("red"), result.frame});
    EXPECT_EQ(PowerClip::place_inside(set).refusal, PowerClip::Refusal::NoContents);

    EXPECT_EQ(xml(), before);
    EXPECT_EQ(PowerClip::content_items(result.frame).size(), 1u);
}

TEST_F(PowerClipTest, addToFrameViaContainer)
{
    auto const first = place({"red", "box"});
    ASSERT_EQ(first.refusal, PowerClip::Refusal::None);
    auto *frame = first.frame;
    auto *contents = PowerClip::contents_of(frame);
    ASSERT_NE(contents, nullptr);
    auto *clip = contents->getClipObject();
    ASSERT_NE(clip, nullptr);

    ObjectSet set(doc.get());
    set.add(item("green"));
    set.add(item("box"));
    auto const second = PowerClip::place_inside(set);

    ASSERT_EQ(second.refusal, PowerClip::Refusal::None);
    EXPECT_EQ(second.frame, frame);
    auto contents_items = PowerClip::content_items(frame);
    ASSERT_EQ(contents_items.size(), 2u);
    EXPECT_EQ(contents_items[0], item("red"));
    EXPECT_EQ(contents_items[1], item("green"));

    // Still exactly one frame and one clip.
    EXPECT_EQ(PowerClip::frames(doc.get()).size(), 1u);
    EXPECT_EQ(contents->getClipObject(), clip);
    auto *clip2 = contents->getClipObject();
    ASSERT_NE(clip2, nullptr);
    EXPECT_EQ(clip_href(clip2), "#box");
}

TEST_F(PowerClipTest, addToFrameViaFrame)
{
    auto const first = place({"red", "box"});
    ASSERT_EQ(first.refusal, PowerClip::Refusal::None);
    auto *frame = first.frame;

    ObjectSet set(doc.get());
    set.add(item("green"));
    set.add(frame);
    auto const second = PowerClip::place_inside(set);

    ASSERT_EQ(second.refusal, PowerClip::Refusal::None);
    EXPECT_EQ(second.frame, frame);
    auto contents_items = PowerClip::content_items(frame);
    ASSERT_EQ(contents_items.size(), 2u);
    EXPECT_EQ(contents_items[0], item("red"));
    EXPECT_EQ(contents_items[1], item("green"));
    EXPECT_EQ(PowerClip::frames(doc.get()).size(), 1u);
}

TEST_F(PowerClipTest, transformFollowsTheFrame)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    auto *contents = PowerClip::contents_of(frame);
    ASSERT_NE(contents, nullptr);

    // Original rectangles in document coordinates.
    Geom::Rect const box_original(20, 20, 60, 60);
    Geom::Rect const red_original(40, 40, 80, 80);

    // 1. translate(30, 5)
    Geom::Affine const translate = Geom::Translate(30, 5);
    frame->doWriteTransform(translate);
    expect_rect(bounds(item("box")), transformed_bounds(box_original, translate));
    expect_rect(bounds(item("red")), transformed_bounds(red_original, translate));

    // 2. rotate 30 degrees about the centre of the translated box.
    Geom::Point const centre = transformed_bounds(box_original, translate).midpoint();
    Geom::Affine const rotate = Geom::Translate(centre) * Geom::Rotate::from_degrees(30) * Geom::Translate(-centre);
    Geom::Affine const translated_rotated = translate * rotate;
    frame->doWriteTransform(translated_rotated);
    expect_rect(bounds(item("box")), transformed_bounds(box_original, translated_rotated));
    expect_rect(bounds(item("red")), transformed_bounds(red_original, translated_rotated));

    // 3. scale(2, 0.5) on top of that.
    Geom::Affine const translated_rotated_scaled = translated_rotated * Geom::Scale(2, 0.5);
    frame->doWriteTransform(translated_rotated_scaled);
    expect_rect(bounds(item("box")), transformed_bounds(box_original, translated_rotated_scaled));
    expect_rect(bounds(item("red")), transformed_bounds(red_original, translated_rotated_scaled));

    // The contents group itself never carries a transform.
    EXPECT_TRUE(contents->transform.isIdentity(1e-9));
    EXPECT_EQ(contents->getAttribute("transform"), nullptr);
}

TEST_F(PowerClipTest, containerOnlyMoveKeepsContents)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *contents = PowerClip::contents_of(result.frame);
    ASSERT_NE(contents, nullptr);
    auto *clip = contents->getClipObject();
    ASSERT_NE(clip, nullptr);

    Geom::Rect const red_before = bounds(item("red"));
    Geom::Rect const box_before = bounds(item("box"));

    item("box")->doWriteTransform(Geom::Translate(-10, 0));

    // The container moves, the clip still uses it, the contents stay put.
    auto const box_after = bounds(item("box"));
    EXPECT_NEAR(box_after.left(), box_before.left() - 10, 1e-6);
    EXPECT_NEAR(box_after.top(), box_before.top(), 1e-6);
    expect_rect(bounds(item("red")), red_before);
    EXPECT_EQ(clip_href(clip), "#box");
    EXPECT_EQ(contents->getClipObject(), clip);
}

// Parameterised over the three clone-compensation modes. Moving the container
// alone through the Select-tool path (SPItem::move_rel, which fires clone
// compensation on the clip <use>) must leave the live clip glued to the
// container in every mode (AC-1), while a plain clone of the container keeps
// its upstream compensation behaviour (AC-2).
class ContainerMoveKeepsClipAligned
    : public PowerClipTest,
      public ::testing::WithParamInterface<int>
{
protected:
    void TearDown() override
    {
        // The preference is global; restore the upstream default for the
        // other tests in this binary.
        Inkscape::Preferences::get()->setInt("/options/clonecompensation/value",
                                             SP_CLONE_COMPENSATION_PARALLEL);
        PowerClipTest::TearDown();
    }
};

TEST_P(ContainerMoveKeepsClipAligned, ClipTracksContainerAndPlainClonesCompensate)
{
    Inkscape::Preferences::get()->setInt("/options/clonecompensation/value", GetParam());

    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *contents = PowerClip::contents_of(result.frame);
    ASSERT_NE(contents, nullptr);
    auto *clip = contents->getClipObject();
    ASSERT_NE(clip, nullptr);
    auto *clip_use = cast<SPUse>(clip_child(clip));
    ASSERT_NE(clip_use, nullptr);

    // A plain clone of the container, outside the frame: upstream clone
    // compensation must keep working for it (AC-2).
    auto *clone_repr = doc->getReprDoc()->createElement("svg:use");
    clone_repr->setAttribute("id", "plainclone");
    clone_repr->setAttribute("href", "#box");
    group("layer1")->getRepr()->appendChild(clone_repr);
    Inkscape::GC::release(clone_repr);
    doc->ensureUpToDate();
    auto *plain = cast<SPUse>(item("plainclone"));
    ASSERT_NE(plain, nullptr);
    Geom::Rect const plain_before = bounds(plain);

    // Move the container alone, exactly as the Select tool does.
    item("box")->move_rel(Geom::Translate(-10, 0));
    doc->ensureUpToDate();

    // AC-1: the live clip stays aligned with the container in every mode.
    expect_rect(bounds(clip_use), bounds(item("box")));
    EXPECT_EQ(clip_href(clip), "#box");
    EXPECT_EQ(contents->getClipObject(), clip);
    EXPECT_TRUE(clip_use->transform.isIdentity(1e-9));

    // AC-2: the plain clone still follows its original's compensation mode.
    if (GetParam() == SP_CLONE_COMPENSATION_UNMOVED) {
        expect_rect(bounds(plain), plain_before);
    } else {
        // NONE and PARALLEL both let the clone move with its original.
        expect_rect(bounds(plain), transformed_bounds(plain_before, Geom::Translate(-10, 0)));
    }
}

INSTANTIATE_TEST_SUITE_P(CompensationModes, ContainerMoveKeepsClipAligned,
                         ::testing::Values(static_cast<int>(SP_CLONE_COMPENSATION_NONE),
                                           static_cast<int>(SP_CLONE_COMPENSATION_PARALLEL),
                                           static_cast<int>(SP_CLONE_COMPENSATION_UNMOVED)));

TEST_F(PowerClipTest, normalizeRealignsCompensatedClipUse)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    auto *contents = PowerClip::contents_of(frame);
    ASSERT_NE(contents, nullptr);

    // Translating the container alone fires upstream clone compensation on
    // the clip <use> (pref-dependent). Whatever it did, normalize must leave
    // an untransformed use of the container, so the clip is the moved box.
    item("box")->doWriteTransform(Geom::Translate(-10, 0));
    PowerClip::normalize(frame);
    doc->ensureUpToDate();

    auto *clip = contents->getClipObject();
    ASSERT_NE(clip, nullptr);
    auto *use = cast<SPUse>(clip_child(clip));
    ASSERT_NE(use, nullptr);
    EXPECT_TRUE(use->transform.isIdentity(1e-9));
    EXPECT_EQ(clip_href(clip), "#box");
    EXPECT_FALSE(PowerClip::normalize(frame));

    // box 10..50 x 20..60, red 40..80 x 40..80: visible contents 40..50 x 40..60.
    expect_rect(bounds(contents), Geom::Rect(40, 40, 50, 60));
}

TEST_F(PowerClipTest, placeInsideContentsNestsANewFrame)
{
    auto const outer = place({"red", "green", "box"});
    ASSERT_EQ(outer.refusal, PowerClip::Refusal::None);
    auto *frame_a = outer.frame;
    auto *contents_a = PowerClip::contents_of(frame_a);
    ASSERT_NE(contents_a, nullptr);

    // While editing A: red into green (green last) makes a new frame B
    // inside A's contents instead of a no-op add to A.
    auto const inner = place({"red", "green"});
    ASSERT_EQ(inner.refusal, PowerClip::Refusal::None);
    auto *frame_b = inner.frame;
    ASSERT_NE(frame_b, nullptr);
    EXPECT_NE(frame_b, frame_a);
    EXPECT_EQ(frame_b->parent, contents_a);
    EXPECT_EQ(PowerClip::container_of(frame_b), item("green"));
    EXPECT_EQ(PowerClip::frame_of(item("red")), frame_b);

    // Finishing inside B returns to B, not to the outer A.
    auto *contents_b = PowerClip::contents_of(frame_b);
    ASSERT_NE(contents_b, nullptr);
    EXPECT_EQ(PowerClip::finish_target(contents_b), frame_b);
    EXPECT_EQ(PowerClip::finish_target(contents_a), frame_a);
}

TEST_F(PowerClipTest, placingAnotherFramesContainerMovesTheWholeFrame)
{
    auto const first = place({"red", "box"});
    ASSERT_EQ(first.refusal, PowerClip::Refusal::None);
    auto *frame_b = first.frame;

    // The container of B (as selected inside an entered frame) stands for B.
    ObjectSet set(doc.get());
    add_raw(set, {item("box"), item("txt")});
    auto const outer = PowerClip::place_inside(set);
    ASSERT_EQ(outer.refusal, PowerClip::Refusal::None);

    EXPECT_EQ(PowerClip::container_of(frame_b), item("box"));
    EXPECT_EQ(PowerClip::frame_of(frame_b->parent), outer.frame);
    auto contents = PowerClip::content_items(outer.frame);
    ASSERT_EQ(contents.size(), 1u);
    EXPECT_EQ(contents[0], frame_b);
}

TEST_F(PowerClipTest, extractNestedAndOuterTogether)
{
    auto const inner = place({"red", "box"});
    ASSERT_EQ(inner.refusal, PowerClip::Refusal::None);
    auto *frame_b = inner.frame;

    ObjectSet set(doc.get());
    set.add(frame_b);
    set.add(group("cgrp"));
    set.add(item("txt"));
    auto const outer = PowerClip::place_inside(set);
    ASSERT_EQ(outer.refusal, PowerClip::Refusal::None);
    ASSERT_EQ(PowerClip::frames(doc.get()).size(), 2u);

    // One item in each frame: the inner frame is extracted first, then the
    // outer one moves those items again.
    ObjectSet extract_set(doc.get());
    add_raw(extract_set, {item("red"), item("cgrp")});
    auto const extracted = PowerClip::extract(extract_set);

    EXPECT_TRUE(PowerClip::frames(doc.get()).empty());
    ASSERT_EQ(extracted.size(), 3u); // red (from B), then A's contents: box, cgrp
    EXPECT_EQ(extracted[0], item("red"));
    EXPECT_EQ(extracted[1], item("box"));
    EXPECT_EQ(extracted[2], item("cgrp"));
    EXPECT_EQ(extract_set.size(), 3u);
    for (auto *object : extracted) {
        EXPECT_EQ(object->parent, group("layer2"));
    }
}

TEST_F(PowerClipTest, normalizePushesContentsTransformDown)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    auto *contents = PowerClip::contents_of(frame);
    ASSERT_NE(contents, nullptr);

    contents->doWriteTransform(Geom::Translate(7, 3));
    doc->ensureUpToDate();
    ASSERT_FALSE(contents->transform.isIdentity(1e-9));
    Geom::Rect const red_after_transform = bounds(item("red"));

    EXPECT_TRUE(PowerClip::normalize(frame));
    EXPECT_TRUE(contents->transform.isIdentity(1e-9));
    EXPECT_EQ(contents->getAttribute("transform"), nullptr);
    expect_rect(bounds(item("red")), red_after_transform);

    // Nothing left to repair.
    EXPECT_FALSE(PowerClip::normalize(frame));
}

TEST_F(PowerClipTest, normalizeGivesDuplicatedFrameItsOwnClip)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    auto *original_contents = PowerClip::contents_of(frame);
    ASSERT_NE(original_contents, nullptr);
    auto *original_clip = original_contents->getClipObject();
    ASSERT_NE(original_clip, nullptr);
    std::string const original_clip_id = original_clip->getId();
    ASSERT_TRUE(PowerClip::normalize(frame) == false);

    ObjectSet selection(doc.get());
    selection.set(frame);
    selection.duplicate(true);
    doc->ensureUpToDate();

    auto *copy = cast<SPGroup>(selection.single());
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(copy, frame);
    ASSERT_TRUE(PowerClip::is_frame(copy));
    auto *copy_contents = PowerClip::contents_of(copy);
    ASSERT_NE(copy_contents, nullptr);
    // Before the repair the copy shares the original clip.
    EXPECT_EQ(copy_contents->getClipObject(), original_clip);

    EXPECT_TRUE(PowerClip::normalize(copy));
    doc->ensureUpToDate();

    auto *copy_clip = copy_contents->getClipObject();
    ASSERT_NE(copy_clip, nullptr);
    EXPECT_NE(copy_clip, original_clip);
    auto *copy_container = PowerClip::container_of(copy);
    ASSERT_NE(copy_container, nullptr);
    EXPECT_EQ(clip_href(copy_clip), "#" + std::string(copy_container->getId()));

    // The original is untouched and both clips exist.
    EXPECT_EQ(original_contents->getClipObject(), original_clip);
    EXPECT_EQ(clip_href(original_clip), "#box");
    EXPECT_NE(doc->getObjectById(original_clip_id.c_str()), nullptr);
    EXPECT_NE(doc->getObjectById(copy_clip->getId()), nullptr);
    EXPECT_FALSE(PowerClip::normalize(frame));
}

TEST_F(PowerClipTest, normalizeSurvivesMissingContents)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    auto *contents = PowerClip::contents_of(frame);
    ASSERT_NE(contents, nullptr);

    contents->deleteObject(false);
    EXPECT_FALSE(PowerClip::normalize(frame));
    EXPECT_TRUE(PowerClip::is_frame(frame));
}

TEST_F(PowerClipTest, extractRestoresPlainObjects)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    frame->doWriteTransform(Geom::Translate(30, 5));
    doc->ensureUpToDate();
    // Commit the setup as its own step, so undoing the extract returns to the
    // placed state rather than to the document before placement.
    DocumentUndo::done(doc.get(), RC_("Undo", "Place inside PowerClip"), "");

    auto *contents = PowerClip::contents_of(frame);
    ASSERT_NE(contents, nullptr);
    auto *clip = contents->getClipObject();
    ASSERT_NE(clip, nullptr);
    std::string const clip_id = clip->getId();

    Geom::Rect const box_before = bounds(item("box"));
    Geom::Rect const red_before = bounds(item("red"));
    auto const before = xml();

    ObjectSet set(doc.get());
    set.add(item("red"));
    auto const extracted = PowerClip::extract(set);

    ASSERT_EQ(extracted.size(), 1u);
    EXPECT_EQ(extracted[0], item("red"));
    EXPECT_EQ(set.single(), item("red"));
    EXPECT_TRUE(PowerClip::frames(doc.get()).empty());

    auto *box = item("box");
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(box->getAttribute(PowerClip::attr), nullptr);
    expect_rect(bounds(box), box_before);
    expect_rect(bounds(item("red")), red_before);

    // Both objects are back in the layer, the container below the content.
    EXPECT_EQ(box->parent, group("layer1"));
    EXPECT_EQ(item("red")->parent, group("layer1"));
    EXPECT_TRUE(sp_repr_compare_position_bool(box->getRepr(), item("red")->getRepr()));

    // The clipPath is gone and nothing references it.
    EXPECT_EQ(doc->getObjectById(clip_id.c_str()), nullptr);

    DocumentUndo::done(doc.get(), RC_("Undo", "Extract PowerClip contents"), "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(xml(), before);
}

TEST_F(PowerClipTest, extractKeepsObjectsDrawnInsideTheFrameGroup)
{
    // Double-click enters the frame group (upstream), so a user can draw
    // directly into it. Extract must bring such objects out, not delete them.
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    frame->doWriteTransform(Geom::Translate(30, 5));
    doc->ensureUpToDate();

    auto *stray_repr = doc->getReprDoc()->createElement("svg:rect");
    stray_repr->setAttribute("id", "stray");
    stray_repr->setAttribute("x", "0");
    stray_repr->setAttribute("y", "0");
    stray_repr->setAttribute("width", "4");
    stray_repr->setAttribute("height", "4");
    frame->getRepr()->appendChild(stray_repr);
    Inkscape::GC::release(stray_repr);
    // Inside the translated frame: page 30..34 x 5..9.
    expect_rect(bounds(item("stray")), Geom::Rect(30, 5, 34, 9));

    ObjectSet set(doc.get());
    set.add(item("red"));
    auto const extracted = PowerClip::extract(set);

    ASSERT_NE(item("stray"), nullptr);
    EXPECT_EQ(item("stray")->parent, group("layer1"));
    expect_rect(bounds(item("stray")), Geom::Rect(30, 5, 34, 9));
    // Frame order: box, contents (red), stray -> stays above red.
    EXPECT_TRUE(sp_repr_compare_position_bool(item("red")->getRepr(), item("stray")->getRepr()));
    EXPECT_EQ(extracted.size(), 2u);
    EXPECT_TRUE(PowerClip::frames(doc.get()).empty());
}

TEST_F(PowerClipTest, nestedFrames)
{
    auto const inner = place({"red", "box"});
    ASSERT_EQ(inner.refusal, PowerClip::Refusal::None);
    auto *frame_b = inner.frame;

    ObjectSet set(doc.get());
    set.add(frame_b);
    set.add(item("green"));
    auto const outer = PowerClip::place_inside(set);
    ASSERT_EQ(outer.refusal, PowerClip::Refusal::None);
    ASSERT_NE(outer.frame, nullptr);
    EXPECT_NE(outer.frame, frame_b);
    auto *frame_a = outer.frame;

    EXPECT_EQ(PowerClip::frame_of(item("red")), frame_b);
    auto all = PowerClip::frames(doc.get());
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0], frame_a);
    EXPECT_EQ(all[1], frame_b);

    // Extracting the inner frame leaves the outer one intact.
    ObjectSet extract_set(doc.get());
    extract_set.add(item("red"));
    auto const extracted = PowerClip::extract(extract_set);
    ASSERT_EQ(extracted.size(), 1u);
    EXPECT_EQ(extracted[0], item("red"));

    auto remaining = PowerClip::frames(doc.get());
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0], frame_a);
    EXPECT_NE(PowerClip::contents_of(frame_a), nullptr);
    EXPECT_EQ(PowerClip::frame_of(item("red")), frame_a);
}

TEST_F(PowerClipTest, editAndFinishTargets)
{
    auto const result = place({"red", "box"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    auto *contents = PowerClip::contents_of(frame);
    ASSERT_NE(contents, nullptr);

    // A group placed inside the contents, to prove descendant finishing.
    ObjectSet set(doc.get());
    set.add(group("cgrp"));
    set.add(frame);
    ASSERT_EQ(PowerClip::place_inside(set).refusal, PowerClip::Refusal::None);

    EXPECT_EQ(PowerClip::edit_target(frame), contents);
    EXPECT_EQ(PowerClip::edit_target(item("red")), contents);
    EXPECT_EQ(PowerClip::edit_target(item("green")), nullptr);
    EXPECT_EQ(PowerClip::edit_target(nullptr), nullptr);

    EXPECT_EQ(PowerClip::finish_target(contents), frame);
    EXPECT_EQ(PowerClip::finish_target(group("cgrp")), frame);
    EXPECT_EQ(PowerClip::finish_target(group("layer1")), nullptr);
    EXPECT_EQ(PowerClip::finish_target(nullptr), nullptr);
}

TEST_F(PowerClipTest, textAndGroupContainers)
{
    {
        auto const result = place({"green", "txt"});
        ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
        auto *contents = PowerClip::contents_of(result.frame);
        ASSERT_NE(contents, nullptr);
        auto *clip = contents->getClipObject();
        ASSERT_NE(clip, nullptr);
        EXPECT_EQ(clip_href(clip), "#txt");
    }

    auto *gframe = group("gframe");
    ASSERT_NE(gframe, nullptr);
    auto const result = place({"red", "gframe"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *contents = PowerClip::contents_of(result.frame);
    ASSERT_NE(contents, nullptr);
    auto *clip = contents->getClipObject();
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip_href(clip), "#gframe");
    EXPECT_EQ(PowerClip::container_of(result.frame), gframe);
}

TEST_F(PowerClipTest, masterLayerFrameIsCopiedToLaterPages)
{
    use(kMasterDoc);

    auto const result = place({"mred", "mbox"});
    ASSERT_EQ(result.refusal, PowerClip::Refusal::None);
    auto *frame = result.frame;
    ASSERT_NE(frame, nullptr);
    doc->ensureUpToDate();

    EXPECT_TRUE(MasterLayer::in_master(frame));

    Drawing drawing;
    unsigned const key = SPItem::display_key_new(1);
    auto *rootitem = doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    ASSERT_NE(rootitem, nullptr);
    drawing.setRoot(rootitem);
    drawing.update();

    {
        MasterLayerDisplay display(*doc, key);
        drawing.update();

        auto const &copies = display.copies();
        ASSERT_EQ(copies.size(), 1u); // one master layer, one page after the first
        EXPECT_EQ(copies[0].layer, group("master"));
        ASSERT_EQ(copies[0].items.size(), 1u);
        EXPECT_EQ(copies[0].items[0], frame);
    }

    doc->getRoot()->invoke_hide(key);
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
