// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Master layers: top-level layers whose content is shown on every page.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef INK_OBJECT_MASTER_LAYER_H
#define INK_OBJECT_MASTER_LAYER_H

#include <utility>
#include <vector>

#include <2geom/point.h>

class SPDocument;
class SPGroup;
class SPItem;
class SPObject;
class SPPage;

namespace Inkscape::MasterLayer {

inline constexpr char const *attr = "inkscape:master-layer";

bool is_master(SPObject const *object);
bool set_master(SPGroup *layer, bool on);
std::vector<SPGroup *> master_layers(SPDocument *doc);
bool in_master(SPItem const *item);
std::vector<SPItem *> without_master(std::vector<SPItem *> items);
std::vector<std::pair<SPPage *, Geom::Point>> page_offsets(SPDocument *doc);
void reanchor_before_delete(SPDocument *doc, SPPage *deleted);

/**
 * The items to export for one page: the items overlapping it, plus every
 * master layer that has no item among them yet (document order). This is the
 * export dialog's per-page item set, so master layers survive the crop on
 * pages other than the first.
 */
std::vector<SPItem *> page_export_items(SPDocument *doc, SPPage const *page);

} // namespace Inkscape::MasterLayer

#endif // INK_OBJECT_MASTER_LAYER_H

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
