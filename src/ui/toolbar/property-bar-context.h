// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PropertyBarContext - which object section the property bar shows.
 *
 * Pure model: active tool + current selection -> the upstream toolbar_data
 * key of the object control set to host next to the tool's own toolbar.
 * No Gtk, no preferences access, no widget code.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_UI_TOOLBAR_PROPERTY_BAR_CONTEXT_H
#define SEEN_UI_TOOLBAR_PROPERTY_BAR_CONTEXT_H

#include <optional>
#include <string_view>
#include <vector>

class SPItem;

namespace Inkscape::UI::Toolbar {

/// The object type of one selected item, as far as the property bar cares.
enum class ObjectKind { Other, Rect, Ellipse, Star, Spiral, Text };

/// Classify @a item; nullptr and everything without a section give Other.
/// Does not recurse into groups.
ObjectKind classify_object(SPItem const *item);

/// The one kind shared by all entries, unless empty, mixed, or Other.
std::optional<ObjectKind> common_kind(std::vector<ObjectKind> const &kinds);

/**
 * The upstream toolbar_data key of the object section for @a tool_path with a
 * homogeneous @a kind selection, or "" when no section applies:
 * no common kind, Other, @a enabled == false, a tool without sections, or a
 * result equal to the tool's own toolbar key.
 */
std::string_view object_section_for(std::string_view tool_path,
                                    std::optional<ObjectKind> kind,
                                    bool enabled);

/// Live preference gating the object sections (bool, default true).
inline constexpr char const *object_sections_pref_path = "/toolbox/propertybar/object-sections";

} // namespace Inkscape::UI::Toolbar

#endif // SEEN_UI_TOOLBAR_PROPERTY_BAR_CONTEXT_H
