// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Lock for the live "artistic_media" LPE and the ArtisticOps operations:
 * creation from a skeleton, live update on skeleton edits/transforms, width
 * and mode changes, undo/redo, duplicate forking, save/reload, break apart
 * and the load-time repair/fallback rules.
 *//*
 *
 * All expected numbers are hand-derived from the model in
 * artistic-media-geometry.h / artistic-media-library.h, not copied from output.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <glibmm/i18n.h>
#include <gtest/gtest.h>

#include <2geom/affine.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

#include "actions/actions-artistic-media.h"
#include "document-undo.h"
#include "document-update.h"
#include "document.h"
#include "inkscape.h"
#include "live_effects/artistic-media-geometry.h"
#include "live_effects/artistic-media-library.h"
#include "live_effects/effect-enum.h"
#include "live_effects/lpe-artistic-media.h"
#include "object/object-set.h"
#include "object/sp-item-group.h"
#include "object/sp-lpe-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "preferences.h"
#include "style.h"
#include "svg/svg.h"
#include "util-string/context-string.h"
#include "xml/node.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace std::literals;
using Inkscape::ArtisticOps::Options;
using Inkscape::ArtisticOps::Row;
using Inkscape::ArtisticOps::Verb;
using Inkscape::LivePathEffect::LPEArtisticMedia;
namespace AM = Inkscape::LivePathEffect::ArtisticMedia;

namespace {

// 1 user unit = 1 mm.
constexpr auto DOC = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="200mm" height="100mm" viewBox="0 0 200 100">
  <!-- INVARIANT: s1 = open line (20,30)-(120,30), stroke #000000 width 1;
       s3 = closed square (40,60)-(80,60)-(80,90)-(40,90), stroke #0000ff;
       r1 = rect 150,60 30x20 -->
  <path id="s1" d="M 20,30 L 120,30" fill="none" stroke="#000000" stroke-width="1" />
  <path id="s3" d="M 40,60 L 80,60 L 80,90 L 40,90 Z" fill="none" stroke="#0000ff" stroke-width="1" />
  <rect id="r1" x="150" y="60" width="30" height="20" fill="#ff0000" />
</svg>
)A"sv;

/// A path inside a scaled group: 1 group unit = 2 mm on the page.
constexpr auto SCALED_DOC = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="200mm" height="100mm" viewBox="0 0 200 100">
  <g id="g" transform="scale(2)">
    <path id="s1" d="M 20,30 L 120,30" fill="none" stroke="#000000" stroke-width="1" />
  </g>
</svg>
)A"sv;

/// A stroke saved without shape_d (repair) or with an unknown shape (fallback).
std::string legacy_doc(char const *shape)
{
    return std::string(R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="200mm" height="100mm" viewBox="0 0 200 100">
  <defs>
    <inkscape:path-effect id="s1-artistic" effect="artistic_media" is_visible="true" lpeversion="1"
                          mode="preset" shape=")A") + shape +
           R"A(" width="10" angle="30" scale_with_object="true" />
  </defs>
  <path id="s1" d="M 20,25 L 120,25 L 120,35 L 20,35 Z"
        inkscape:original-d="M 20,30 L 120,30"
        inkscape:path-effect="#s1-artistic"
        style="fill:#000000;stroke:none;fill-rule:nonzero" />
</svg>
)A";
}

class ArtisticMediaLpeTest : public ::testing::Test
{
public:
    static void SetUpTestCase()
    {
        Inkscape::Application::create(false);
    }

