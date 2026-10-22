// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dimension tool: interactive creation of live Rosepen dimension objects.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "dimension-tool.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include <glib.h>

#include <2geom/path.h>
#include <2geom/rect.h>

#include "actions/actions-dimension.h"
#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-text.h"
#include "document-undo.h"
#include "document.h"
#include "layer-manager.h"
#include "live_effects/dimension-geometry.h"
#include "message-stack.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "preferences.h"
#include "snap.h"
#include "style-enums.h"
#include "ui/widget/events/canvas-event.h"
#include "util/units.h"
#include "xml/repr.h"

namespace Inkscape::UI::Tools {

namespace {

using Inkscape::LivePathEffect::Dimension::Kind;
using Inkscape::LivePathEffect::Dimension::TextPosition;

constexpr unsigned ANCHOR_TOLERANCE_PX = 6;
constexpr double MIN_OFFSET_MM = 0.5;
constexpr double ANGLE_STEP = M_PI / 12.0; // 15 degrees

std::string num(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(buffer, sizeof buffer, value);
    return buffer;
}

char const *bbox_handle_name(int index)
{
    static char const *const names[] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};
    return names[index];
}

Geom::Point snap_to_angle_step(Geom::Point const &from, Geom::Point const &to)
{
    auto const delta = to - from;
    double const length = delta.length();
    if (length < 1e-9) {
        return to;
    }
    double const angle = std::round(std::atan2(delta[Geom::Y], delta[Geom::X]) / ANGLE_STEP) * ANGLE_STEP;
    return from + Geom::Point(std::cos(angle), std::sin(angle)) * length;
}

std::vector<std::pair<std::string, std::string>> creation_options()
{
    auto const prefs = Preferences::get();
    return {
        {"offset", num(prefs->getDouble("/tools/dimension/offset", 5.0))},
        {"precision", std::to_string(prefs->getInt("/tools/dimension/precision", 2))},
        {"unit", prefs->getString("/tools/dimension/unit", "mm").raw()},
        {"text", prefs->getString("/tools/dimension/text", "above").raw()},
        {"horizontal", prefs->getBool("/tools/dimension/horizontal", false) ? "true" : "false"},
        {"units", prefs->getBool("/tools/dimension/units", true) ? "true" : "false"},
        {"scale", "1"},
    };
}

} // namespace

DimensionTool::DimensionTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/dimension", "measure.svg")
{
    _mode = mode_from_prefs();
    _offset_mm = Preferences::get()->getDouble("/tools/dimension/offset", 5.0);
}

DimensionTool::~DimensionTool()
{
    ungrabCanvasEvents();
    clear_preview();
}

DimensionTool::Mode DimensionTool::mode_from_prefs() const
{
    auto const mode = Preferences::get()->getString("/tools/dimension/mode", "auto");
    if (mode == "parallel") {
        return Mode::Parallel;
    }
    if (mode == "angular") {
        return Mode::Angular;
    }
    if (mode == "segment") {
        return Mode::Segment;
    }
    return Mode::Auto;
}

double DimensionTool::to_mm(double px)
{
    return Inkscape::Util::Quantity::convert(px, "px", "mm");
}

std::optional<Geom::Point> DimensionTool::snap(Geom::Point const &window_pos, unsigned modifiers) const
{
    Geom::Point point = _desktop->w2d(window_pos);
    if (modifiers & GDK_CONTROL_MASK) {
        return point;
    }
    auto &snap_manager = _desktop->getNamedView()->snap_manager;
    snap_manager.setup(_desktop);
    snap_manager.freeSnapReturnByRef(point, SNAPSOURCE_OTHER_HANDLE);
    snap_manager.unSetup();
    return point;
}

