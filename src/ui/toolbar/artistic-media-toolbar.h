// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Artistic Media tool controls bar (classic property-bar semantics).
 *
 * Every control stores its /tools/artistic/* preference and applies the same
 * change to the selection: existing artistic media strokes through
 * ArtisticOps::set, plain selected paths through ArtisticOps::apply. Smoothing
 * is creation-only: it stores the preference but never touches the selection.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLBAR_ARTISTIC_MEDIA_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_ARTISTIC_MEDIA_TOOLBAR_H

#include <memory>
#include <string>
#include <vector>

#include "live_effects/artistic-media-library.h"
#include "toolbar.h"

namespace Gtk {
class Box;
class Builder;
class DropDown;
class ToggleButton;
} // namespace Gtk

namespace Inkscape::UI::Widget {
class SpinButton;
} // namespace Inkscape::UI::Widget

namespace Inkscape::ArtisticOps {
struct Options;
}

namespace Inkscape::UI::Toolbar {

class ArtisticMediaToolbar : public Toolbar
{
public:
    ArtisticMediaToolbar();
    ~ArtisticMediaToolbar() override;

    void setDesktop(SPDesktop *desktop) override;

private:
    ArtisticMediaToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    void mode_changed();
    void shape_changed();
    void width_changed();
    void angle_changed();
    void smoothing_changed();
    void scale_changed();

    void rebuild_shape_combo(Inkscape::LivePathEffect::ArtisticMedia::Mode mode, std::string const &select_name);
    void sync_sensitivity();
    void sync_from_prefs();
    void sync_from_selection();
    void apply_to_selection(Inkscape::ArtisticOps::Options const &options);

    bool _updating = false;
    std::vector<std::string> _shape_names; ///< library names of the combo rows
    /// Mode the widgets show (the selected stroke's, else the tool's).
    Inkscape::LivePathEffect::ArtisticMedia::Mode _shown_mode = Inkscape::LivePathEffect::ArtisticMedia::Mode::Preset;

    Gtk::ToggleButton &_mode_preset;
    Gtk::ToggleButton &_mode_brush;
    Gtk::ToggleButton &_mode_calligraphic;
    Gtk::Box &_shape_box;
    Gtk::Box &_angle_box;
    Gtk::DropDown &_shape_combo;
    UI::Widget::SpinButton &_width_item;
    UI::Widget::SpinButton &_angle_item;
    UI::Widget::SpinButton &_smoothing_item;
    Gtk::ToggleButton &_scale_toggle;
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_ARTISTIC_MEDIA_TOOLBAR_H

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