    void SetUp() override
    {
        doc = SPDocument::createNewDocFromMem(DOC);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    void load(std::string const &xml)
    {
        doc = SPDocument::createNewDocFromMem(xml);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        sp_file_fix_lpe(doc.get());
        doc->ensureUpToDate();
    }

    SPItem *item(char const *id) const { return cast<SPItem>(doc->getObjectById(id)); }
    SPPath *path(char const *id) const { return cast<SPPath>(doc->getObjectById(id)); }

    LPEArtisticMedia *effect(SPPath *p) const
    {
        if (!p) {
            return nullptr;
        }
        auto *found = p->getFirstPathEffectOfType(
            static_cast<int>(LivePathEffect::EffectType::ARTISTIC_MEDIA));
        return dynamic_cast<LPEArtisticMedia *>(found);
    }

    Inkscape::XML::Node *effect_repr(SPPath *p) const
    {
        auto *lpe = effect(p);
        return lpe ? lpe->getRepr() : nullptr;
    }

    static std::string attr(Inkscape::XML::Node *repr, char const *key)
    {
        if (!repr) {
            return {};
        }
        char const *value = repr->attribute(key);
        return value ? value : std::string();
    }

    /// The rendered bounds in document user units (= mm in this fixture).
    Geom::OptRect bounds_uu(SPItem *object) const
    {
        if (!object) {
            return {};
        }
        auto box = object->documentGeometricBounds();
        if (!box) {
            return box;
        }
        Geom::Affine const doc2uu = doc->getRoot()->c2p.inverse();
        return Geom::OptRect(box->min() * doc2uu, box->max() * doc2uu);
    }

    /// Signed area of the rendered outline, in user units squared.
    static double outline_area(SPShape const *shape)
    {
        if (!shape || !shape->curve()) {
            return 0.0;
        }
        double area = 0.0;
        for (auto const &subpath : *shape->curve()) {
            Geom::Point centroid;
            double sub = 0.0;
            Geom::centroid(subpath.toPwSb(), centroid, sub);
            area -= sub; // Geom::centroid's area is the negated shoelace area
        }
        return area;
    }

    std::unique_ptr<ObjectSet> selection(std::initializer_list<char const *> ids) const
    {
        auto set = std::make_unique<ObjectSet>(doc.get());
        for (auto *id : ids) {
            if (auto *object = item(id)) {
                set->add(object);
            }
        }
        return set;
    }

    static Options options_for(std::string const &spec, Verb verb)
    {
        std::string error;
        auto parsed = ArtisticOps::parse_options(spec, verb, error);
        EXPECT_TRUE(parsed) << spec << ": " << error;
        return parsed.value_or(Options{});
    }

    /// Apply options to a selection and commit one undo step, like the action.
    int apply(ObjectSet &set, std::string const &spec)
    {
        auto options = options_for(spec, Verb::Apply);
        std::string errors;
        int const applied = ArtisticOps::apply(set, options, errors);
        EXPECT_TRUE(errors.empty()) << errors;
        doc->ensureUpToDate();
        if (applied > 0) {
            DocumentUndo::done(doc.get(), RC_("Undo", "Apply artistic media"), "");
            doc->ensureUpToDate();
        }
        return applied;
    }

    bool set(SPPath *p, std::string const &spec)
    {
        auto options = options_for(spec, Verb::Set);
        std::string error;
        bool const ok = ArtisticOps::set(p, options, error);
        EXPECT_TRUE(ok) << error;
        doc->ensureUpToDate();
        if (ok) {
            DocumentUndo::done(doc.get(), RC_("Undo", "Change artistic media"), "");
            doc->ensureUpToDate();
        }
        return ok;
    }

    /// Serialized document with all whitespace and the inkscape:version the
    /// writer stamps on the root removed: layout varies, content does not.
    std::string xml() const
    {
        std::string out = sp_repr_save_buf(doc->getReprDoc()).raw();
        std::size_t at = 0;
        while ((at = out.find("inkscape:version=\"")) != std::string::npos) {
            std::size_t const end = out.find('"', at + 18);
            if (end == std::string::npos) {
                break;
            }
            out.erase(at, end - at + 1);
        }
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [](unsigned char c) { return std::isspace(c) != 0; }),
                  out.end());
        return out;
    }

    std::vector<Row> rows() { return ArtisticOps::list(doc.get()); }

    /// Apply uniform W = 10 to s1 and return it.
    SPPath *make_uniform_stroke()
    {
        auto object_set = selection({"s1"});
        EXPECT_EQ(apply(*object_set, "mode=preset,shape=uniform,width=10"), 1);
        return path("s1");
    }

    std::unique_ptr<SPDocument> doc;
};

} // namespace

