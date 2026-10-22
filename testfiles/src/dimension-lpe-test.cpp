// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Lock for the live "dimension" LPE and the app.dimension-* operations:
 * creation, live update on move/scale/node-edit, freeze on delete,
 * precision/unit changes, save/reload, undo/redo, duplicate and errors.
 *//*
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <2geom/rect.h>

#include "actions/actions-dimension.h"
#include "document-undo.h"
#include "document-update.h"
#include "document.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-item-group.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "xml/node.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace std::literals;
using Inkscape::DimensionOps::Ref;
using Inkscape::DimensionOps::Row;
using Inkscape::DimensionOps::Spec;

namespace {

// 1 user unit = 1 mm.
constexpr auto DOC = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     width="200mm" height="100mm" viewBox="0 0 200 100">
  <!-- INVARIANT: r1 = rect 10,20 40x60; r2 = rect 100,30 50x40;
       p1 = open polyline (20,90) (60,90) (60,50);
       p2 = right angle path (150,20) (180,20) (180,50) -->
  <rect id="r1" x="10" y="20" width="40" height="60" fill="#55ddff" />
  <rect id="r2" x="100" y="30" width="50" height="40" fill="#ffdd55" />
  <path id="p1" d="M 20,90 L 60,90 L 60,50" fill="none" stroke="#000" />
  <path id="p2" d="M 150,20 L 180,20 L 180,50" fill="none" stroke="#000" />
</svg>
)A"sv;

Ref anchored(char const *id, char const *anchor)
{
    Ref ref;
    ref.id = id;
    ref.anchor = anchor;
    return ref;
}

Ref free_point(double x, double y)
{
    Ref ref;
    ref.point = Geom::Point(x, y);
    return ref;
}

Spec make_spec(std::string kind, std::vector<Ref> refs, std::vector<std::pair<std::string, std::string>> opts = {})
{
    Spec spec;
    spec.kind = std::move(kind);
    spec.refs = std::move(refs);
    spec.options = std::move(opts);
    return spec;
}

class DimensionLpeTest : public ::testing::Test
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

    SPItem *item(char const *id) const { return cast<SPItem>(doc->getObjectById(id)); }
    SPGroup *group(char const *id) const { return cast<SPGroup>(doc->getObjectById(id)); }

    /// Create a dimension and require success.
    SPGroup *create(Spec const &spec)
    {
        auto *created = DimensionOps::create(doc.get(), spec, nullptr);
        EXPECT_TRUE(created) << "dimension-create failed";
        doc->ensureUpToDate();
        return created;
    }

    static std::string label_of(SPObject *obj)
    {
        if (!obj) {
            return {};
        }
        for (auto &child : obj->children) {
            auto *repr = child.getRepr();
            if (!repr || std::strcmp(repr->name(), "svg:text") != 0) {
                continue;
            }
            if (char const *cls = repr->attribute("class")) {
                if (!std::strstr(cls, "rosepen-dimension-label")) {
                    continue;
                }
            }
            for (auto *span = repr->firstChild(); span; span = span->next()) {
                if (std::strcmp(span->name(), "svg:tspan") != 0) {
                    continue;
                }
                for (auto *node = span->firstChild(); node; node = node->next()) {
                    if (node->type() == Inkscape::XML::NodeType::TEXT_NODE && node->content()) {
                        return node->content();
                    }
                }
            }
        }
        return {};
    }

    SPPath *line_of(SPObject *obj) const
    {
        if (!obj) {
            return nullptr;
        }
        for (auto &child : obj->children) {
            auto *path = cast<SPPath>(&child);
            if (path && path->getRepr()->attribute("class") &&
                std::strstr(path->getRepr()->attribute("class"), "rosepen-dimension-line")) {
                return path;
            }
        }
        return nullptr;
    }

    /// The document bounds of the rendered dimension geometry, in user units.
    Geom::OptRect line_bounds(SPObject *dim_group) const
    {
        auto *path = line_of(dim_group);
        EXPECT_TRUE(path != nullptr);
        if (!path) {
            return Geom::OptRect();
        }
        auto box = path->documentGeometricBounds(); // document (px) coordinates
        if (!box) {
            return box;
        }
        Geom::Affine const doc2uu = doc->getRoot()->c2p.inverse();
        return Geom::OptRect(box->min() * doc2uu, box->max() * doc2uu);
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(DimensionLpeTest, createHorizontalFromBBoxAnchors)
{
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}, {{"id", "dim1"}}));

    ASSERT_TRUE(dim);
    EXPECT_EQ(dim->getId(), std::string("dim1"));
    // bbox:r of r1 = (50,80); bbox:l of r2 = (100,50): |dx| = 50 mm.
    EXPECT_EQ(label_of(dim), "50.00 mm");

    auto rows = DimensionOps::list(doc.get());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].id, "dim1");
    EXPECT_EQ(rows[0].kind, "horizontal");
    EXPECT_NEAR(rows[0].value, 50.0, 1e-6);
    EXPECT_EQ(rows[0].label, "50.00 mm");

    // Group structure: class, label and LPE path child.
    EXPECT_STREQ(dim->getRepr()->attribute("inkscape:label"), "Dimension");
    ASSERT_TRUE(dim->getRepr()->attribute("class"));
    EXPECT_NE(std::strstr(dim->getRepr()->attribute("class"), "rosepen-dimension"), nullptr);
    ASSERT_TRUE(line_of(dim));
    ASSERT_TRUE(line_of(dim)->getRepr()->attribute("inkscape:path-effect"));
    // Dimension line at y = 80 - 5 = 45; extension lines overshoot it by 2 mm.
    auto const box = line_bounds(dim);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->min().x(), 50.0, 0.05);   // feet of the measured points
    EXPECT_NEAR(box->max().x(), 100.0, 0.05);
    EXPECT_NEAR(box->min().y(), 43.0, 0.05);   // 2 mm overshoot past the line
    EXPECT_NEAR(box->max().y(), 49.0, 0.05);   // extension gap 1 mm below r2's foot
}

