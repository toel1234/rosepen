// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Build one document from the per-page SVG strings of an RVNG drawing import.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "rvng-pages.h"

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>
#include <2geom/rect.h>

#include "document.h"
#include "document-undo.h"
#include "id-clash.h"
#include "page-manager.h"
#include "object/sp-defs.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "util/units.h"
#include "xml/document.h"
#include "xml/node.h"

namespace Inkscape::Extension::Internal::RvngPages {

namespace {

char const *const SKIPPED_CONTENT_TAGS[] = {
    "svg:defs",
    "sodipodi:namedview",
    "svg:metadata",
    "svg:title",
    "svg:desc",
};

/// Whether the node is page content rather than document furniture.
bool is_content(Inkscape::XML::Node const *node)
{
    for (auto tag : SKIPPED_CONTENT_TAGS) {
        if (std::strcmp(node->name(), tag) == 0) {
            return false;
        }
    }
    return true;
}

/// Locale independent SVG number, e.g. "154" or "0.5".
std::string number(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(buffer, sizeof buffer, value);
    return buffer;
}

/// Transform placing the content of a page of another document scale on its page.
std::string layer_transform(double scale, double x, double y)
{
    // Unit conversions (points to pixels and back) leave a scale a rounding
    // error away from 1; treat that as the plain page translation it is.
    if (std::abs(scale - 1.0) < 1e-12) {
        return "translate(" + number(x) + "," + number(y) + ")";
    }
    return "matrix(" + number(scale) + ",0,0," + number(scale) + "," + number(x) + "," + number(y) + ")";
}

Inkscape::XML::Node *append_layer(SPDocument *doc, std::string const &label, std::string const &transform = {})
{
    auto *xml_doc = doc->getReprDoc();
    auto *layer = xml_doc->createElement("svg:g");
    layer->setAttribute("inkscape:groupmode", "layer");
    layer->setAttribute("inkscape:label", label.c_str());
    if (!transform.empty()) {
        layer->setAttribute("transform", transform.c_str());
    }
    doc->getReprRoot()->appendChild(layer);
    Inkscape::GC::release(layer);
    return layer;
}

/// Copy a page's defs children into the base document, then its content into the layer.
void duplicate_page_children(SPDocument *page_doc, SPDocument *doc, Inkscape::XML::Node *layer)
{
    auto *xml_doc = doc->getReprDoc();
    auto *page_root = page_doc->getReprRoot();

    auto *target_defs = doc->getDefs()->getRepr();
    for (auto *child = page_root->firstChild(); child; child = child->next()) {
        if (std::strcmp(child->name(), "svg:defs") != 0) {
            continue;
        }
        for (auto *node = child->firstChild(); node; node = node->next()) {
            // The id clash resolver keeps the id of a gradient equivalent to one
            // already in the document (repeated master content); references then
            // resolve to that one, and a copy would only duplicate the id.
            if (auto id = node->attribute("id"); id && doc->getObjectById(id)) {
                continue;
            }
            auto *copy = node->duplicate(xml_doc);
            target_defs->appendChild(copy);
            Inkscape::GC::release(copy);
        }
    }

    for (auto *child = page_root->firstChild(); child; child = child->next()) {
        if (!is_content(child)) {
            continue;
        }
        auto *copy = child->duplicate(xml_doc);
        layer->appendChild(copy);
        Inkscape::GC::release(copy);
    }
}

} // namespace

std::unique_ptr<SPDocument> document_from_page(std::string const &svg)
{
    auto doc = SPDocument::createNewDocFromMem({svg.data(), svg.size()});

    if (doc && !doc->getRoot()->viewBox_set) {
        // Scales the document to account for 72dpi scaling in older generator versions
        doc->setWidth(Inkscape::Util::Quantity(doc->getWidth().quantity, "pt"), false);
        doc->setHeight(Inkscape::Util::Quantity(doc->getHeight().quantity, "pt"), false);
        doc->setViewBox(Geom::Rect::from_xywh(0, 0, doc->getWidth().value("pt"), doc->getHeight().value("pt")));
    }

    return doc;
}

Result document_from_pages(std::span<std::string const> pages)
{
    Result result;

    if (pages.empty()) {
        return result;
    }

    auto base = document_from_page(pages[0]);
    if (!base) {
        result.failed_pages.push_back(1);
        return result;
    }

    if (pages.size() == 1) {
        result.doc = std::move(base);
        return result;
    }

    auto *doc = base.get();
    auto const base_scale = doc->getDocumentScale();

    {
        Inkscape::DocumentUndo::ScopedInsensitive no_undo(doc);
        auto &page_manager = doc->getPageManager();
        page_manager.enablePages();

        // Page 1 keeps its content as is; it just moves into a layer of its own.
        auto *root = doc->getReprRoot();
        std::vector<Inkscape::XML::Node *> first_page_content;
        for (auto *child = root->firstChild(); child; child = child->next()) {
            if (is_content(child)) {
                first_page_content.push_back(child);
            }
        }
        auto *first_layer = append_layer(doc, "Page 1");
        for (auto *node : first_page_content) {
            Inkscape::GC::anchor(node);
            node->parent()->removeChild(node);
            first_layer->appendChild(node);
            Inkscape::GC::release(node);
        }

        double last_width = page_manager.getPage(0)->getRect().width();
        double last_height = page_manager.getPage(0)->getRect().height();

        for (std::size_t i = 1; i < pages.size(); ++i) {
            int const page_number = static_cast<int>(i) + 1;
            std::string const label = "Page " + std::to_string(page_number);

            auto page_doc = document_from_page(pages[i]);
            if (!page_doc) {
                result.failed_pages.push_back(page_number);
                page_manager.newPage(last_width, last_height);
                append_layer(doc, label);
                continue;
            }

            page_doc->ensureUpToDate();
            prevent_id_clashes(page_doc.get(), doc);

            auto const page_scale = page_doc->getDocumentScale();
            double const width = page_doc->getWidth().value("px") / base_scale[0];
            double const height = page_doc->getHeight().value("px") / base_scale[1];

            if (auto *page = page_manager.newPage(width, height)) {
                auto const rect = page->getRect();
                double const scale = page_scale[0] / base_scale[0];
                auto *layer = append_layer(doc, label, layer_transform(scale, rect.left(), rect.top()));
                duplicate_page_children(page_doc.get(), doc, layer);
                last_width = width;
                last_height = height;
            } else {
                result.failed_pages.push_back(page_number);
            }
        }

        doc->ensureUpToDate();
    }

    result.doc = std::move(base);
    return result;
}

} // namespace Inkscape::Extension::Internal::RvngPages

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
