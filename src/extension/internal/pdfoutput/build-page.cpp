// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Provide a capypdf interface that understands 2geom, styles, etc.
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "build-page.h"

#include "colors/color.h"
#include "colors/spaces/enum.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-object.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "style.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

namespace {

/**
 * capypdf::Color has no public separation setter, but the C API does.
 * Adopt a C-level handle into the wrapper for the registration mark colour.
 */
class SeparationColor final : public capypdf::Color
{
public:
    SeparationColor(CapyPDF_SeparationId id, double tint)
    {
        CapyPDF_Color *color = nullptr;
        if (auto const rc = capy_color_new(&color)) {
            throw capypdf::PdfException(capy_error_message(rc));
        }
        if (auto const rc = capy_color_set_separation(color, id, tint)) {
            capy_color_destroy(color);
            throw capypdf::PdfException(capy_error_message(rc));
        }
        _d.reset(color);
    }
};

} // namespace

/**
 * Get the transformation for the given page.
 * 96 to 72 dpi plus flip y axis (for PDF) plus this page's translation in the SVG document.
 */
Geom::Affine PageContext::page_transform(SPPage const *page)
{
    // The position of the page in the svg document
    return page_transform(page->getDocumentBleed());
}

Geom::Affine PageContext::page_transform(Geom::Rect const &media_box)
{
    return Geom::Translate(-media_box.left(), -media_box.top())
           // Flip the Y-Axis because PDF is bottom-left
           * Geom::Affine(1.0, 0.0, 0.0, -1.0, 0.0, media_box.height())
           // Resize from SVG's 96dpi to PDF's 72dpi
           * px2pt;
}

PageContext::PageContext(Document &doc, SPPage const *page)
    : PageContext(doc, page, page->getDocumentBleed())
{
}

PageContext::PageContext(Document &doc, SPPage const *page, Geom::Rect const &media_box)
    : DrawContext(doc, doc.generator().new_page_context(), false)
    , page_tr(page_transform(media_box))
{
    set_pagebox(CAPY_BOX_MEDIA, media_box);
}

void PageContext::set_pagebox(CapyPDF_Page_Box type, Geom::Rect const &size)
{
    // Page boxes are not affected by the cm transformations so must be transformed first
    auto box = size * page_tr;

    if (type == CAPY_BOX_MEDIA && !Geom::are_near(box.corner(0), {0.0, 0.0})) {
        // The specification technically allows non-zero media boxes, but lots of PDF
        // readers get very grumpy if you do this. Including Inkscape's own importer.
        std::cerr << "The media box must start at 0,0, found " << box.left() << ", " << box.top() << "\n";
    }

    page_props.set_pagebox(type, box.left(), box.top(), box.right(), box.bottom());
};

/**
 * Clip the page content to the content box (the BleedBox on PDF/X pages).
 * The rectangle is in document px and is mapped into page points here.
 */
void PageContext::begin_content_clip(Geom::Rect const &content)
{
    auto const box = content * page_tr;
    _ctx.cmd_q();
    _ctx.cmd_re(box.left(), box.top(), box.width(), box.height());
    _ctx.cmd_W();
    _ctx.cmd_n();
}

void PageContext::end_content_clip()
{
    _ctx.cmd_Q();
}

/**
 * Draw the crop marks in page points, outside the content clip and therefore
 * with literal coordinates. The registration colour is a Separation "All"
 * with a DeviceCMYK (1,1,1,1) alternate.
 */
void PageContext::draw_crop_marks(std::vector<std::pair<Geom::Point, Geom::Point>> const &segments)
{
    _ctx.cmd_q();

    auto const registration = Colors::Color(Colors::Space::Type::CMYK, {1.0, 1.0, 1.0, 1.0});
    if (auto const separation = _doc.get_separation("All", registration)) {
        SeparationColor color(*separation, 1.0);
        _ctx.set_stroke(color);
    } else {
        capypdf::Color color;
        color.set_cmyk(1.0, 1.0, 1.0, 1.0);
        _ctx.set_stroke(color);
    }

    _ctx.cmd_w(0.25);
    _ctx.cmd_J(CAPY_LC_BUTT);
    for (auto const &[from, to] : segments) {
        _ctx.cmd_m(from.x(), from.y());
        _ctx.cmd_l(to.x(), to.y());
        _ctx.cmd_S();
    }

    _ctx.cmd_Q();
}

/**
 * Paint the entire canvas as a transparency group.
 */
void PageContext::paint_drawing(CapyPDF_TransparencyGroupId drawing_id, Geom::Affine const &affine)
{
    paint_group(drawing_id, nullptr, affine * page_tr);
}

/**
 * Paint the root's direct children one at a time.
 *
 * This mirrors DrawContext::paint_item_group, but at page level so each item
 * can be given an extra affine (used to shift master layers to their page).
 * The per-item transparency groups are cached per item, so repeating this on
 * later pages reuses them instead of duplicating content.
 *
 * @arg extra - Extra affine per child, applied in root user units before the
 *              root viewport transform and the page transform.
 */
void PageContext::paint_root_children(SPRoot const *root,
                                      std::function<Geom::Affine(SPItem const *)> const &extra)
{
    if (!root) {
        return;
    }

    auto const page_affine = root->c2p * page_tr;

    for (auto &obj : root->children) {
        auto const child_item = cast<SPItem>(&obj);
        if (!child_item) {
            continue;
        }

        // Calculate a soft mask
        std::optional<CapyPDF_TransparencyGroupId> mask_id;
        auto const mask = child_item->getMaskObject();
        // The raster path handles masking itself.
        if (mask && !should_rasterize(*child_item)) {
            mask_id = _doc.mask_to_transparency_group(mask, child_item->transform);
        }

        // Find out if this object is a source for a clone
        std::vector<SPObject *> links;
        child_item->getLinkedRecursive(links, SPObject::LinkedObjectNature::DEPENDENT);

        auto const affine = extra(child_item) * page_affine;
        if (!is<SPGroup>(child_item) && !mask_id && links.empty() && !style_needs_group(child_item->style)) {
            // Paint the child-shape directly; its own transform is not baked anywhere.
            paint_item(child_item, child_item->transform * affine, nullptr);
        } else if (auto item_id = _doc.item_to_transparency_group(child_item)) {
            // The group already carries child->transform.
            paint_group(*item_id, child_item->style, affine, mask_id);
        }
    }
}

/**
 * Finalise the page and add any page properties.
 */
void PageContext::finalize()
{
    _ctx.set_custom_page_properties(page_props);
}

/**
 * Add any saved anchors (currently cached in the Document) to this page.
 *
 * @arg page - Limit the anchors to the bounds of just this page.
 */
void PageContext::add_anchors_for_page(SPPage const *page)
{
    for (CapyPDF_AnnotationId aid : _doc.get_anchors_for_page(page)) {
        _ctx.annotate(aid);
    }
}

/**
 * Add the created annotation to the page.
 */
void PageContext::annotate(CapyPDF_AnnotationId aid)
{
    _ctx.annotate(aid);
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