TEST_F(DimensionLpeTest, liveMoveUpdatesValueAndGeometry)
{
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}));
    ASSERT_TRUE(dim);
    auto const before = line_bounds(dim);
    ASSERT_TRUE(before);

    item("r2")->doWriteTransform(Geom::Translate(20, 0));
    doc->ensureUpToDate();

    EXPECT_EQ(label_of(dim), "70.00 mm");
    auto const after = line_bounds(dim);
    ASSERT_TRUE(after);
    EXPECT_NEAR(after->max().x(), before->max().x() + 20.0, 1e-6); // geometry follows
}

TEST_F(DimensionLpeTest, liveScaleFollowsWithRelAnchor)
{
    // rel anchors are fractions of the geometric bbox and follow scaling.
    auto *dim = create(make_spec("aligned", {anchored("r2", "rel:0/0"), anchored("r2", "rel:1/1")}));
    ASSERT_TRUE(dim);
    EXPECT_EQ(label_of(dim), "64.03 mm"); // diagonal of 50x40

    item("r2")->doWriteTransform(Geom::Scale(2, 2));
    doc->ensureUpToDate();

    EXPECT_EQ(label_of(dim), "128.06 mm"); // 2x diagonal
}

TEST_F(DimensionLpeTest, nodeEditFollowsSegmentDimension)
{
    auto *dim = create(make_spec("segment", {anchored("p1", "node:0")}));
    ASSERT_TRUE(dim);
    EXPECT_EQ(label_of(dim), "40.00 mm"); // p1 nodes 0..1: (20,90)-(60,90)

    item("p1")->setAttribute("d", "M 20,90 L 80,90 L 60,50");
    doc->ensureUpToDate();

    EXPECT_EQ(label_of(dim), "60.00 mm");
}

TEST_F(DimensionLpeTest, deleteFreezesAtLastValue)
{
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}));
    ASSERT_TRUE(dim);
    item("r2")->doWriteTransform(Geom::Translate(20, 0));
    doc->ensureUpToDate();
    ASSERT_EQ(label_of(dim), "70.00 mm");
    auto const before = line_bounds(dim);
    ASSERT_TRUE(before);

    item("r2")->deleteObject();
    doc->ensureUpToDate();

    EXPECT_EQ(label_of(dim), "70.00 mm"); // frozen at the last value
    auto const after = line_bounds(dim);
    ASSERT_TRUE(after); // geometry survives
    EXPECT_NEAR(after->max().x(), before->max().x(), 1e-6);
    EXPECT_FALSE(line_of(dim)->curve()->empty());
}

TEST_F(DimensionLpeTest, setPrecisionAndUnit)
{
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}));
    ASSERT_TRUE(dim);
    item("r2")->doWriteTransform(Geom::Translate(20, 0));
    doc->ensureUpToDate();
    ASSERT_EQ(label_of(dim), "70.00 mm");

    ASSERT_TRUE(DimensionOps::set(dim, "precision", "1"));
    ASSERT_TRUE(DimensionOps::set(dim, "unit", "cm"));
    doc->ensureUpToDate();

    EXPECT_EQ(label_of(dim), "7.0 cm");

    auto rows = DimensionOps::list(doc.get());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NEAR(rows[0].value, 70.0, 1e-6); // list value stays in user units
}