DimensionTool::Captured DimensionTool::capture(Geom::Point const &window_pos,
                                               Geom::Point const &snapped_desktop) const
{
    Captured captured;
    captured.doc = _desktop->dt2doc(snapped_desktop);

    auto const root = _desktop->getDocument()->getRoot();
    captured.uu = captured.doc * root->c2p.inverse();

    auto const item = _desktop->getItemAtPoint(window_pos, true);
    if (!item || !item->getId()) {
        return captured;
    }

    auto const doc2dt = _desktop->doc2dt();
    auto const tolerance = static_cast<Geom::Coord>(ANCHOR_TOLERANCE_PX);

    if (auto const shape = cast<SPShape>(item)) {
        if (auto const curve = shape->curve()) {
            auto const nodes = Inkscape::LivePathEffect::Dimension::curve_nodes(
                *curve * item->i2doc_affine());
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (Geom::distance(nodes[i] * doc2dt, snapped_desktop) <= tolerance) {
                    captured.id = item->getId();
                    captured.anchor = "node:" + std::to_string(i);
                    return captured;
                }
            }
        }
    }

    // Document coordinates, like the effect resolves bbox:/rel: anchors.
    if (auto const bbox = item->documentGeometricBounds()) {
        auto const min = bbox->min();
        auto const size = bbox->dimensions();
        Geom::Point const handles[9] = {
            {min[Geom::X], min[Geom::Y]},
            {min[Geom::X] + size[Geom::X] / 2, min[Geom::Y]},
            {min[Geom::X] + size[Geom::X], min[Geom::Y]},
            {min[Geom::X], min[Geom::Y] + size[Geom::Y] / 2},
            {min[Geom::X] + size[Geom::X] / 2, min[Geom::Y] + size[Geom::Y] / 2},
            {min[Geom::X] + size[Geom::X], min[Geom::Y] + size[Geom::Y] / 2},
            {min[Geom::X], min[Geom::Y] + size[Geom::Y]},
            {min[Geom::X] + size[Geom::X] / 2, min[Geom::Y] + size[Geom::Y]},
            {min[Geom::X] + size[Geom::X], min[Geom::Y] + size[Geom::Y]}
        };
        for (int i = 0; i < 9; ++i) {
            if (Geom::distance(handles[i] * doc2dt, snapped_desktop) <= tolerance) {
                captured.id = item->getId();
                captured.anchor = std::string("bbox:") + bbox_handle_name(i);
                return captured;
            }
        }

        if (bbox->contains(captured.doc) && size[Geom::X] > 0 && size[Geom::Y] > 0) {
            double const u = std::clamp((captured.doc[Geom::X] - min[Geom::X]) / size[Geom::X], 0.0, 1.0);
            double const v = std::clamp((captured.doc[Geom::Y] - min[Geom::Y]) / size[Geom::Y], 0.0, 1.0);
            captured.id = item->getId();
            captured.anchor = "rel:" + num(u) + "/" + num(v);
        }
    }

    return captured;
}

void DimensionTool::reset_state()
{
    _state = State::Idle;
    _dragging = false;
    _p1 = {};
    _p2 = {};
    _vertex = {};
    _arm_a = {};
    _arm_b = {};
    clear_preview();
    ungrabCanvasEvents();
}

void DimensionTool::cancel()
{
    reset_state();
    _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Dimension cancelled."));
}

void DimensionTool::clear_preview()
{
    if (_preview_path) {
        _preview_path->set_visible(false);
    }
    if (_preview_text) {
        _preview_text->set_visible(false);
    }
}

