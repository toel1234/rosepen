// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PropertiesPanelKind implementation.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/dialog/properties-panel-kind.h"

#include "object/sp-anchor.h"
#include "object/sp-ellipse.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "object/sp-polygon.h"
#include "object/sp-polyline.h"
#include "object/sp-rect.h"
#include "object/sp-star.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "util/cast.h"

namespace Inkscape::UI::Dialog {

namespace {

// The exact-tag -> panel mapping. Mirrors upstream
// ObjectAttributes::create_panel (object-attributes.cpp:2546-2562 at
// fork-base); keep byte-identical so upstream panel choice never changes.
struct TagKind {
    int tag;
    PropertiesPanelKind kind;
};

constexpr TagKind tag_table[] = {
    {tag_of<SPImage>,          PropertiesPanelKind::Image},
    {tag_of<SPRect>,           PropertiesPanelKind::Rect},
    {tag_of<SPGenericEllipse>, PropertiesPanelKind::Ellipse},
    {tag_of<SPStar>,           PropertiesPanelKind::Star},
    {tag_of<SPAnchor>,         PropertiesPanelKind::Anchor},
    {tag_of<SPPath>,           PropertiesPanelKind::Path},
    {tag_of<SPPolyLine>,       PropertiesPanelKind::Polyline},
    {tag_of<SPPolygon>,        PropertiesPanelKind::Polygon},
    {tag_of<SPGroup>,          PropertiesPanelKind::Group},
    {tag_of<SPText>,           PropertiesPanelKind::Text},
    {tag_of<SPUse>,            PropertiesPanelKind::Clone},
};

constexpr int table_size = sizeof(tag_table) / sizeof(tag_table[0]);

} // namespace

PropertiesPanelKind properties_panel_for(SPItem const *item)
{
    if (!item) {
        return PropertiesPanelKind::None;
    }

    int const tag = item->tag();
    for (int i = 0; i < table_size; ++i) {
        if (tag_table[i].tag == tag) {
            return tag_table[i].kind;
        }
    }
    return PropertiesPanelKind::Generic;
}

PropertiesPanelKind properties_panel_for(std::size_t selection_size, SPItem const *single_item)
{
    if (selection_size == 0) {
        return PropertiesPanelKind::Empty;
    }
    if (selection_size > 1) {
        return PropertiesPanelKind::Multi;
    }
    return properties_panel_for(single_item);
}

} // namespace Inkscape::UI::Dialog

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
