// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PropertyBarContext implementation.
 *
 * The whole "tool + selection -> object section" rule set is the one table
 * below; it reuses the upstream per-tool Toolbar classes as sections.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "property-bar-context.h"

#include "object/sp-ellipse.h"
#include "object/sp-flowtext.h"
#include "object/sp-rect.h"
#include "object/sp-spiral.h"
#include "object/sp-star.h"
#include "object/sp-text.h"
#include "util/cast.h"

namespace Inkscape::UI::Toolbar {

ObjectKind classify_object(SPItem const *item)
{
    if (item == nullptr) {
        return ObjectKind::Other;
    }
    if (is<SPRect>(item)) {
        return ObjectKind::Rect;
    }
    if (is<SPGenericEllipse>(item)) { // circle, ellipse and arc
        return ObjectKind::Ellipse;
    }
    if (is<SPStar>(item)) { // star and flatsided polygon
        return ObjectKind::Star;
    }
    if (is<SPSpiral>(item)) {
        return ObjectKind::Spiral;
    }
    if (is<SPText>(item) || is<SPFlowtext>(item)) {
        return ObjectKind::Text;
    }
    return ObjectKind::Other; // path, group, use/clone, image, 3D box, ...
}

std::optional<ObjectKind> common_kind(std::vector<ObjectKind> const &kinds)
{
    if (kinds.empty() || kinds.front() == ObjectKind::Other) {
        return std::nullopt;
    }
    for (auto const kind : kinds) {
        if (kind != kinds.front()) {
            return std::nullopt;
        }
    }
    return kinds.front();
}

namespace {

struct SectionRow {
    std::string_view tool_path;
    ObjectKind kind;
    std::string_view section; // toolbar_data key in toolbars.cpp
};

// clang-format off
constexpr SectionRow section_table[] = {
    // tool_path        kind                 section
    {"/tools/select",   ObjectKind::Rect,    "/tools/shapes/rect"},
    {"/tools/select",   ObjectKind::Ellipse, "/tools/shapes/arc"},
    {"/tools/select",   ObjectKind::Star,    "/tools/shapes/star"},
    {"/tools/select",   ObjectKind::Spiral,  "/tools/shapes/spiral"},
    {"/tools/select",   ObjectKind::Text,    "/tools/text"},
    {"/tools/nodes",    ObjectKind::Rect,    "/tools/shapes/rect"},
    {"/tools/nodes",    ObjectKind::Ellipse, "/tools/shapes/arc"},
    {"/tools/nodes",    ObjectKind::Star,    "/tools/shapes/star"},
    {"/tools/nodes",    ObjectKind::Spiral,  "/tools/shapes/spiral"},
    // Node + Text: the Node tool already edits text kerning on-canvas and the
    // text section's tool-gated controls would be noise there.
    //
    // Tools that are selection-aware themselves (Rect, Arc, Star, Spiral,
    // Text, ...) get no rows: their own toolbar already shows these controls.
};
// clang-format on

} // namespace

std::string_view object_section_for(std::string_view tool_path,
                                    std::optional<ObjectKind> kind,
                                    bool enabled)
{
    if (!enabled || !kind || *kind == ObjectKind::Other) {
        return "";
    }
    for (auto const &row : section_table) {
        if (row.kind == *kind && row.tool_path == tool_path) {
            if (row.section == tool_path) { // never a kind's own toolbar
                return "";
            }
            return row.section;
        }
    }
    return "";
}

} // namespace Inkscape::UI::Toolbar
