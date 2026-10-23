// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Smart Fill creation, shared by the app.smart-fill action and the tool.
 *
 * The tool only picks the point; objects are created through
 * SmartFillOps::create, the same entry point as app.smart-fill.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INK_ACTIONS_SMART_FILL_H
#define INK_ACTIONS_SMART_FILL_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <2geom/pathvector.h>
#include <2geom/point.h>

#include "path/smart-fill-geometry.h"

class SPDocument;
class SPItem;
class SPObject;
class SPPath;
class InkscapeApplication;

namespace Inkscape {
namespace SmartFillOps {

/// Everything a Smart Fill call can be told.
struct Options {
    std::string fill = "source";            ///< #rrggbb | none | source
    std::string stroke = "none";            ///< #rrggbb | none | source
    std::optional<double> stroke_width_mm;  ///< visual width in millimetres
    std::optional<std::string> id;          ///< id of the new path
    std::optional<std::string> parent;      ///< host group id
    std::string fallback_fill = "#000000";  ///< used when no flat source colour
    std::string fallback_stroke = "#000000";
};

/**
 * Parse "key=value[,key=value]*" into @a options.
 *
 * Keys: fill, stroke, stroke-width (mm, (0, 100]), id, parent. On error
 * returns nullopt and sets @a error to the bare reason (no "smart-fill: ").
 */
std::optional<Options> parse_options(std::string_view spec, std::string &error);

/// One visible object boundary: geometry in document user units, topmost first.
struct Candidate {
    SPItem *item = nullptr;
    Geom::PathVector path_uu;
    SmartFill::Rule rule = SmartFill::Rule::NonZero;
};

/// Visible, unlocked boundary candidates of @a doc, topmost first.
std::vector<Candidate> collect_candidates(SPDocument *doc);

struct CreateResult {
    SPPath *path = nullptr;
    SmartFill::Status status = SmartFill::Status::NoRegion;
    std::string error; ///< full "smart-fill: <reason>" text, empty on success
};

/**
 * Create one path covering the face at @a point_uu (document user units).
 *
 * @a parent_or_null overrides Options::parent. Invalid parent or id fails
 * before any document change; geometry failures leave the document and the
 * undo stack untouched. On success the path is appended last to its parent,
 * labelled "Smart Fill", in one undo step.
 */
CreateResult create(SPDocument *doc, Geom::Point const &point_uu, Options const &options,
                    SPObject *parent_or_null);

} // namespace SmartFillOps
} // namespace Inkscape

void add_actions_smart_fill(InkscapeApplication *app);

#endif // INK_ACTIONS_SMART_FILL_H

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
