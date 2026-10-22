// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ObjectManager page-view filter - which objects the Object Manager lists.
 *
 * Pure model: the selected page + the "Current page only" preference decide,
 * per top-level object, whether the object tree shows it. Master layer
 * content, straddling objects and pasteboard objects stay visible on every
 * page; layers are document-wide and are always listed.
 * No Gtk, no preferences access, no widget code.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_UI_DIALOG_OBJECT_MANAGER_FILTER_H
#define SEEN_UI_DIALOG_OBJECT_MANAGER_FILTER_H

class SPDocument;
class SPItem;
class SPPage;

namespace Inkscape::UI::Dialog::ObjectManager {

/// Live preference gating the page view (bool, default false: off = the exact upstream tree).
inline constexpr char const *current_page_only_pref_path = "/dialogs/objects/current_page_only";

/**
 * The page whose objects the Object Manager would list, or nullptr when the
 * view must not be filtered: no document, or an implicit single page (a
 * document without <view> pages has nothing to filter against).
 * With pages present: the selected page, else page 1.
 */
SPPage *filter_page(SPDocument *doc);

/// Whether the page view is active at all: @a enabled and a page to filter by.
bool page_filter_active(SPDocument *doc, bool enabled);

/**
 * The 7-rule page-view decision for one row of the object tree, in order:
 *  1. no filter, no page, or no item -> show;
 *  2. layers and the document root are always listed;
 *  3. master layer content is shown on every page;
 *  4. an object without a bounding box cannot be placed, show it;
 *  5. the object intersects the current page (straddlers list on both pages);
 *  6. no page holds the object -> pasteboard content, never hidden;
 *  7. otherwise the object lies on another page only -> hide.
 * The decision is taken at the top-level item (the nearest ancestor-or-self
 * whose parent is a layer or the root), so an item and all its descendants
 * give the same answer.
 */
bool page_filter_shows(SPItem *item, SPPage const *page, bool enabled);

} // namespace Inkscape::UI::Dialog::ObjectManager

#endif // SEEN_UI_DIALOG_OBJECT_MANAGER_FILTER_H

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
