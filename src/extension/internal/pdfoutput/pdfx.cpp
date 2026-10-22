// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pure PDF/X helpers: mode, page boxes, crop marks, XMP and preflight.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "pdfx.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "build-document.h"
#include "colors/cms/profile.h"
#include "colors/document-cms.h"
#include "colors/spaces/cms.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "object/master-layer.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-marker.h"
#include "object/sp-page.h"
#include "object/sp-pattern.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "page-manager.h"
#include "style.h"

namespace Inkscape::Extension::Internal::PdfX {

namespace {

// Document px are 96 dpi CSS px, so a physical millimetre is this many px.
constexpr double px_per_mm = 96.0 / 25.4;
constexpr double min_mark_offset_mm = 3.0;
constexpr double mark_length_mm = 5.0;

double side_top(Geom::Rect const &rect, Geom::Rect const &outer)
{
    return outer.top() - rect.top();
}

double side_right(Geom::Rect const &rect, Geom::Rect const &outer)
{
    return outer.right() - rect.right();
}

double side_bottom(Geom::Rect const &rect, Geom::Rect const &outer)
{
    return outer.bottom() - rect.bottom();
}

double side_left(Geom::Rect const &rect, Geom::Rect const &outer)
{
    return rect.left() - outer.left();
}

std::string escape_xml(std::string const &text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

std::string page_id(SPPage const *page)
{
    if (auto const id = page->getId()) {
        return id;
    }
    return "page";
}

std::string item_id(SPItem const *item)
{
    if (auto const id = item->getId()) {
        return id;
    }
    return "item";
}

/**
 * True when the image has at least one pixel that is not fully opaque.
 *
 * Every Pixbuf carries an alpha byte. PF_GDK is RGBA, PF_CAIRO is a native
 * ARGB32 word, whose alpha byte sits first on big-endian systems.
 */
bool pixbuf_has_transparency(Inkscape::Pixbuf const &pixbuf)
{
    auto const *data = pixbuf.pixels();
    int const width = pixbuf.width();
    int const height = pixbuf.height();
    int const stride = pixbuf.rowstride();
    if (!data || width <= 0 || height <= 0) {
        return false;
    }

    int alpha = 3;
    if (pixbuf.pixelFormat() == Inkscape::Pixbuf::PF_CAIRO) {
#if G_BYTE_ORDER == G_BIG_ENDIAN
        alpha = 0;
#endif
    }
    for (int y = 0; y < height; ++y) {
        auto const *row = data + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            if (row[x * 4 + alpha] != 255) {
                return true;
            }
        }
    }
    return false;
}

/**
 * The first reason this item is not allowed in a PDF/X file, or an empty
 * string. The image check applies to both modes (an image that cannot be
 * decoded cannot be converted); the transparency rules are X-1a only.
 */
std::string item_pdfx_reason(SPItem const *item, bool pdfx_1a, bool rasterize_filters)
{
    if (auto const *image = cast<SPImage>(item)) {
        if (!image->pixbuf) {
            return "image not decodable for PDF/X";
        }
    }
    if (!pdfx_1a) {
        return {};
    }

    auto const *style = item->style;
    if (!style) {
        return {};
    }
    if (style->opacity.as_double() < 1.0) {
        return "opacity is less than 1";
    }
    if (style->fill_opacity.as_double() < 1.0) {
        return "fill-opacity is less than 1";
    }
    if (style->stroke_opacity.as_double() < 1.0) {
        return "stroke-opacity is less than 1";
    }
    if (item->getMaskObject()) {
        return "mask is applied";
    }
    if (style->mix_blend_mode.set && style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) {
        return "mix-blend-mode is not normal";
    }
    if (PdfBuilder::style_has_gradient_transparency(style)) {
        return "a gradient or mesh paint has transparency";
    }
    if (rasterize_filters && item->isFiltered()) {
        return "a filtered item is rasterized with alpha";
    }
    if (auto const *image = cast<SPImage>(item)) {
        if (image->pixbuf && pixbuf_has_transparency(*image->pixbuf)) {
            return "the image has an alpha channel below 255";
        }
    }
    return {};
}

struct ItemWalk
{
    bool pdfx_1a;
    bool rasterize_filters;
    std::vector<Finding> &findings;
    // Pattern tiles and markers already walked (they may be shared or cyclic).
    std::set<SPObject const *> visited;
    // Items already reported; a clone shares its original's repr.
    std::set<Inkscape::XML::Node const *> reported;
};

void collect_children(SPObject const *object, ItemWalk &walk);

void collect_item(SPItem const *item, ItemWalk &walk)
{
    if (item->isHidden()) {
        return;
    }
    if (auto const reason = item_pdfx_reason(item, walk.pdfx_1a, walk.rasterize_filters); !reason.empty()) {
        if (walk.reported.insert(item->getRepr()).second) {
            walk.findings.push_back({item_id(item), reason, true});
        }
    }
    collect_children(item, walk);

    // Pattern tiles and markers are painted from defs content that the tree
    // walk above never reaches; inspect it through the item that uses it.
    if (auto const *style = item->style) {
        for (SPIPaint const *paint : {static_cast<SPIPaint const *>(&style->fill),
                                      static_cast<SPIPaint const *>(&style->stroke)}) {
            if (!paint->isPaintserver() || !paint->href) {
                continue;
            }
            // Same lookup as Document::get_pattern(): the first pattern in the
            // href chain that has item children supplies the tile.
            for (auto const *pattern = cast<SPPattern>(paint->href->getObject()); pattern;
                 pattern = pattern->ref.getObject()) {
                if (!walk.visited.insert(pattern).second) {
                    break;
                }
                if (pattern->hasItemChildren()) {
                    collect_children(pattern, walk);
                    break;
                }
            }
        }
    }
    if (auto const *shape = cast<SPShape>(item)) {
        for (auto const *marker : shape->_marker) {
            if (marker && walk.visited.insert(marker).second) {
                collect_children(marker, walk);
            }
        }
    }
}

void collect_children(SPObject const *object, ItemWalk &walk)
{
    for (auto const &child : object->children) {
        if (auto const *item = cast<SPItem>(&child)) {
            collect_item(item, walk);
        }
    }
}

} // namespace

Mode mode_from_param(std::string_view value)
{
    if (value == "PDF-X-1a") {
        return Mode::X1a;
    }
    if (value == "PDF-X-4") {
        return Mode::X4;
    }
    return Mode::None;
}

Boxes page_boxes(Geom::Rect const &trim, Geom::Rect const &bleed, bool marks)
{
    Boxes boxes;
    boxes.trim = trim;
    boxes.bleed = bleed;
    if (!marks) {
        boxes.media = bleed;
        return boxes;
    }

    auto const extent = [](double bleed_side) {
        return std::max(bleed_side, min_mark_offset_mm * px_per_mm) + mark_length_mm * px_per_mm;
    };
    auto const top = extent(side_top(trim, bleed));
    auto const right = extent(side_right(trim, bleed));
    auto const bottom = extent(side_bottom(trim, bleed));
    auto const left = extent(side_left(trim, bleed));

    boxes.media = Geom::Rect(Geom::Point(trim.left() - left, trim.top() - top),
                             Geom::Point(trim.right() + right, trim.bottom() + bottom));
    return boxes;
}

std::vector<std::pair<Geom::Point, Geom::Point>> crop_mark_segments(Geom::Rect const &trim,
                                                                    Geom::Rect const &bleed)
{
    auto const offset = [](double bleed_side) {
        return std::max(bleed_side, min_mark_offset_mm * px_per_mm);
    };
    auto const off_top = offset(side_top(trim, bleed));
    auto const off_right = offset(side_right(trim, bleed));
    auto const off_bottom = offset(side_bottom(trim, bleed));
    auto const off_left = offset(side_left(trim, bleed));
    double const length = mark_length_mm * px_per_mm;

    auto const left = trim.left();
    auto const right = trim.right();
    auto const top = trim.top();
    auto const bottom = trim.bottom();

    // Two segments per corner, each parallel to a trim edge. The horizontal
    // segment starts at the side offset and runs outwards; the vertical one
    // does the same along the other axis.
    return {
        {{left - off_left, top}, {left - off_left - length, top}},
        {{left, top - off_top}, {left, top - off_top - length}},
        {{right + off_right, top}, {right + off_right + length, top}},
        {{right, top - off_top}, {right, top - off_top - length}},
        {{left - off_left, bottom}, {left - off_left - length, bottom}},
        {{left, bottom + off_bottom}, {left, bottom + off_bottom + length}},
        {{right + off_right, bottom}, {right + off_right + length, bottom}},
        {{right, bottom + off_bottom}, {right, bottom + off_bottom + length}},
    };
}

std::string xmp_packet(Mode mode, std::string const &title, std::string const &iso_date,
                       std::string const &doc_uuid)
{
    if (mode != Mode::X4) {
        return {};
    }

    std::ostringstream xml;
    xml << "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        << " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        << "  <rdf:Description rdf:about=\"\"\n"
        << "    xmlns:pdfxid=\"http://www.npes.org/pdfx/ns/id/\"\n"
        << "    xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\"\n"
        << "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
        << "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
        << "    xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\">\n"
        << "   <pdfxid:GTS_PDFXVersion>PDF/X-4</pdfxid:GTS_PDFXVersion>\n"
        // Must equal capypdf's Info /Producer, or Info and XMP disagree.
        << "   <pdf:Producer>CapyPDF " CAPYPDF_VERSION_STR "</pdf:Producer>\n"
        << "   <pdf:Trapped>False</pdf:Trapped>\n"
        << "   <xmp:CreateDate>" << iso_date << "</xmp:CreateDate>\n"
        << "   <xmp:ModifyDate>" << iso_date << "</xmp:ModifyDate>\n"
        << "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" << escape_xml(title)
        << "</rdf:li></rdf:Alt></dc:title>\n"
        << "   <xmpMM:DocumentID>" << doc_uuid << "</xmpMM:DocumentID>\n"
        << "   <xmpMM:VersionID>1</xmpMM:VersionID>\n"
        << "   <xmpMM:RenditionClass>default</xmpMM:RenditionClass>\n"
        << "  </rdf:Description>\n"
        << " </rdf:RDF>\n"
        << "</x:xmpmeta>\n"
        << "<?xpacket end=\"w\"?>\n";
    return xml.str();
}

std::vector<Finding> preflight(SPDocument *doc, Mode mode, bool rasterize_filters)
{
    std::vector<Finding> findings;
    if (mode == Mode::None) {
        return findings;
    }
    if (!doc) {
        findings.push_back({"document", "no document", true});
        return findings;
    }

    auto const output_space = doc->getDocumentCMS().getOutputSpace();
    if (!output_space) {
        findings.push_back({"document", "no CMYK output profile selected", true});
    } else if (output_space->getComponentType() != Colors::Space::Type::CMYK) {
        findings.push_back({"document", "the output profile is not a CMYK profile", true});
    } else if (!output_space->hasValidCmsProfile() || !output_space->getProfile()) {
        findings.push_back({"document", "the CMYK output profile could not be loaded", true});
    } else if (output_space->getProfile()->getProfileClass() != cmsSigOutputClass) {
        // The OutputIntent's DestOutputProfile must be an output device (prtr) profile.
        findings.push_back({"document", "the CMYK output profile is not a printer (output class) profile", true});
    } else if (mode == Mode::X1a && cmsGetEncodedICCversion(output_space->getProfile()->getHandle()) >= 0x4000000) {
        // PDF/X-1a:2003 is based on PDF 1.3/1.4, which predate ICC version 4.
        findings.push_back({"document", "PDF/X-1a needs an ICC version 2 output profile", true});
    }

    auto const &pages = doc->getPageManager().getPages();
    for (std::size_t i = 0; i < pages.size(); ++i) {
        auto const *page = pages[i];
        if (!page) {
            continue;
        }
        auto const bleed = page->getDocumentBleed();
        for (std::size_t j = i + 1; j < pages.size(); ++j) {
            auto const *other = pages[j];
            if (!other) {
                continue;
            }
            auto const overlap = bleed & other->getDocumentBleed();
            if (overlap && overlap->area() > 0.0) {
                findings.push_back({page_id(page),
                                    "the bleed of this page overlaps page " + page_id(other),
                                    false});
            }
        }
    }

    for (auto const *layer : MasterLayer::master_layers(doc)) {
        if (layer->getClipObject() || layer->getMaskObject() || layer->isFiltered()) {
            findings.push_back({layer->getId() ? layer->getId() : "master",
                                "a master layer carries its own clip, mask or filter", false});
        }
    }

    // Per-item checks, in document order. Gradients, meshes, pattern tiles and
    // markers are inspected through the item that paints with them, so defs
    // content is covered without walking defs (unused defs are never painted).
    // The root itself is checked too: the exporter paints it as one group, so
    // an opacity or blend mode on <svg> is transparency as well.
    ItemWalk walk{mode == Mode::X1a, rasterize_filters, findings, {}, {}};
    collect_item(doc->getRoot(), walk);

    return findings;
}

} // namespace Inkscape::Extension::Internal::PdfX

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