TEST_F(ArtisticMediaLpeTest, ApplyUniformToStraightSkeleton)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    ASSERT_TRUE(effect(s1));

    // Width 10 mm: 5 either side of y = 30.
    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 20.0, 0.05);
    EXPECT_NEAR(box->top(), 25.0, 0.05);
    EXPECT_NEAR(box->right(), 120.0, 0.05);
    EXPECT_NEAR(box->bottom(), 35.0, 0.05);

    auto *shape = cast<SPShape>(s1);
    ASSERT_TRUE(shape);
    EXPECT_NEAR(std::fabs(outline_area(shape)), 1000.0, 5.0); // length * width

    ASSERT_TRUE(shape->style);
    EXPECT_TRUE(shape->style->fill.isColor());
    EXPECT_EQ(shape->style->fill.getColor().toRGBA(), 0x000000ffu); // the former stroke colour
    EXPECT_TRUE(shape->style->stroke.isNone());
    EXPECT_EQ(shape->style->fill_rule.computed, SP_WIND_RULE_NONZERO);

    EXPECT_NE(std::strstr(attr(s1->getRepr(), "class").c_str(), "rosepen-artistic"), nullptr);
    EXPECT_STREQ(attr(s1->getRepr(), "inkscape:label").c_str(), "Artistic Media");
    // The embedded shape data is the library string itself.
    EXPECT_STREQ(attr(effect_repr(s1), "shape_d").c_str(), AM::find(AM::Mode::Preset, "uniform")->unit_d);

    // One effect, on the path itself (no hidden satellites).
    EXPECT_EQ(s1->countLPEOfType(static_cast<int>(LivePathEffect::EffectType::ARTISTIC_MEDIA)), 1u);
}

TEST_F(ArtisticMediaLpeTest, CalligraphicUsesTheStrokeColour)
{
    auto object_set = selection({"s3"});
    ASSERT_EQ(apply(*object_set, "mode=calligraphic,width=10,angle=45"), 1);

    auto *s3 = path("s3");
    ASSERT_TRUE(s3);
    auto *shape = cast<SPShape>(s3);
    ASSERT_TRUE(shape && shape->style);
    EXPECT_EQ(shape->style->fill.getColor().toRGBA(), 0x0000ffffu);

    auto const row = rows();
    ASSERT_EQ(row.size(), 1u);
    EXPECT_EQ(row[0].mode, "calligraphic");
    EXPECT_EQ(row[0].shape, "-");
    EXPECT_EQ(row[0].source, "-");
    EXPECT_NEAR(row[0].angle, 45.0, 1e-6);
    EXPECT_EQ(row[0].nodes, 4u);
    EXPECT_NEAR(row[0].length, 140.0, 1e-6);
}

TEST_F(ArtisticMediaLpeTest, LiveSkeletonEditThroughTheAttribute)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    // FINDING (R3): a bare repr write of inkscape:original-d updates the
    // cached curve-before-LPE but does not dirty the item, so the effect is
    // not re-evaluated until an explicit update. Every supported route (the
    // Node tool, app.artistic-set, the tool) pairs the write with one.
    s1->setAttribute("inkscape:original-d", "M 20,30 L 180,30");
    doc->ensureUpToDate();
    auto const stale = bounds_uu(s1);
    ASSERT_TRUE(stale);
    EXPECT_NEAR(stale->right(), 120.0, 0.05);

    sp_lpe_item_update_patheffect(s1, false, true, true);
    doc->ensureUpToDate();

    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 20.0, 0.05);
    EXPECT_NEAR(box->top(), 25.0, 0.05);
    EXPECT_NEAR(box->right(), 180.0, 0.05);
    EXPECT_NEAR(box->bottom(), 35.0, 0.05);
    EXPECT_NEAR(rows()[0].length, 160.0, 1e-6);
}

TEST_F(ArtisticMediaLpeTest, LiveSkeletonEditThroughSetSkeleton)
{
    // The action/tool route: ArtisticOps::set writes original-d and updates.
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    ASSERT_TRUE(set(s1, "skeleton=M 20,30 L 180,30"));
    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 20.0, 0.05);
    EXPECT_NEAR(box->right(), 180.0, 0.05);
    EXPECT_NEAR(box->top(), 25.0, 0.05);
    EXPECT_NEAR(box->bottom(), 35.0, 0.05);
    EXPECT_NEAR(rows()[0].length, 160.0, 1e-6);
}

