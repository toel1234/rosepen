// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PropertiesPanelKind - which Object Properties panel a selection gets.
 *
 * Pure model: the exact tag of a single selected item -> the panel kind the
 * Object Properties docker hosts; selection size -> empty/multi. Every item
 * type upstream has no specific panel for maps to Generic, so the panel is
 * never blank. No Gtk, no widget code.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_UI_DIALOG_PROPERTIES_PANEL_KIND_H
#define SEEN_UI_DIALOG_PROPERTIES_PANEL_KIND_H

#include <cstddef>

class SPItem;

namespace Inkscape::UI::Dialog {

enum class PropertiesPanelKind {
    None,     // no single item (upstream: get_panel returned nullptr)
    Empty,    // empty selection
    Multi,    // more than one selected object
    Image,
    Rect,
    Ellipse,
    Star,
    Anchor,
    Path,
    Polyline,
    Polygon,
    Group,
    Text,
    Clone,
    Generic,  // any single item without its own upstream panel
};

/**
 * The panel kind for the exact tag of @a item: byte-for-byte the upstream
 * ObjectAttributes::create_panel mapping for the 11 kinds it handles, Generic
 * for any other non-null item, None for nullptr.
 */
PropertiesPanelKind properties_panel_for(SPItem const *item);

/**
 * The panel kind for a selection of @a selection_size objects of which
 * @a single_item is the only item when selection_size == 1 (upstream
 * singleItem()): 0 -> Empty, >1 -> Multi, 1 -> properties_panel_for(item).
 */
PropertiesPanelKind properties_panel_for(std::size_t selection_size, SPItem const *single_item);

} // namespace Inkscape::UI::Dialog

#endif // SEEN_UI_DIALOG_PROPERTIES_PANEL_KIND_H

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