double DimensionTool::offset_mm_from_cursor(Mode mode, Geom::Point const &cursor_doc) const
{
    double const fallback = Preferences::get()->getDouble("/tools/dimension/offset", 5.0);

    if (mode == Mode::Angular) {
        return std::max(to_mm(Geom::distance(cursor_doc, _vertex.doc)), MIN_OFFSET_MM);
    }

    auto const &p1 = _p1.doc;
    auto const &p2 = _p2.doc;
    Geom::Point const direction = p2 - p1;
    double const length = direction.length();
    if (length < 1e-9) {
        return std::max(fallback, MIN_OFFSET_MM);
    }

    // The sign must follow the effect's convention (dimension-geometry.cpp,
    // document coordinates are y-down): horizontal + = above the higher
    // point, vertical + = right of the rightmost point, aligned + = left of
    // P1 -> P2, i.e. along (u.y, -u.x).
    Kind const kind = mode == Mode::Auto ? Inkscape::LivePathEffect::Dimension::resolve_auto(p1, p2) : Kind::Aligned;
    auto const signed_mm = [](bool positive, double distance_px) {
        double const mm = std::max(to_mm(distance_px), MIN_OFFSET_MM);
        return positive ? mm : -mm;
    };
    if (kind == Kind::Horizontal) {
        double const top = std::min(p1[Geom::Y], p2[Geom::Y]);
        double const bottom = std::max(p1[Geom::Y], p2[Geom::Y]);
        bool const above = cursor_doc[Geom::Y] <= (top + bottom) / 2.0;
        return signed_mm(above, above ? top - cursor_doc[Geom::Y] : cursor_doc[Geom::Y] - bottom);
    }
    if (kind == Kind::Vertical) {
        double const left = std::min(p1[Geom::X], p2[Geom::X]);
        double const right = std::max(p1[Geom::X], p2[Geom::X]);
        bool const on_right = cursor_doc[Geom::X] >= (left + right) / 2.0;
        return signed_mm(on_right, on_right ? cursor_doc[Geom::X] - right : left - cursor_doc[Geom::X]);
    }

    double const cross = direction[Geom::X] * (cursor_doc[Geom::Y] - p1[Geom::Y]) -
                         direction[Geom::Y] * (cursor_doc[Geom::X] - p1[Geom::X]);
    double const along_left = -cross / length; // (c - p1) . (u.y, -u.x)
    return signed_mm(along_left >= 0.0, std::abs(along_left));
}

void DimensionTool::update_preview(Geom::Point const &window_pos, unsigned modifiers)
{
    using namespace Inkscape::LivePathEffect::Dimension;

    auto const snapped = snap(window_pos, modifiers);
    if (!snapped) {
        return;
    }
    auto const cursor_doc = _desktop->dt2doc(*snapped);

    std::vector<Geom::Point> points;
    Kind kind = Kind::Aligned;

    if (_mode == Mode::Angular) {
        if (_state != State::ArmA && _state != State::ArmB) {
            return;
        }
        points.push_back(_arm_a.doc);
        points.push_back(_vertex.doc);
        points.push_back(_state == State::ArmB ? _arm_b.doc : cursor_doc);
        kind = Kind::Angular;
        _offset_mm = offset_mm_from_cursor(Mode::Angular, cursor_doc);
    } else {
        if (_state == State::Idle) {
            return;
        }
        points.push_back(_p1.doc);
        points.push_back(_state == State::Second ? _p2.doc : cursor_doc);
        if (_mode == Mode::Parallel && (modifiers & GDK_SHIFT_MASK) && _state != State::Second) {
            points.back() = snap_to_angle_step(points[0], points.back());
        }
        if (_mode == Mode::Auto) {
            kind = resolve_auto(points[0], points[1]);
        }
        // While the second point is still being dragged the offset is the
        // tool default; it follows the cursor once both points are set.
        _offset_mm = _state == State::Second ? offset_mm_from_cursor(_mode, cursor_doc)
                                             : Preferences::get()->getDouble("/tools/dimension/offset", 5.0);
    }

    auto const doc = _desktop->getDocument();

    Style style;
    style.offset = Inkscape::Util::Quantity::convert(_offset_mm, "mm", "px");
    style.arrow_length = Inkscape::Util::Quantity::convert(2.5, "mm", "px");
    style.ext_gap = Inkscape::Util::Quantity::convert(1.0, "mm", "px");
    style.ext_overshoot = Inkscape::Util::Quantity::convert(2.0, "mm", "px");
    style.text_position = TextPosition::Above;

    auto const result = build(kind, points, style, true);
    if (!result.valid) {
        clear_preview();
        return;
    }

    auto const doc2dt = _desktop->doc2dt();
    if (!_preview_path) {
        _preview_path = CanvasItemPtr<CanvasItemBpath>(
            new CanvasItemBpath(_desktop->getCanvasTemp()));
        _preview_path->set_stroke(0x000000ff);
        _preview_path->set_fill(0x0, SP_WIND_RULE_EVENODD);
    }
    _preview_path->set_bpath(result.path * doc2dt);
    _preview_path->set_visible(true);

    auto const prefs = Preferences::get();
    auto const unit = prefs->getString("/tools/dimension/unit", "mm");
    auto const precision = prefs->getInt("/tools/dimension/precision", 2);
    Glib::ustring value;
    if (kind == Kind::Angular) {
        value = format_label(result.value_uu_or_deg, precision, "\xc2\xb0", true, "", "", true);
    } else {
        double const display = Inkscape::Util::Quantity::convert(result.value_uu_or_deg, "px", unit);
        value = format_label(display, precision, unit, prefs->getBool("/tools/dimension/units", true),
                             "", "", false);
    }

    if (!_preview_text) {
        _preview_text = CanvasItemPtr<CanvasItemText>(
            new CanvasItemText(_desktop->getCanvasTemp(), result.label_pos * doc2dt, value));
        _preview_text->set_fontsize(Inkscape::Util::Quantity::convert(10, "pt", "px"));
    }
    _preview_text->set_text(value);
    _preview_text->set_coord(result.label_pos * doc2dt);
    _preview_text->set_visible(true);
}