TEST_F(ArtisticMediaLpeTest, LiveSkeletonEditThroughCurveBeforeLpe)
{
    // The Node tool path: set the curve before the effect and re-evaluate.
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    auto *shape = cast<SPShape>(s1);
    ASSERT_TRUE(shape);

    shape->setCurveBeforeLPE(sp_svg_read_pathv("M 20,30 L 180,30"));
    sp_lpe_item_update_patheffect(s1, false, true, true);
    doc->ensureUpToDate();

    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->right(), 180.0, 0.05);
    EXPECT_NEAR(box->bottom(), 35.0, 0.05);
}

TEST_F(ArtisticMediaLpeTest, TranslateKeepsTheWidthParameter)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    ASSERT_EQ(attr(effect_repr(s1), "width"), "10");

    s1->doWriteTransform(Geom::Translate(10, 5));
    doc->ensureUpToDate();

    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 30.0, 0.05);
    EXPECT_NEAR(box->top(), 30.0, 0.05);
    EXPECT_NEAR(box->right(), 130.0, 0.05);
    EXPECT_NEAR(box->bottom(), 40.0, 0.05);
    EXPECT_EQ(attr(effect_repr(s1), "width"), "10");
    // R2: an LPE path is unoptimized, so even a translation stays a transform
    // attribute; the brush follows because it is drawn in path coordinates.
    EXPECT_TRUE(s1->getAttribute("transform"));
}

TEST_F(ArtisticMediaLpeTest, ScaleWithObjectDoublesTheWidth)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    s1->doWriteTransform(Geom::Scale(2));
    doc->ensureUpToDate();

    // The skeleton scales about the origin: (40,60)-(240,60). An LPE path is
    // unoptimized (SPItem::unoptimized() is true for inkscape:path-effect), so
    // the scale stays as a transform attribute and the stroke scales visually
    // through i2doc: the parameter stays 10 and the brush is 20 mm wide on the
    // page (10 either side of y = 60).
    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 40.0, 0.05);
    EXPECT_NEAR(box->top(), 50.0, 0.05);
    EXPECT_NEAR(box->right(), 240.0, 0.05);
    EXPECT_NEAR(box->bottom(), 70.0, 0.05);
    EXPECT_EQ(attr(effect_repr(s1), "width"), "10");
    EXPECT_TRUE(s1->getAttribute("transform")); // R2: the transform is not baked
}

TEST_F(ArtisticMediaLpeTest, ScaleWithoutObjectKeepsTheWidth)
{
    auto object_set = selection({"s1"});
    ASSERT_EQ(apply(*object_set, "mode=preset,shape=uniform,width=10,scale-with-object=false"), 1);
    auto *s1 = path("s1");
    ASSERT_TRUE(s1);

    s1->doWriteTransform(Geom::Scale(2));
    doc->ensureUpToDate();

    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->top(), 55.0, 0.05);
    EXPECT_NEAR(box->bottom(), 65.0, 0.05);
    // The parameter is in path-local user units: with a x2 object scale,
    // holding the visual width needs half the parameter.
    EXPECT_EQ(attr(effect_repr(s1), "width"), "5");
    auto const row = rows();
    ASSERT_EQ(row.size(), 1u);
    EXPECT_NEAR(row[0].width, 10.0, 1e-6);

    // A later move is no scale: the parameter must stay 5, the box keep 10 mm.
    s1->doWriteTransform(Geom::Scale(2) * Geom::Translate(10, 0));
    doc->ensureUpToDate();
    EXPECT_EQ(attr(effect_repr(s1), "width"), "5");
    auto const moved = bounds_uu(s1);
    ASSERT_TRUE(moved);
    EXPECT_NEAR(moved->top(), 55.0, 0.05);
    EXPECT_NEAR(moved->bottom(), 65.0, 0.05);
}