TEST_F(DimensionLpeTest, alignedVerticalAngularKinds)
{
    auto *aligned = create(make_spec("aligned", {anchored("p2", "node:0"), anchored("p2", "node:2")}));
    ASSERT_TRUE(aligned);
    EXPECT_EQ(label_of(aligned), "42.43 mm"); // (150,20) -> (180,50)

    auto *vertical = create(make_spec("vertical", {anchored("r2", "bbox:t"), anchored("r2", "bbox:b")}));
    ASSERT_TRUE(vertical);
    EXPECT_EQ(label_of(vertical), "40.00 mm"); // (100,30) -> (100,70)

    auto *angular = create(make_spec(
        "angular", {anchored("p2", "node:0"), anchored("p2", "node:1"), anchored("p2", "node:2")}));
    ASSERT_TRUE(angular);
    EXPECT_EQ(label_of(angular), "90.00°"); // right angle at p2 node 1

    auto rows = DimensionOps::list(doc.get());
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].kind, "aligned");
    EXPECT_EQ(rows[1].kind, "vertical");
    EXPECT_EQ(rows[2].kind, "angular");
    EXPECT_NEAR(rows[2].value, 90.0, 1e-6);
}

TEST_F(DimensionLpeTest, setKindSwitchesMeasurement)
{
    // bbox:t of r1 = (30,20), bbox:l of r2 = (100,50): dx = 70, dy = 30.
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:t"), anchored("r2", "bbox:l")}));
    ASSERT_TRUE(dim);
    ASSERT_EQ(label_of(dim), "70.00 mm");

    ASSERT_TRUE(DimensionOps::set(dim, "kind", "vertical"));
    doc->ensureUpToDate();
    EXPECT_EQ(label_of(dim), "30.00 mm");

    EXPECT_FALSE(DimensionOps::set(dim, "kind", "angular")); // not switchable via set
}

TEST_F(DimensionLpeTest, freeDimensionMovesWithGroupLinkedStaysGlued)
{
    auto *free_dim = create(make_spec("horizontal", {free_point(10, 10), free_point(50, 10)}));
    auto *linked_dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}));
    ASSERT_TRUE(free_dim);
    ASSERT_TRUE(linked_dim);

    auto const free_before = line_bounds(free_dim);
    auto const linked_before = line_bounds(linked_dim);
    ASSERT_TRUE(free_before);
    ASSERT_TRUE(linked_before);

    free_dim->doWriteTransform(Geom::Translate(100, 0));
    doc->ensureUpToDate();
    auto const free_after = line_bounds(free_dim);
    ASSERT_TRUE(free_after);
    EXPECT_NEAR(free_after->min().x(), free_before->min().x() + 100.0, 1e-6); // travels with the group

    // A linked dimension stays glued to the measured geometry in document
    // coordinates even when its own group is translated.
    linked_dim->doWriteTransform(Geom::Translate(100, 0));
    doc->ensureUpToDate();
    auto const linked_after = line_bounds(linked_dim);
    ASSERT_TRUE(linked_after);
    EXPECT_NEAR(linked_after->min().x(), linked_before->min().x(), 1e-6);
    EXPECT_NEAR(linked_after->max().x(), linked_before->max().x(), 1e-6);
    EXPECT_NEAR(linked_after->min().y(), linked_before->min().y(), 1e-6);
    EXPECT_NEAR(linked_after->max().y(), linked_before->max().y(), 1e-6);
}

TEST_F(DimensionLpeTest, saveReloadKeepsLabelAndLiveness)
{
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}, {{"id", "dimR"}}));
    ASSERT_TRUE(dim);
    item("r2")->doWriteTransform(Geom::Translate(20, 0));
    doc->ensureUpToDate();
    ASSERT_EQ(label_of(dim), "70.00 mm");

    auto const saved = sp_repr_save_buf(doc->getReprDoc());
    doc = SPDocument::createNewDocFromMem(saved.raw());
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();

    auto *reloaded = group("dimR");
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(label_of(reloaded), "70.00 mm"); // identical label after reload

    // Opening a file runs the LPE fixup pass, which re-arms live updates.
    sp_file_fix_lpe(doc.get());
    doc->ensureUpToDate();

    // Still live: moving the measured object updates the reloaded dimension.
    // doWriteTransform applies an absolute transform, so move r2 to 30 mm.
    item("r2")->doWriteTransform(Geom::Translate(30, 0));
    doc->ensureUpToDate();
    EXPECT_EQ(label_of(reloaded), "80.00 mm");
}

