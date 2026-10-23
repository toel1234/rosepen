// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live Sketch tool controls bar.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live-sketch-toolbar.h"

#include <glibmm/i18n.h>

#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/widget/spinbutton.h"

using Inkscape::UI::Widget::SpinButton;

namespace Inkscape::UI::Toolbar {

namespace {

char const *smoothing_pref = "/tools/livesketch/smoothing";

} // namespace

LiveSketchToolbar::LiveSketchToolbar()
    : LiveSketchToolbar{create_builder("toolbar-live-sketch.ui")}
{
}

LiveSketchToolbar::LiveSketchToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "live-sketch-toolbar")}
    , _smoothing_item{get_derived_widget<SpinButton>(builder, "smoothing_item")}
{
    _smoothing_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &LiveSketchToolbar::smoothing_changed));
    _smoothing_item.setDefocusTarget(this);

    // First show must not touch the prefs: the programmatic value below runs
    // guarded (05-02 gate fix #7).
    _updating = true;
    _smoothing_item.get_adjustment()->set_value(
        Preferences::get()->getDoubleLimited(smoothing_pref, 50.0, 0.0, 100.0));
    _updating = false;
}

LiveSketchToolbar::~LiveSketchToolbar() = default;

void LiveSketchToolbar::smoothing_changed()
{
    if (_updating) {
        return;
    }
    // Creation-only: smoothing never changes existing strokes.
    Preferences::get()->setDouble(smoothing_pref, _smoothing_item.get_adjustment()->get_value());
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