TEST_F(ArtisticMediaLpeTest, RotationKeepsTheNibAnglePageFixed)
{
    auto object_set = selection({"s1"});
    ASSERT_EQ(apply(*object_set, "mode=calligraphic,width=10,angle=45"), 1);
    auto *s1 = path("s1");
    ASSERT_TRUE(s1);

    s1->doWriteTransform(Geom::Rotate::from_degrees(90));
    doc->ensureUpToDate();

    EXPECT_EQ(attr(effect_repr(s1), "angle"), "45");

    // On the page the skeleton is now x = -30, y 20..120. A page-fixed 45
    // degree nib d = (0.7071, -0.7071) sweeps p = (-30 + 0.7071 s,
    // t - 0.7071 s), s in [-5, 5], t in [20, 120]: (-27, 21) is s = 4.243,
    // t = 24 (inside), (-33, 21) is s = -4.243, t = 18 (outside). A nib that
    // rotated with the object, d = (0.7071, 0.7071), gives the mirror image.
    ASSERT_TRUE(s1->curve());
    Geom::PathVector const page =
        *s1->curve() * s1->i2doc_affine() * doc->getRoot()->c2p.inverse();
    EXPECT_NE(page.winding(Geom::Point(-27, 21)), 0);
    EXPECT_EQ(page.winding(Geom::Point(-33, 21)), 0);
    EXPECT_NE(page.winding(Geom::Point(-33, 119)), 0);
    EXPECT_EQ(page.winding(Geom::Point(-27, 119)), 0);
}

TEST_F(ArtisticMediaLpeTest, SetWidth)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    ASSERT_TRUE(set(s1, "width=4"));
    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->top(), 28.0, 0.05);
    EXPECT_NEAR(box->bottom(), 32.0, 0.05);
    EXPECT_NEAR(rows()[0].width, 4.0, 1e-6);
}

TEST_F(ArtisticMediaLpeTest, SetModeToCalligraphic)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    ASSERT_TRUE(set(s1, "mode=calligraphic,angle=-45"));
    // The nib angle is stored normalised to [0, 180).
    EXPECT_EQ(attr(effect_repr(s1), "angle"), "135");
    EXPECT_EQ(attr(effect_repr(s1), "mode"), "calligraphic");
    EXPECT_FALSE(effect_repr(s1)->attribute("shape_d")); // calligraphic ignores shapes

    auto const row = rows();
    ASSERT_EQ(row.size(), 1u);
    EXPECT_EQ(row[0].shape, "-");
    EXPECT_NEAR(row[0].angle, 135.0, 1e-6);
}

TEST_F(ArtisticMediaLpeTest, SetWithABadLaterOptionLeavesTheDocumentUntouched)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    std::string const before = xml();
    auto options = options_for("width=4,shape=nope", Verb::Set);
    std::string error;
    EXPECT_FALSE(ArtisticOps::set(s1, options, error));
    EXPECT_EQ(error, "artistic-set: unknown preset 'nope'");
    doc->ensureUpToDate();
    EXPECT_EQ(xml(), before);
}

TEST_F(ArtisticMediaLpeTest, WidthOnAScaledParentIsVisual)
{
    load(std::string(SCALED_DOC));
    auto object_set = selection({"s1"});
    ASSERT_EQ(apply(*object_set, "mode=preset,shape=uniform,width=10"), 1);

    auto *s1 = path("s1");
    ASSERT_TRUE(s1);
    // The path lives in a doubled coordinate system: the parameter is half
    // the visual millimetres.
    EXPECT_EQ(attr(effect_repr(s1), "width"), "5");

    auto const row = rows();
    ASSERT_EQ(row.size(), 1u);
    EXPECT_NEAR(row[0].width, 10.0, 1e-6);
}

TEST_F(ArtisticMediaLpeTest, UndoRedoApply)
{
    std::string const before = xml();
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    std::string const applied = xml();
    EXPECT_NE(applied, before);

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(xml(), before);

    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_NE(std::strstr(attr(path("s1")->getRepr(), "class").c_str(), "rosepen-artistic"), nullptr);
}

TEST_F(ArtisticMediaLpeTest, UndoRedoSet)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    std::string const applied = xml();

    ASSERT_TRUE(set(s1, "width=4"));
    EXPECT_NE(xml(), applied);

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    EXPECT_EQ(xml(), applied);
}

