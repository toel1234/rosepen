// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dimension tool controls bar.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLBAR_DIMENSION_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_DIMENSION_TOOLBAR_H

#include <memory>
#include <string>

#include "toolbar.h"

namespace Gtk {
class Builder;
class DropDown;
class ToggleButton;
} // namespace Gtk

namespace Inkscape::UI::Widget {
class SpinButton;
class UnitTracker;
} // namespace Inkscape::UI::Widget

namespace Inkscape::UI::Toolbar {

class DimensionToolbar : public Toolbar
{
public:
    DimensionToolbar();
    ~DimensionToolbar() override;

    void setDesktop(SPDesktop *desktop) override;

private:
    DimensionToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    void mode_changed();
    void precision_changed();
    void offset_changed();
    void text_changed();
    void units_changed();
    void unit_changed();

    void sync_mode_toggles();
    void apply_to_selection(char const *key, std::string const &value);

    std::unique_ptr<UI::Widget::UnitTracker> _tracker;
    bool _unit_set = false;
    bool _updating = false;

    Gtk::ToggleButton &_mode_auto;
    Gtk::ToggleButton &_mode_parallel;
    Gtk::ToggleButton &_mode_angular;
    Gtk::ToggleButton &_mode_segment;

    UI::Widget::SpinButton &_precision_item;
    UI::Widget::SpinButton &_offset_item;

    Gtk::DropDown &_text_combo;
    Gtk::ToggleButton &_units_btn;
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_DIMENSION_TOOLBAR_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((inclass . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
