// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Master layers: top-level layers whose content is shown on every page.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "master-layer.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <glib.h>

#include "document.h"
#include "page-manager.h"
#include "sp-item-group.h"
#include "sp-item.h"
#include "sp-object.h"
#include "sp-page.h"
#include "sp-root.h"

namespace Inkscape::MasterLayer {

bool is_master(SPObject const *object)
{
    auto const *layer = cast<SPGroup>(object);
    if (!layer || !SP_IS_LAYER(layer)) {
        return false;
    }
    auto const *document = layer->document;
    if (!document) {
        return false;
    }
    auto const *root = document->getRoot();
    if (!root || layer->parent != static_cast<SPObject const *>(root)) {
        return false;
    }
    return g_strcmp0(layer->getAttribute(attr), "true") == 0;
}

bool set_master(SPGroup *layer, bool on)
{
    if (!layer) {
        g_warning("%s: refusing a null layer", attr);
        return false;
    }
    if (!SP_IS_LAYER(layer)) {
        g_warning("%s: refusing an object that is not a layer", attr);
        return false;
    }
    auto *document = layer->document;
    auto *root = document ? document->getRoot() : nullptr;
    if (!root || layer->parent != root) {
        g_warning("%s: refusing a layer that is not a direct child of the root", attr);
        return false;
    }

    bool const current = is_master(layer);
    if (on) {
        if (current) {
            return false;
        }
        if (layer->getClipObject() || layer->getMaskObject() || layer->isFiltered()) {
            g_warning("%s: a clip, mask or filter on the master layer itself is evaluated in "
                      "page-1 space and clips or filters the copies on later pages", attr);
        }
        layer->setAttribute(attr, "true");
        return true;
    }

    if (!current) {
        return false;
    }
    layer->removeAttribute(attr);
    return true;
}

std::vector<SPGroup *> master_layers(SPDocument *doc)
{
    std::vector<SPGroup *> layers;
    if (!doc) {
        return layers;
    }
    auto *root = doc->getRoot();
    if (!root) {
        return layers;
    }
    for (auto *item : root->item_list()) {
        if (auto *layer = cast<SPGroup>(item); layer && is_master(layer)) {
            layers.push_back(layer);
        }
    }
    return layers;
}

bool in_master(SPItem const *item)
{
    for (auto const *object = static_cast<SPObject const *>(item); object; object = object->parent) {
        if (is_master(object)) {
            return true;
        }
    }
    return false;
}

std::vector<SPItem *> without_master(std::vector<SPItem *> items)
{
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](SPItem *item) { return in_master(item); }),
                items.end());
    return items;
}

std::vector<SPItem *> page_export_items(SPDocument *doc, SPPage const *page)
{
    if (!doc || !page) {
        return {};
    }
    auto items = page->getOverlappingItems(true, true);
    for (auto *layer : master_layers(doc)) {
        bool const contained = std::any_of(items.begin(), items.end(),
                                           [layer](SPItem const *item) {
                                               return layer->isAncestorOf(item);
                                           });
        if (!contained) {
            items.push_back(layer);
        }
    }
    return items;
}

std::vector<std::pair<SPPage *, Geom::Point>> page_offsets(SPDocument *doc)
{
    std::vector<std::pair<SPPage *, Geom::Point>> offsets;
    if (!doc) {
        return offsets;
    }
    auto &pages = doc->getPageManager();
    if (pages.getPageCount() < 2) {
        return offsets;
    }
    auto *first = pages.getFirstPage();
    if (!first) {
        return offsets;
    }
    auto const origin = first->getRect().corner(0);
    for (int i = 1; i < pages.getPageCount(); ++i) {
        auto *page = pages.getPage(i);
        if (page) {
            offsets.emplace_back(page, page->getRect().corner(0) - origin);
        }
    }
    return offsets;
}

void reanchor_before_delete(SPDocument *doc, SPPage *deleted)
{
    if (!doc || !deleted) {
        return;
    }
    auto &pages = doc->getPageManager();
    if (pages.getPageCount() < 2 || pages.getFirstPage() != deleted) {
        return;
    }
    auto *second = pages.getPage(1);
    if (!second) {
        return;
    }
    auto const layers = master_layers(doc);
    if (layers.empty()) {
        return;
    }

    auto const delta = second->getDesktopRect().corner(0) - deleted->getDesktopRect().corner(0);
    if (delta.x() == 0 && delta.y() == 0) {
        return;
    }
    Geom::Translate const translate(delta);
    for (auto *layer : layers) {
        layer->move_rel(translate);
    }
}

} // namespace Inkscape::MasterLayer

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