TEST_F(DimensionLpeTest, undoRedoCreate)
{
    auto const before = sp_repr_save_buf(doc->getReprDoc());
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}, {{"id", "dimU"}}));
    ASSERT_TRUE(dim);

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(doc->getObjectById("dimU"), nullptr);

    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    auto *restored = group("dimU");
    ASSERT_TRUE(restored);
    doc->ensureUpToDate();
    EXPECT_EQ(label_of(restored), "50.00 mm");

    EXPECT_NE(sp_repr_save_buf(doc->getReprDoc()), before);
}

TEST_F(DimensionLpeTest, duplicateKeepsLabelsIndependent)
{
    auto *dim = create(make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")}));
    ASSERT_TRUE(dim);

    ObjectSet oset(doc.get());
    oset.add(dim);
    oset.duplicate(true);
    doc->ensureUpToDate();

    auto rows = DimensionOps::list(doc.get());
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].label, "50.00 mm");
    EXPECT_EQ(rows[1].label, "50.00 mm");
    EXPECT_NE(rows[0].id, rows[1].id);

    // Both dimensions follow the same measured object; each keeps its own
    // label node (no cross-wiring).
    item("r2")->doWriteTransform(Geom::Translate(20, 0));
    doc->ensureUpToDate();
    rows = DimensionOps::list(doc.get());
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].label, "70.00 mm");
    EXPECT_EQ(rows[1].label, "70.00 mm");
}

TEST_F(DimensionLpeTest, badInputLeavesDocumentUnchanged)
{
    auto const before = sp_repr_save_buf(doc->getReprDoc());

    EXPECT_EQ(DimensionOps::create(doc.get(),
                                   make_spec("horizontal", {anchored("nope", "bbox:c"), anchored("r2", "bbox:l")}),
                                   nullptr),
              nullptr); // unknown id
    EXPECT_EQ(DimensionOps::create(doc.get(),
                                   make_spec("horizontal", {anchored("r1", "bbox:xx"), anchored("r2", "bbox:l")}),
                                   nullptr),
              nullptr); // bad anchor
    EXPECT_EQ(DimensionOps::create(doc.get(), make_spec("horizontal", {anchored("r1", "bbox:r")}), nullptr),
              nullptr); // wrong ref count
    EXPECT_EQ(DimensionOps::create(doc.get(),
                                   make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")},
                                             {{"wat", "1"}}),
                                   nullptr),
              nullptr); // unknown key
    EXPECT_EQ(DimensionOps::create(doc.get(),
                                   make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")},
                                             {{"offset", "wide"}}),
                                   nullptr),
              nullptr); // unparsable number
    EXPECT_EQ(DimensionOps::create(doc.get(),
                                   make_spec("horizontal", {anchored("r1", "bbox:r"), anchored("r2", "bbox:l")},
                                             {{"unit", "parsec"}}),
                                   nullptr),
              nullptr); // unknown unit
    EXPECT_TRUE(DimensionOps::list(doc.get()).empty());

    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
}

TEST_F(DimensionLpeTest, scaledParentDoesNotChangeTheValue)
{
    // The dimension is hosted by the measured items' group, which is scaled
    // by 2: a (local x 5..15) and b (local x 40..60) sit at document x 10..30
    // and 80..120 mm. bbox:r of a = 30, bbox:l of b = 80 -> 50 mm on the page.
    constexpr auto SCALED = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg xmlns="http://www.w3.org/2000/svg" width="200mm" height="100mm" viewBox="0 0 200 100">
  <g id="g" transform="scale(2)">
    <rect id="a" x="5" y="10" width="10" height="10" />
    <rect id="b" x="40" y="10" width="20" height="10" />
  </g>
</svg>
)A"sv;
    doc = SPDocument::createNewDocFromMem(SCALED);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();

    auto *dim = create(make_spec("horizontal", {anchored("a", "bbox:r"), anchored("b", "bbox:l")}, {{"id", "dim"}}));
    ASSERT_TRUE(dim);
    EXPECT_EQ(dim->parent, group("g"));
    EXPECT_EQ(label_of(dim), "50.00 mm");
    auto const rows = DimensionOps::list(doc.get());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NEAR(rows[0].value, 50.0, 1e-6);

    // The drawn line spans the measured feet on the page.
    auto const box = line_bounds(dim);
    ASSERT_TRUE(box);
    EXPECT_NEAR(box->min().x(), 30.0, 0.05);
    EXPECT_NEAR(box->max().x(), 80.0, 0.05);
}

} // namespace

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
