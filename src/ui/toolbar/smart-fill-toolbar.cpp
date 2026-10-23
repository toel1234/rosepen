// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Smart Fill tool controls bar.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "smart-fill-toolbar.h"

#include <glibmm/i18n.h>

#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>

#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/widget/spinbutton.h"

using Inkscape::UI::Widget::SpinButton;

namespace Inkscape::UI::Toolbar {

namespace {

char const *fillmode_pref = "/tools/smartfill/fillmode";
char const *strokemode_pref = "/tools/smartfill/strokemode";
char const *strokewidth_pref = "/tools/smartfill/strokewidth";

// DropDown row order of toolbar-smart-fill.ui.
char const *fill_mode_keys[] = {"source", "tool", "none"};
char const *stroke_mode_keys[] = {"none", "source", "tool"};

/// The stored mode, or the given default when unknown.
std::string stored_mode(char const *pref, char const *const *keys, std::size_t n, char const *dflt)
{
    auto const key = Preferences::get()->getString(pref, dflt);
    for (std::size_t i = 0; i < n; ++i) {
        if (key == keys[i]) {
            return keys[i];
        }
    }
    return dflt;
}

int selected_of(Gtk::DropDown const &combo)
{
    auto const selected = combo.get_selected();
    return selected == GTK_INVALID_LIST_POSITION ? 0 : selected;
}

} // namespace

SmartFillToolbar::SmartFillToolbar()
    : SmartFillToolbar{create_builder("toolbar-smart-fill.ui")}
{
}

SmartFillToolbar::SmartFillToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "smart-fill-toolbar")}
    , _fill_mode_combo{get_widget<Gtk::DropDown>(builder, "fill_mode_combo")}
    , _stroke_mode_combo{get_widget<Gtk::DropDown>(builder, "stroke_mode_combo")}
    , _stroke_width_item{get_derived_widget<SpinButton>(builder, "stroke_width_item")}
{
    auto const prefs = Preferences::get();

    _fill_mode_combo.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &SmartFillToolbar::fill_mode_changed));

    _stroke_mode_combo.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &SmartFillToolbar::stroke_mode_changed));

    _stroke_width_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &SmartFillToolbar::stroke_width_changed));
    _stroke_width_item.setDefocusTarget(this);

    // First show must not touch the document or the prefs: everything
    // programmatic below runs guarded (05-02 gate fix #7).
    _updating = true;

    auto const fillmode = stored_mode(fillmode_pref, fill_mode_keys, G_N_ELEMENTS(fill_mode_keys), "source");
    for (std::size_t i = 0; i < G_N_ELEMENTS(fill_mode_keys); ++i) {
        if (fillmode == fill_mode_keys[i]) {
            _fill_mode_combo.set_selected(i);
            break;
        }
    }

    auto const strokemode = stored_mode(strokemode_pref, stroke_mode_keys, G_N_ELEMENTS(stroke_mode_keys), "none");
    for (std::size_t i = 0; i < G_N_ELEMENTS(stroke_mode_keys); ++i) {
        if (strokemode == stroke_mode_keys[i]) {
            _stroke_mode_combo.set_selected(i);
            break;
        }
    }

    _stroke_width_item.get_adjustment()->set_value(prefs->getDoubleLimited(strokewidth_pref, 0.25, 0.01, 100.0));
    _stroke_width_item.set_sensitive(strokemode != "none");

    _updating = false;
}

SmartFillToolbar::~SmartFillToolbar() = default;

void SmartFillToolbar::fill_mode_changed()
{
    if (_updating) {
        return;
    }
    auto const index = selected_of(_fill_mode_combo);
    Preferences::get()->setString(fillmode_pref, fill_mode_keys[index < 3 ? index : 0]);
}

void SmartFillToolbar::stroke_mode_changed()
{
    if (_updating) {
        return;
    }
    auto const index = selected_of(_stroke_mode_combo);
    Preferences::get()->setString(strokemode_pref, stroke_mode_keys[index < 3 ? index : 0]);
    // The width only matters when an outline is actually drawn.
    _stroke_width_item.set_sensitive(index != 0);
}

void SmartFillToolbar::stroke_width_changed()
{
    if (_updating) {
        return;
    }
    Preferences::get()->setDouble(strokewidth_pref, _stroke_width_item.get_adjustment()->get_value());
}

} // namespace Inkscape::UI::Toolbar

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