TEST_F(ArtisticMediaLpeTest, DuplicateForksTheEffect)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    ObjectSet object_set(doc.get());
    object_set.add(s1);
    object_set.duplicate(true);
    doc->ensureUpToDate();

    auto const listed = rows();
    ASSERT_EQ(listed.size(), 2u);
    auto *copy = path(listed[1].id.c_str());
    ASSERT_TRUE(copy);
    ASSERT_TRUE(effect(copy));
    EXPECT_NE(effect(s1)->getLPEObj(), effect(copy)->getLPEObj());

    // Editing the copy never touches the original.
    ASSERT_TRUE(set(copy, "width=2"));
    auto const original_box = bounds_uu(s1);
    ASSERT_TRUE(original_box);
    EXPECT_NEAR(original_box->top(), 25.0, 0.05);
    EXPECT_NEAR(original_box->bottom(), 35.0, 0.05);
}

TEST_F(ArtisticMediaLpeTest, SharedEffectIsForkedBeforeEditing)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);

    auto *prefs = Preferences::get();
    prefs->setBool("/options/forklpeonduplicate/value", false);

    ObjectSet object_set(doc.get());
    object_set.add(s1);
    object_set.duplicate(true);
    doc->ensureUpToDate();

    auto const listed = rows();
    ASSERT_EQ(listed.size(), 2u);
    auto *copy = path(listed[1].id.c_str());
    ASSERT_TRUE(copy);
    ASSERT_TRUE(effect(copy));
    EXPECT_EQ(effect(s1)->getLPEObj(), effect(copy)->getLPEObj()); // shared on purpose

    // A rejected edit writes nothing, not even the fork.
    std::string const before = xml();
    std::string error;
    EXPECT_FALSE(ArtisticOps::set(copy, options_for("width=2,shape=nope", Verb::Set), error));
    doc->ensureUpToDate();
    EXPECT_EQ(xml(), before);
    EXPECT_EQ(effect(s1)->getLPEObj(), effect(copy)->getLPEObj());

    ASSERT_TRUE(set(copy, "width=2"));
    EXPECT_NE(effect(s1)->getLPEObj(), effect(copy)->getLPEObj()); // forked on edit
    auto const original_box = bounds_uu(s1);
    ASSERT_TRUE(original_box);
    EXPECT_NEAR(original_box->top(), 25.0, 0.05);
    EXPECT_NEAR(original_box->bottom(), 35.0, 0.05);

    prefs->setBool("/options/forklpeonduplicate/value", true);
}

TEST_F(ArtisticMediaLpeTest, SaveReloadKeepsTheStrokeLive)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);

    auto const saved = sp_repr_save_buf(doc->getReprDoc());
    load(saved.raw());

    auto *reloaded = path("s1");
    ASSERT_TRUE(reloaded);
    auto const after = bounds_uu(reloaded);
    ASSERT_TRUE(after);
    EXPECT_NEAR(after->left(), box->left(), 0.05);
    EXPECT_NEAR(after->top(), box->top(), 0.05);
    EXPECT_NEAR(after->right(), box->right(), 0.05);
    EXPECT_NEAR(after->bottom(), box->bottom(), 0.05);

    // Still live after reload (see LiveSkeletonEditThroughTheAttribute for
    // why the write is paired with an explicit update).
    reloaded->setAttribute("inkscape:original-d", "M 20,30 L 180,30");
    sp_lpe_item_update_patheffect(reloaded, false, true, true);
    doc->ensureUpToDate();
    auto const edited = bounds_uu(reloaded);
    ASSERT_TRUE(edited);
    EXPECT_NEAR(edited->right(), 180.0, 0.05);
}