std::optional<std::pair<int, int>> DimensionTool::segment_nodes(SPItem *item, Geom::Point const &window_pos) const
{
    auto const shape = cast<SPShape>(item);
    if (!shape) {
        return std::nullopt;
    }
    auto const pv = shape->curve();
    if (!pv || pv->empty()) {
        return std::nullopt;
    }

    Geom::Point const d1 = _desktop->w2d(window_pos);
    Geom::Point const d2 = _desktop->w2d(window_pos + Geom::Point(ANCHOR_TOLERANCE_PX, 0));
    Geom::Point const i1 = _desktop->dt2doc(d1) * item->i2doc_affine().inverse();
    Geom::Point const i2 = _desktop->dt2doc(d2) * item->i2doc_affine().inverse();
    double const tolerance = std::max(Geom::distance(i1, i2), 1e-9);

    double best = std::numeric_limits<double>::infinity();
    std::pair<int, int> found{-1, -1};
    int node_offset = 0;

    for (auto const &path : *pv) {
        int const count = static_cast<int>(path.size());
        for (int j = 0; j < count; ++j) {
            auto const &curve = path[j];
            auto const time = curve.nearestTime(i1);
            double const distance = Geom::distance(curve.pointAt(time), i1);
            if (distance < best) {
                best = distance;
                int const start = node_offset + j;
                int const end = (j + 1 < count) ? start + 1
                                                : (path.closed() ? node_offset : start + 1);
                found = {start, end};
            }
        }
        node_offset += path.closed() ? count : count + 1;
    }

    if (found.first < 0 || best > tolerance) {
        return std::nullopt;
    }
    return found;
}

bool DimensionTool::commit_segment(Geom::Point const &window_pos)
{
    auto const item = _desktop->getItemAtPoint(window_pos, true);
    if (!item || !item->getId()) {
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("Click on a path segment to add a dimension."));
        return false;
    }

    auto const nodes = segment_nodes(item, window_pos);
    if (!nodes) {
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("No path segment under the cursor."));
        return false;
    }

    Inkscape::DimensionOps::Spec spec;
    spec.kind = "segment";
    spec.refs.push_back({item->getId(), "node:" + std::to_string(nodes->first), {}});
    spec.refs.push_back({item->getId(), "node:" + std::to_string(nodes->second), {}});
    spec.options = creation_options();

    auto const doc = _desktop->getDocument();
    auto const layer = _desktop->layerManager().currentLayer();
    // DimensionOps::create records the single undo step itself.
    if (!Inkscape::DimensionOps::create(doc, spec, layer)) {
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("Could not create the dimension"));
        return false;
    }
    return true;
}

void DimensionTool::commit(Mode mode)
{
    Inkscape::DimensionOps::Spec spec;
    std::vector<std::pair<std::string, std::string>> options = creation_options();
    for (auto &[key, value] : options) {
        if (key == "offset") {
            value = num(_offset_mm);
        }
    }
    spec.options = std::move(options);

    switch (mode) {
        case Mode::Auto:
            spec.kind = "auto";
            spec.refs.push_back({_p1.id, _p1.anchor, _p1.uu});
            spec.refs.push_back({_p2.id, _p2.anchor, _p2.uu});
            break;
        case Mode::Parallel:
            spec.kind = "aligned";
            spec.refs.push_back({_p1.id, _p1.anchor, _p1.uu});
            spec.refs.push_back({_p2.id, _p2.anchor, _p2.uu});
            break;
        case Mode::Angular:
            spec.kind = "angular";
            spec.refs.push_back({_arm_a.id, _arm_a.anchor, _arm_a.uu});
            spec.refs.push_back({_vertex.id, _vertex.anchor, _vertex.uu});
            spec.refs.push_back({_arm_b.id, _arm_b.anchor, _arm_b.uu});
            break;
        case Mode::Segment:
            return;
    }

    auto const doc = _desktop->getDocument();
    auto const layer = _desktop->layerManager().currentLayer();
    if (!Inkscape::DimensionOps::create(doc, spec, layer)) {
        _desktop->messageStack()->flash(ERROR_MESSAGE, _("Could not create the dimension"));
        reset_state();
        return;
    }

    // DimensionOps::create recorded the single undo step.
    reset_state();
}

