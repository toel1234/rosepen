// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for page management, application level and headless.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INK_ACTIONS_PAGE_MANAGER_H
#define INK_ACTIONS_PAGE_MANAGER_H

#include <optional>
#include <string>
#include <string_view>

#include <2geom/point.h>

class SPDocument;
class SPPage;
class InkscapeApplication;

namespace Inkscape::PageOps {

enum class Where
{
    Before,
    After,
    End,
};

enum class Orientation
{
    Portrait,
    Landscape,
};

SPPage *current_page(SPDocument *doc);
bool goto_page(SPDocument *doc, int pos);
bool move_to(SPDocument *doc, SPPage *page, int pos);
SPPage *insert_page(SPDocument *doc, Where where);
SPPage *duplicate_after(SPDocument *doc);
bool remove_page(SPDocument *doc, bool contents);
bool rename_page(SPDocument *doc, std::string_view text);
std::optional<Geom::Point> parse_size(SPDocument *doc, std::string_view spec);
bool resize_page(SPDocument *doc, SPPage *page, Geom::Point px, std::optional<Orientation> orientation,
                 std::string preset_label);
std::string list_pages(SPDocument *doc);

} // namespace Inkscape::PageOps

void add_actions_page_manager(InkscapeApplication *app);

#endif // INK_ACTIONS_PAGE_MANAGER_H

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