TEST_F(ArtisticMediaLpeTest, BreakApartFreezesTheOutlineAndKeepsTheSkeleton)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    std::string const outline = attr(s1->getRepr(), "d");
    std::string const skeleton = attr(s1->getRepr(), "inkscape:original-d");
    ASSERT_FALSE(outline.empty());
    ASSERT_FALSE(skeleton.empty());

    ObjectSet object_set(doc.get());
    object_set.add(s1);
    auto const broken = ArtisticOps::break_apart(object_set);
    ASSERT_EQ(broken.size(), 1u);
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), RC_("Undo", "Break artistic media apart"), "");
    doc->ensureUpToDate();

    auto *frozen = path("s1");
    ASSERT_TRUE(frozen);
    EXPECT_EQ(attr(frozen->getRepr(), "d"), outline); // byte identical
    EXPECT_FALSE(frozen->getAttribute("inkscape:path-effect"));
    EXPECT_FALSE(frozen->getAttribute("inkscape:original-d"));
    EXPECT_FALSE(frozen->getAttribute("class"));

    auto *next = frozen->getNext();
    ASSERT_TRUE(next);
    EXPECT_EQ(std::string(next->getId()), "s1-skeleton");
    auto *skeleton_path = cast<SPPath>(next);
    ASSERT_TRUE(skeleton_path);
    EXPECT_EQ(attr(skeleton_path->getRepr(), "d"), skeleton);
    ASSERT_TRUE(skeleton_path->style);
    EXPECT_TRUE(skeleton_path->style->fill.isNone());
    EXPECT_TRUE(skeleton_path->style->stroke.isNone());

    // No element references an artistic media effect any more.
    EXPECT_TRUE(ArtisticOps::list(doc.get()).empty());

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    auto *restored = path("s1");
    ASSERT_TRUE(restored);
    EXPECT_TRUE(effect(restored));
    EXPECT_EQ(attr(restored->getRepr(), "d"), outline);
    EXPECT_EQ(attr(restored->getRepr(), "inkscape:original-d"), skeleton);
    EXPECT_EQ(doc->getObjectById("s1-skeleton"), nullptr);
}

TEST_F(ArtisticMediaLpeTest, BreakApartAfterAMoveKeepsTheSkeletonOnTheStroke)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    // The LPE path keeps the move as a transform attribute (R2).
    s1->doWriteTransform(Geom::Translate(10, 5));
    doc->ensureUpToDate();

    ObjectSet object_set(doc.get());
    object_set.add(s1);
    ASSERT_EQ(ArtisticOps::break_apart(object_set).size(), 1u);
    doc->ensureUpToDate();

    // Outline [30,30,130,40] on the page, skeleton (30,35)-(130,35) under it.
    auto const outline_box = bounds_uu(path("s1"));
    ASSERT_TRUE(outline_box);
    EXPECT_NEAR(outline_box->left(), 30.0, 0.05);
    EXPECT_NEAR(outline_box->top(), 30.0, 0.05);
    auto *skeleton_path = path("s1-skeleton");
    ASSERT_TRUE(skeleton_path);
    auto const skeleton_box = bounds_uu(skeleton_path);
    ASSERT_TRUE(skeleton_box);
    EXPECT_NEAR(skeleton_box->left(), 30.0, 0.05);
    EXPECT_NEAR(skeleton_box->right(), 130.0, 0.05);
    EXPECT_NEAR(skeleton_box->top(), 35.0, 0.05);
    EXPECT_NEAR(skeleton_box->bottom(), 35.0, 0.05);
}

TEST_F(ArtisticMediaLpeTest, LoadRepairsMissingShapeData)
{
    load(legacy_doc("taper-end"));
    auto *s1 = path("s1");
    ASSERT_TRUE(s1);
    ASSERT_TRUE(effect(s1));

    // The repair happens on the first evaluation after load; the actions
    // force one with their update call (app.artistic-list does the same).
    sp_lpe_item_update_patheffect(s1, false, false, true);
    doc->ensureUpToDate();

    // Repaired from the library on evaluation, with no undo entry.
    EXPECT_STREQ(attr(effect_repr(s1), "shape_d").c_str(), "M 0,-0.5 L 1,0 L 0,0.5 Z");
    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->left(), 20.0, 0.05);
    EXPECT_NEAR(box->top(), 25.0, 0.05);
    EXPECT_NEAR(box->right(), 120.0, 0.05);
    EXPECT_NEAR(box->bottom(), 35.0, 0.05);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));

    auto const row = rows();
    ASSERT_EQ(row.size(), 1u);
    EXPECT_EQ(row[0].source, "embedded");
}

