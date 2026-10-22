// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ObjectManager page-view filter implementation.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/dialog/object-manager-filter.h"

#include "document.h"
#include "object/master-layer.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-object.h"
#include "object/sp-page.h"
#include "page-manager.h"
#include "util/cast.h"

namespace Inkscape::UI::Dialog::ObjectManager {

SPPage *filter_page(SPDocument *doc)
{
    if (!doc) {
        return nullptr;
    }

    auto &pages = doc->getPageManager();
    if (!pages.hasPages()) {
        // Implicit single page: nothing to filter against.
        return nullptr;
    }
    if (auto *selected = pages.getSelected()) {
        return selected;
    }
    return pages.getPage(0);
}

bool page_filter_active(SPDocument *doc, bool enabled)
{
    return enabled && filter_page(doc);
}

bool page_filter_shows(SPItem *item, SPPage const *page, bool enabled)
{
    // Rule 1: no filter, no page, or no item: show everything.
    if (!enabled || !page || !item) {
        return true;
    }

    // Find the deciding item: the nearest ancestor-or-self whose parent is a
    // layer or the document root. Deciding at the top-level item keeps the
    // result identical for an item and all its descendants.
    SPObject *top = item;
    while (SPObject *parent = top->parent) {
        if (auto *group = cast<SPGroup>(parent);
            group && group->layerMode() == SPGroup::LAYER)
        {
            break; // parent is a layer
        }
        if (!parent->parent) {
            break; // parent is the document root
        }
        top = parent;
    }

    // Rule 2: a layer itself (or the root) is always listed; layers are
    // document-wide, like Corel listing every layer under every page.
    if (!top->parent) {
        return true; // the document root itself
    }
    if (auto *group = cast<SPGroup>(top); group && group->layerMode() == SPGroup::LAYER) {
        return true;
    }
    auto *top_item = cast<SPItem>(top);
    if (!top_item) {
        return true; // non-item container: never hide
    }

    // Rule 3: master layer content is shown on every page.
    if (MasterLayer::in_master(top_item)) {
        return true;
    }

    // Rule 4: an object without a bounding box (empty group, empty text)
    // cannot be placed on a page; hiding it would lose it.
    auto const bounds = top_item->desktopGeometricBounds();
    if (!bounds) {
        return true;
    }

    // Rule 5: the object intersects the current page; straddlers list on
    // every page they touch.
    if (page->itemOnPage(top_item, false)) {
        return true;
    }

    // Rule 6: the page manager finds no page for the object: pasteboard
    // content, shared like Corel's Desktop layer; never hide off-page objects.
    if (!page->document->getPageManager().getPageFor(top_item, false)) {
        return true;
    }

    // Rule 7: the object lies on another page only.
    return false;
}

} // namespace Inkscape::UI::Dialog::ObjectManager

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
