// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live Sketch tool: freehand strokes smoothed live into plain editable paths.
 *
 * While the user drags, every captured position feeds a windowed fitter whose
 * smooth curve (not the raw mouse trail) follows the cursor. On release the
 * whole captured polyline is committed through LiveSketchOps::create, the
 * same entry point as app.live-sketch, so the tool and the headless corpus
 * produce identical objects. A click without drag creates nothing.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live-sketch-tool.h"

#include <gdk/gdk.h>
#include <glibmm/i18n.h>

#include <2geom/pathvector.h>

#include "actions/actions-live-sketch.h"
#include "colors/color.h"
#include "desktop-style.h"
#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "document.h"
#include "layer-manager.h"
#include "message-stack.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/repr.h"

namespace Inkscape::UI::Tools {

namespace {

/// Minimum distance between captured points, in window pixels: the only
/// view-dependent input; the committed geometry stays view-independent.
constexpr double kMinCaptureDistanceWindowPx = 1.0;

char const *smoothing_pref = "/tools/livesketch/smoothing";

/// The property-bar smoothing, limited to 0..100; the preview and the commit
/// both use it so the release fit matches what was drawn.
double smoothing_preference()
{
    return Preferences::get()->getDoubleLimited(smoothing_pref, LiveSketch::kDefaultSmoothing, 0.0, 100.0);
}

} // namespace

LiveSketchTool::LiveSketchTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/livesketch", "pencil.svg")
{
}

LiveSketchTool::~LiveSketchTool()
{
    ungrabCanvasEvents();
    clear_preview();
}

void LiveSketchTool::reset()
{
    _fitter.reset();
    _points_uu.clear();
    _dragging = false;
}

bool LiveSketchTool::capture(Geom::Point const &pos_window)
{
    if (!_fitter || !_desktop->getDocument()) {
        return false;
    }
    if (!_points_uu.empty() &&
        Geom::distance(pos_window, _last_capture_window_pos) < kMinCaptureDistanceWindowPx) {
        return false;
    }

    // Event positions are window coordinates. Capture in document pixels
    // (the root viewBox scale included) and keep the commit polyline in
    // document user units, the space of LiveSketchOps::create.
    Geom::Point const p_px = _desktop->dt2doc(_desktop->w2d(pos_window));
    if (!LiveSketch::in_plane(p_px) || !_fitter->add(p_px)) {
        return false;
    }

    _last_capture_window_pos = pos_window;
    _points_uu.push_back(p_px * _desktop->getDocument()->getRoot()->c2p.inverse());
    return true;
}

void LiveSketchTool::clear_preview()
{
    if (_preview_path) {
        _preview_path->set_visible(false);
    }
}

void LiveSketchTool::update_preview()
{
    if (!_fitter) {
        clear_preview();
        return;
    }
    Geom::Path const preview = _fitter->preview();
    if (preview.empty()) {
        clear_preview();
        return;
    }

    if (!_preview_path) {
        _preview_path = CanvasItemPtr<CanvasItemBpath>(
            new CanvasItemBpath(_desktop->getCanvasTemp()));
        if (auto const color = sp_desktop_get_color_tool(_desktop, "/tools/livesketch", /*is_fill=*/false)) {
            _preview_path->set_stroke(color->toRGBA());
        } else {
            _preview_path->set_stroke(0x000000ff);
        }
        _preview_path->set_fill(0x0, SP_WIND_RULE_NONZERO);
    }
    // The fitter works in document pixels; the canvas wants desktop coords.
    _preview_path->set_bpath(Geom::PathVector{preview} * _desktop->doc2dt());
    _preview_path->set_visible(true);
}

void LiveSketchTool::cancel()
{
    reset();
    clear_preview();
    ungrabCanvasEvents();
    set_high_motion_precision(false);
    _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Sketch cancelled."));
}

void LiveSketchTool::commit()
{
    auto *doc = _desktop->getDocument();
    if (!doc) {
        reset();
        return;
    }

    Inkscape::LiveSketchOps::Options options;
    options.smoothing = smoothing_preference();
    if (auto *css = _desktop->getCurrentOrToolStyle("/tools/livesketch", false)) {
        Glib::ustring style;
        sp_repr_css_write_string(css, style);
        sp_repr_css_attr_unref(css);
        if (!style.empty()) {
            options.style = style;
        }
    }

    // LiveSketchOps::create records the single undo step itself.
    auto const result = Inkscape::LiveSketchOps::create(doc, _points_uu, options,
                                                        _desktop->layerManager().currentLayer());
    if (result.path) {
        _desktop->getSelection()->set(result.path);
    } else {
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("Could not create the sketch stroke."));
    }
    reset();
}

bool LiveSketchTool::root_handler(CanvasEvent const &event)
{
    bool ret = false;

    inspect_event(
        event,
        [&](ButtonPressEvent const &press) {
            if (press.button != 1 || press.num_press != 1) {
                return;
            }
            ret = true;

            reset();
            // The fitter runs in document pixels; the tolerance is VISUAL
            // millimetres and does not scale with the zoom.
            _fitter.emplace(LiveSketch::smoothing_to_tolerance_mm(smoothing_preference()) *
                            LiveSketch::kPxPerMm);
            capture(press.pos);
            saveDragOrigin(press.pos);
            grabCanvasEvents(EventType::KEY_PRESS | EventType::BUTTON_PRESS |
                             EventType::BUTTON_RELEASE | EventType::MOTION);
            set_high_motion_precision();
        },
        [&](MotionEvent const &motion) {
            if (!(motion.modifiers & GDK_BUTTON1_MASK) || !_fitter) {
                return;
            }
            if (!_dragging && !checkDragMoved(motion.pos)) {
                return; // still within the click tolerance
            }
            _dragging = true;
            for (auto const &hist : motion.history) {
                capture(hist.pos);
            }
            capture(motion.pos);
            update_preview();
            ret = true;
        },
        [&](ButtonReleaseEvent const &release) {
            if (release.button != 1) {
                return;
            }
            ret = true;

            bool const pressed = _fitter.has_value(); // false after Esc or a double-click press
            bool const dragged = _dragging;
            bool const enough = _points_uu.size() >= 2;
            if (!pressed) {
                reset();
            } else if (dragged && enough) {
                commit(); // also resets
            } else {
                reset();
                // A plain click (or an unusable drag) creates nothing.
                _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Drag to sketch a stroke."));
            }
            clear_preview();
            ungrabCanvasEvents();
            set_high_motion_precision(false);
        },
        [&](KeyPressEvent const &key) {
            if (key.keyval == GDK_KEY_Escape && _dragging) {
                cancel();
                ret = true;
            }
        },
        [](CanvasEvent const &) {}
    );

    return ret || ToolBase::root_handler(event);
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
