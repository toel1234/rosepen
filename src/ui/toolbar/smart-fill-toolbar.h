// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Smart Fill tool controls bar.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLBAR_SMART_FILL_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_SMART_FILL_TOOLBAR_H

#include "toolbar.h"

namespace Gtk {
class Builder;
class DropDown;
} // namespace Gtk

namespace Inkscape::UI::Widget {
class SpinButton;
} // namespace Inkscape::UI::Widget

namespace Inkscape::UI::Toolbar {

class SmartFillToolbar : public Toolbar
{
public:
    SmartFillToolbar();
    ~SmartFillToolbar() override;

private:
    SmartFillToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    void fill_mode_changed();
    void stroke_mode_changed();
    void stroke_width_changed();

    Gtk::DropDown &_fill_mode_combo;
    Gtk::DropDown &_stroke_mode_combo;
    UI::Widget::SpinButton &_stroke_width_item;

    bool _updating = false; ///< guards programmatic changes (no pref write on first show)
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_SMART_FILL_TOOLBAR_H

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