TEST_F(ArtisticMediaLpeTest, LoadFallsBackToUniformWithoutWriting)
{
    load(legacy_doc("no-such-shape"));
    auto *s1 = path("s1");
    ASSERT_TRUE(s1);
    ASSERT_TRUE(effect(s1));

    // Nothing written: a newer library may know the name later.
    EXPECT_FALSE(effect_repr(s1)->attribute("shape_d"));
    auto const box = bounds_uu(s1);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->top(), 25.0, 0.05); // uniform keeps the full 10 mm width
    EXPECT_NEAR(box->bottom(), 35.0, 0.05);

    auto const row = rows();
    ASSERT_EQ(row.size(), 1u);
    EXPECT_EQ(row[0].source, "fallback");
    EXPECT_FALSE(DocumentUndo::undo(doc.get())); // DBG probe: no repair write here
}

TEST_F(ArtisticMediaLpeTest, RemovingTheEffectRevertsToAStrokedSkeleton)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    auto *shape = cast<SPShape>(s1);
    ASSERT_TRUE(shape);

    s1->removeAllPathEffects(false);
    doc->ensureUpToDate();

    ASSERT_TRUE(shape->style);
    EXPECT_TRUE(shape->style->fill.isNone());
    EXPECT_TRUE(shape->style->stroke.isColor());
    EXPECT_EQ(shape->style->stroke.getColor().toRGBA(), 0x000000ffu);
    EXPECT_NEAR(shape->style->stroke_width.computed, 10.0, 1e-6);
}

TEST_F(ArtisticMediaLpeTest, RemovingWithKeepPathsLeavesTheStyle)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    auto *shape = cast<SPShape>(s1);
    ASSERT_TRUE(shape);
    ASSERT_TRUE(shape->style);
    ASSERT_TRUE(shape->style->fill.isColor());
    std::string const outline = attr(s1->getRepr(), "d");

    s1->removeAllPathEffects(true);
    doc->ensureUpToDate();

    EXPECT_TRUE(shape->style->fill.isColor());
    EXPECT_TRUE(shape->style->stroke.isNone());
    EXPECT_EQ(attr(s1->getRepr(), "d"), outline);
}

TEST_F(ArtisticMediaLpeTest, ListTsvIsExactAndLocaleIndependent)
{
    auto *s1 = make_uniform_stroke();
    ASSERT_TRUE(s1);
    EXPECT_EQ(ArtisticOps::list_tsv(doc.get()),
              "s1\tpreset\tuniform\t10.000000\t30.000000\t2\t100.000000\tembedded\nartistic-count 1\n");

    char const *locale = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
    if (!locale) {
        GTEST_SKIP() << "de_DE.UTF-8 is not available";
    }
    std::string const in_german = ArtisticOps::list_tsv(doc.get());
    std::setlocale(LC_NUMERIC, "C");
    EXPECT_EQ(in_german,
              "s1\tpreset\tuniform\t10.000000\t30.000000\t2\t100.000000\tembedded\nartistic-count 1\n");
}

TEST_F(ArtisticMediaLpeTest, ParseOptionErrors)
{
    auto check = [](std::string const &spec, Verb verb, std::string const &wanted) {
        std::string error;
        EXPECT_FALSE(ArtisticOps::parse_options(spec, verb, error)) << spec;
        EXPECT_EQ(error, wanted) << spec;
    };
    check("wat=1", Verb::Set, "unknown option 'wat'");
    check("width=wide", Verb::Set, "bad number 'wide'");
    check("width=0", Verb::Set, "width must be > 0 and <= 1000 mm");
    check("width=1001", Verb::Set, "width must be > 0 and <= 1000 mm");
    check("mode=preset,shape=nope", Verb::Set, "unknown preset 'nope'");
    check("mode=brush,shape=lens", Verb::Set, "unknown brush 'lens'");
    check("mode=calligraphic,shape=lens", Verb::Set, "shape is not used by calligraphic");

    // A skeleton value takes the rest of the string, commas included.
    std::string error;
    auto const parsed = ArtisticOps::parse_options("skeleton=M 20,30 L 180,30", Verb::Set, error);
    ASSERT_TRUE(parsed) << error;
    ASSERT_TRUE(parsed->skeleton);
    EXPECT_EQ(*parsed->skeleton, "M 20,30 L 180,30");

    // And the skeleton option is set-only.
    EXPECT_FALSE(ArtisticOps::parse_options("skeleton=M 0,0 L 1,1", Verb::Apply, error));
    EXPECT_EQ(error, "unknown option 'skeleton'");
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