bool DimensionTool::root_handler(CanvasEvent const &event)
{
    bool ret = false;

    inspect_event(
        event,
        [&](ButtonPressEvent const &press) {
            if (press.button != 1 || press.num_press != 1) {
                return;
            }
            ret = true;

            auto const snapped = snap(press.pos, press.modifiers);
            if (!snapped) {
                return;
            }

            if (_mode == Mode::Segment) {
                commit_segment(press.pos);
                return;
            }

            if (_mode == Mode::Angular) {
                auto const point = capture(press.pos, *snapped);
                switch (_state) {
                    case State::Idle:
                        _vertex = point;
                        _state = State::Vertex;
                        break;
                    case State::Vertex:
                        _arm_a = point;
                        _state = State::ArmA;
                        break;
                    case State::ArmA:
                        _arm_b = point;
                        _state = State::ArmB;
                        _offset_mm = offset_mm_from_cursor(Mode::Angular, point.doc);
                        break;
                    case State::ArmB:
                        commit(Mode::Angular);
                        break;
                    default:
                        break;
                }
                grabCanvasEvents(EventType::KEY_PRESS | EventType::BUTTON_PRESS |
                                 EventType::BUTTON_RELEASE | EventType::MOTION);
                return;
            }

            auto const point = [&] {
                auto snapped_point = *snapped;
                if (_state == State::First && _mode == Mode::Parallel &&
                    (press.modifiers & GDK_SHIFT_MASK)) {
                    snapped_point = snap_to_angle_step(_p1.doc * _desktop->doc2dt(), snapped_point);
                }
                return capture(press.pos, snapped_point);
            }();

            if (_state == State::Idle) {
                _p1 = point;
                _state = State::First;
                _dragging = false;
                saveDragOrigin(press.pos);
                grabCanvasEvents(EventType::KEY_PRESS | EventType::BUTTON_PRESS |
                                 EventType::BUTTON_RELEASE | EventType::MOTION);
            } else if (_state == State::First) {
                _p2 = point;
                _state = State::Second;
                _offset_mm = offset_mm_from_cursor(_mode, point.doc);
            } else if (_state == State::Second) {
                commit(_mode);
            }
        },
        [&](MotionEvent const &motion) {
            if (_state == State::Idle) {
                return;
            }
            if (motion.modifiers & GDK_BUTTON1_MASK) {
                if (_state == State::First) {
                    if (checkDragMoved(motion.pos)) {
                        _dragging = true;
                        update_preview(motion.pos, motion.modifiers);
                        gobble_motion_events(GDK_BUTTON1_MASK);
                        ret = true;
                    }
                }
                return;
            }
            update_preview(motion.pos, motion.modifiers);
            ret = true;
        },
        [&](ButtonReleaseEvent const &release) {
            if (release.button != 1) {
                return;
            }
            if (_state == State::First && _dragging) {
                if (auto const snapped = snap(release.pos, release.modifiers)) {
                    auto snapped_point = *snapped;
                    if (_mode == Mode::Parallel && (release.modifiers & GDK_SHIFT_MASK)) {
                        snapped_point = snap_to_angle_step(_p1.doc * _desktop->doc2dt(), snapped_point);
                    }
                    _p2 = capture(release.pos, snapped_point);
                    _state = State::Second;
                    _offset_mm = offset_mm_from_cursor(_mode, _p2.doc);
                    update_preview(release.pos, release.modifiers);
                }
                _dragging = false;
                ret = true;
            }
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
