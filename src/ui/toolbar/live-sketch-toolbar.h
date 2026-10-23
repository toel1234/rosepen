// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live Sketch tool controls bar.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLBAR_LIVE_SKETCH_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_LIVE_SKETCH_TOOLBAR_H

#include "toolbar.h"

namespace Gtk {
class Builder;
} // namespace Gtk

namespace Inkscape::UI::Widget {
class SpinButton;
} // namespace Inkscape::UI::Widget

namespace Inkscape::UI::Toolbar {

class LiveSketchToolbar : public Toolbar
{
public:
    LiveSketchToolbar();
    ~LiveSketchToolbar() override;

private:
    LiveSketchToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    void smoothing_changed();

    UI::Widget::SpinButton &_smoothing_item;

    bool _updating = false; ///< guards programmatic changes (no pref write on first show)
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_LIVE_SKETCH_TOOLBAR_H

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
