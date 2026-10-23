// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live Sketch creation, shared by the app.live-sketch action and the tool.
 *
 * The tool only captures points; the path is created through
 * LiveSketchOps::create, the same entry point as app.live-sketch, so the tool
 * and the headless corpus produce identical objects.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INK_ACTIONS_LIVE_SKETCH_H
#define INK_ACTIONS_LIVE_SKETCH_H

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <2geom/point.h>

#include "path/live-sketch-geometry.h"

class SPDocument;
class SPObject;
class SPPath;
class InkscapeApplication;

namespace Inkscape {
namespace LiveSketchOps {

/// Everything a Live Sketch call can be told.
struct Options {
    double smoothing = LiveSketch::kDefaultSmoothing; ///< 0..100
    std::string stroke = "#000000";                   ///< #rrggbb
    double stroke_width_mm = 0.25;                    ///< visual width in millimetres
    std::optional<std::string> id;                    ///< id of the new path
    std::optional<std::string> parent;                ///< host group id
    std::optional<std::string> style;                 ///< tool-only CSS, written verbatim
};

/**
 * Parse "<x1> <y1> <x2> <y2> ..." into points in document user units.
 *
 * Whitespace-separated classic-locale numbers; an empty list is valid (create
 * then rejects it). On error returns nullopt and sets @a error to the bare
 * reason (no "live-sketch: "), either "bad number '<tok>'" or
 * "odd number of coordinates".
 */
std::optional<std::vector<Geom::Point>> parse_points(std::string_view spec, std::string &error);

/**
 * Parse "key=value[,key=value]*" into @a options.
 *
 * Keys: smoothing (0..100), stroke (#rrggbb only; "none" is rejected because an
 * unstroked open sketch is invisible), stroke-width (mm, (0, 100]), id, parent.
 * On error returns nullopt and sets @a error to the bare reason (no
 * "live-sketch: ").
 */
std::optional<Options> parse_options(std::string_view spec, std::string &error);

struct CreateResult {
    SPPath *path = nullptr;
    std::string error; ///< full "live-sketch: <reason>" text, empty on success
};

/**
 * Create one open sketch path through @a points_uu (document user units).
 *
 * @a parent_or_null overrides Options::parent. Everything is validated before
 * the document changes: out-of-plane coordinates, a non-group or degenerate
 * parent, a used id and fewer than two distinct points all fail with the
 * document and the undo stack untouched. On success the path is appended last
 * to its parent, labelled "Live Sketch", carries a round-capped stroke, and is
 * created in one undo step.
 */
CreateResult create(SPDocument *doc, std::span<Geom::Point const> points_uu, Options const &options,
                    SPObject *parent_or_null);

} // namespace LiveSketchOps
} // namespace Inkscape

void add_actions_live_sketch(InkscapeApplication *app);

#endif // INK_ACTIONS_LIVE_SKETCH_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4
