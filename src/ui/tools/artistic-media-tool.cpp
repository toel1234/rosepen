// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Artistic Media tool: interactive creation of live Rosepen brush strokes.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "artistic-media-tool.h"

#include <cmath>
#include <optional>
#include <string>

#include <gdk/gdk.h>
#include <glibmm/i18n.h>

#include <2geom/pathvector.h>

#include "actions/actions-artistic-media.h"
#include "colors/color.h"
#include "desktop-style.h"
#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "document.h"
#include "layer-manager.h"
#include "live_effects/artistic-media-geometry.h"
#include "live_effects/artistic-media-library.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "ui/tools/desktop-coordinates.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::LivePathEffect::ArtisticMedia::Mode;

namespace Inkscape::UI::Tools {

namespace {

constexpr double NEW_POINT_DISTANCE_PX = 2.0; ///< minimum move that adds a point

char const *mode_pref = "/tools/artistic/mode";
char const *preset_pref = "/tools/artistic/preset";
char const *brush_pref = "/tools/artistic/brush";
char const *width_pref = "/tools/artistic/width";
char const *angle_pref = "/tools/artistic/angle";
char const *smoothing_pref = "/tools/artistic/smoothing";
char const *scale_pref = "/tools/artistic/scale_with_object";

Mode mode_from_prefs()
{
    auto const key = Preferences::get()->getString(mode_pref, "preset");
    if (auto const mode = Inkscape::LivePathEffect::ArtisticMedia::parse_mode(key.raw())) {
        return *mode;
    }
    return Mode::Preset;
}

/// The library shape name the tool currently draws with (the mode's default
/// when the stored name has disappeared from the library).
std::string shape_from_prefs(Mode mode)
{
    if (mode == Mode::Calligraphic) {
        return {};
    }
    auto const prefs = Preferences::get();
    char const *pref = mode == Mode::Brush ? brush_pref : preset_pref;
    auto const name = prefs->getString(pref, mode == Mode::Brush ? "dry-brush" : "lens");
    if (Inkscape::LivePathEffect::ArtisticMedia::find(mode, name.raw())) {
        return name.raw();
    }
    char const *fallback = Inkscape::LivePathEffect::ArtisticMedia::default_shape(mode);
    return fallback ? fallback : std::string();
}

} // namespace

ArtisticMediaTool::ArtisticMediaTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/artistic", "calligraphy.svg")
{
}

ArtisticMediaTool::~ArtisticMediaTool()
{
    ungrabCanvasEvents();
    clear_preview();
}

void ArtisticMediaTool::capture_point(Geom::Point const &position_doc)
{
    if (!_points.empty()) {
        auto const doc2dt = _desktop->doc2dt();
        if (Geom::distance(position_doc * doc2dt, _points.back() * doc2dt) < NEW_POINT_DISTANCE_PX) {
            return;
        }
    }
    _points.push_back(position_doc);
}

void ArtisticMediaTool::clear_preview()
{
    if (_preview_path) {
        _preview_path->set_visible(false);
    }
}

void ArtisticMediaTool::update_preview()
{
    // The preview uses the raw polyline, exactly what would be committed with
    // smoothing 0, recomputed at most once per motion event.
    if (_points.size() < 2) {
        clear_preview();
        return;
    }

    auto const prefs = Preferences::get();
    Mode const mode = mode_from_prefs();
    double const width_mm = prefs->getDouble(width_pref, 10.0);
    double const angle = prefs->getDouble(angle_pref, 30.0);

    // The points are document coordinates (px, the viewBox scale already
    // applied), so the preview width is plain millimetres to px.
    double const width_doc = width_mm * Inkscape::LivePathEffect::ArtisticMedia::kPxPerMm;

    Geom::PathVector skeleton;
    skeleton.push_back(Geom::Path());
    skeleton.back().start(_points.front());
    for (std::size_t i = 1; i < _points.size(); ++i) {
        skeleton.back().appendNew<Geom::LineSegment>(_points[i]);
    }

    Inkscape::LivePathEffect::ArtisticMedia::Result result;
    if (mode == Mode::Calligraphic) {
        result = Inkscape::LivePathEffect::ArtisticMedia::calligraphic(skeleton, width_doc, angle);
    } else {
        auto const unit_shape = Inkscape::LivePathEffect::ArtisticMedia::parse_unit_shape(shape_from_prefs(mode));
        if (!unit_shape) {
            clear_preview();
            return;
        }
        result = Inkscape::LivePathEffect::ArtisticMedia::map_along(skeleton, *unit_shape, width_doc);
    }

    if (!result.valid || result.path.empty()) {
        clear_preview();
        return;
    }

    if (!_preview_path) {
        _preview_path = CanvasItemPtr<CanvasItemBpath>(
            new CanvasItemBpath(_desktop->getCanvasTemp()));
    }
    if (auto const color = sp_desktop_get_color_tool(_desktop, "/tools/artistic", true)) {
        _preview_path->set_fill(color->toRGBA(), SP_WIND_RULE_NONZERO);
    } else {
        _preview_path->set_fill(0x000000ff, SP_WIND_RULE_NONZERO);
    }
    _preview_path->set_bpath(std::move(result.path) * _desktop->doc2dt());
    _preview_path->set_visible(true);
}

void ArtisticMediaTool::cancel()
{
    _points.clear();
    _dragging = false;
    clear_preview();
    ungrabCanvasEvents();
    _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Artistic media stroke cancelled."));
}

void ArtisticMediaTool::commit()
{
    if (_points.size() < 2) {
        cancel();
        return;
    }

    auto const prefs = Preferences::get();
    Mode const mode = mode_from_prefs();

    Inkscape::ArtisticOps::Options options;
    options.mode = mode;
    if (auto const shape = shape_from_prefs(mode); !shape.empty()) {
        options.shape = shape;
    }
    options.width = prefs->getDouble(width_pref, 10.0);
    options.angle = prefs->getDouble(angle_pref, 30.0);
    options.smoothing = prefs->getDouble(smoothing_pref, 50.0);
    options.scale_with_object = prefs->getBool(scale_pref, true);
    if (auto const color = sp_desktop_get_color_tool(_desktop, "/tools/artistic", true)) {
        options.color = color->toString(false);
    }

    auto *doc = _desktop->getDocument();
    auto *layer = _desktop->layerManager().currentLayer();
    // ArtisticOps::create takes document USER units (it applies the root's
    // viewBox transform itself); the captured points are document coordinates.
    Geom::Affine const doc2uu = doc->getRoot()->c2p.inverse();
    std::vector<Geom::Point> points_uu;
    points_uu.reserve(_points.size());
    for (auto const &p : _points) {
        points_uu.push_back(p * doc2uu);
    }
    std::string error;
    // ArtisticOps::create records the single undo step itself.
    if (!Inkscape::ArtisticOps::create(doc, points_uu, options, layer, error)) {
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("Could not create the artistic media stroke"));
    }
    _points.clear();
    _dragging = false;
    clear_preview();
}

