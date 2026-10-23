// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Smart Fill tool: create a new object covering the area under the cursor.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "smart-fill-tool.h"

#include <string>

#include <gdk/gdk.h>
#include <glibmm/i18n.h>

#include "actions/actions-smart-fill.h"
#include "colors/color.h"
#include "desktop-style.h"
#include "desktop.h"
#include "document.h"
#include "layer-manager.h"
#include "message-stack.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "ui/widget/events/canvas-event.h"

namespace Inkscape::UI::Tools {

namespace {

char const *fillmode_pref = "/tools/smartfill/fillmode";
char const *strokemode_pref = "/tools/smartfill/strokemode";
char const *strokewidth_pref = "/tools/smartfill/strokewidth";

/// The tool's configured fill or stroke colour, or #000000 when unset.
std::string tool_color(SPDesktop *desktop, bool is_fill)
{
    if (auto const color = sp_desktop_get_color_tool(desktop, "/tools/smartfill", is_fill)) {
        return color->toString(false);
    }
    return "#000000";
}

/// One of "source" (fill) / "none" (stroke), "tool" or "none", default as given.
std::string mode_from_prefs(char const *pref, std::string const &dflt)
{
    auto const key = Preferences::get()->getString(pref, dflt);
    if (key == "source" || key == "tool" || key == "none") {
        return key.raw();
    }
    return dflt;
}

} // namespace

SmartFillTool::SmartFillTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/smartfill", "flood.svg")
{
}

SmartFillTool::~SmartFillTool() = default;

void SmartFillTool::click(Geom::Point const &position_window)
{
    auto *doc = _desktop->getDocument();
    if (!doc) {
        return;
    }

    // The action takes document USER units (viewBox units); event positions
    // are window (world) coordinates: window -> desktop -> document -> uu.
    Geom::Point const p =
        _desktop->dt2doc(_desktop->w2d(position_window)) * doc->getRoot()->c2p.inverse();

    auto const prefs = Preferences::get();
    auto const fillmode = mode_from_prefs(fillmode_pref, "source");
    auto const strokemode = mode_from_prefs(strokemode_pref, "none");

    Inkscape::SmartFillOps::Options options;
    auto const fill_color = tool_color(_desktop, /*is_fill=*/true);
    if (fillmode == "source") {
        options.fill = "source";
        options.fallback_fill = fill_color;
    } else if (fillmode == "tool") {
        options.fill = fill_color;
    } else {
        options.fill = "none";
    }

    auto const stroke_color = tool_color(_desktop, /*is_fill=*/false);
    if (strokemode == "source") {
        options.stroke = "source";
        options.fallback_stroke = stroke_color;
    } else if (strokemode == "tool") {
        options.stroke = stroke_color;
    } else {
        options.stroke = "none";
    }
    if (strokemode != "none") {
        options.stroke_width_mm = prefs->getDoubleLimited(strokewidth_pref, 0.25, 0.01, 100.0);
    }

    // SmartFillOps::create records the single undo step itself.
    auto const result = Inkscape::SmartFillOps::create(doc, p, options, _desktop->layerManager().currentLayer());
    if (result.path) {
        _desktop->getSelection()->set(result.path);
    } else if (result.status == Inkscape::SmartFill::Status::OnBoundary) {
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("Click inside an area, not on an outline."));
    } else {
        // NoRegion (the tool passes a layer and no id, so parent/id errors
        // cannot occur); result.error is the untranslated action text.
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("No enclosed area under the cursor."));
    }
}

bool SmartFillTool::root_handler(CanvasEvent const &event)
{
    bool ret = false;

    inspect_event(
        event,
        [&](ButtonPressEvent const &press) {
            if (press.button != 1 || press.num_press != 1) {
                return;
            }
            ret = true;

            _pressed = true;
            _dragged = false;
            saveDragOrigin(press.pos);
        },
        [&](MotionEvent const &motion) {
            if (!_pressed || !(motion.modifiers & GDK_BUTTON1_MASK)) {
                return;
            }
            if (!_dragged && !checkDragMoved(motion.pos)) {
                return; // still within the click tolerance
            }
            _dragged = true;
        },
        [&](ButtonReleaseEvent const &release) {
            if (release.button != 1) {
                return;
            }
            if (_pressed && !_dragged) {
                click(release.pos);
            }
            _pressed = false;
            _dragged = false;
            ret = true;
        },
        [&](KeyPressEvent const &key) {
            if (key.keyval == GDK_KEY_Escape) {
                _pressed = false;
                _dragged = false;
                ret = true;
            }
        },
        [](CanvasEvent const &) {}
    );

    return ret ? true : ToolBase::root_handler(event);
}

} // namespace Inkscape::UI::Tools

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