void ArtisticMediaTool::click_select(Geom::Point const &window_pos)
{
    auto *item = _desktop->getItemAtPoint(window_pos, true);
    auto *selection = _desktop->getSelection();
    if (item) {
        selection->set(item);
    } else {
        selection->clear();
    }
}

bool ArtisticMediaTool::root_handler(CanvasEvent const &event)
{
    bool ret = false;

    inspect_event(
        event,
        [&](ButtonPressEvent const &press) {
            if (press.button != 1 || press.num_press != 1) {
                return;
            }
            ret = true;

            _points.clear();
            _dragging = false;
            capture_point(doc_point_from_window(*_desktop, press.pos));
            saveDragOrigin(press.pos);
            grabCanvasEvents(EventType::KEY_PRESS | EventType::BUTTON_PRESS |
                             EventType::BUTTON_RELEASE | EventType::MOTION);
        },
        [&](MotionEvent const &motion) {
            if (!(motion.modifiers & GDK_BUTTON1_MASK) || _points.empty()) {
                return;
            }
            if (!_dragging && !checkDragMoved(motion.pos)) {
                return; // still within the click tolerance
            }
            _dragging = true;
            capture_point(doc_point_from_window(*_desktop, motion.pos));
            update_preview();
            gobble_motion_events(GDK_BUTTON1_MASK);
            ret = true;
        },
        [&](ButtonReleaseEvent const &release) {
            if (release.button != 1) {
                return;
            }
            if (_dragging) {
                commit();
            } else {
                // A click without drag selects instead of drawing, so the
                // property bar can edit the item under the cursor.
                cancel();
                click_select(release.pos);
            }
            ret = true;
        },
        [&](KeyPressEvent const &key) {
            if (key.keyval == GDK_KEY_Escape) {
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
