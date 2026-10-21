// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ToolboxGroups - Corel-style toolbox group model.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_UI_TOOLBAR_TOOLBOX_GROUPS_H
#define SEEN_UI_TOOLBAR_TOOLBOX_GROUPS_H

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glibmm/ustring.h>

namespace Inkscape::UI::Toolbar {

/**
 * One flyout group: a stable id plus the tool names it contains in display
 * order. The first tool is the default face until a member is picked.
 */
struct ToolGroup
{
    Glib::ustring id;
    std::vector<Glib::ustring> tools;
};

/// Preference holding the toolbox layout ("corel" or "classic").
inline constexpr char const *toolbox_layout_path = "/toolbox/layout";

/// True unless /toolbox/layout is "classic"; bad or missing values mean "corel".
bool use_flyout_layout();

/**
 * Parse toolbox-groups.ini data. Pure: no resource lookup, no preferences.
 *
 * Every violation appends one human-readable message to @a errors and drops
 * only the offending entry. Never throws.
 */
std::vector<ToolGroup> parse_toolbox_groups(std::string const &keyfile_data,
                                            std::vector<Glib::ustring> &errors);

/// The shipped groups, parsed and cached on first use. Empty on failure.
std::vector<ToolGroup> const &toolbox_groups();

/// Translated label for a group id; unknown ids are returned unchanged.
Glib::ustring group_label(Glib::ustring const &id);

/// Index of the group containing @a tool, if any.
std::optional<std::size_t> group_index_for_tool(std::vector<ToolGroup> const &groups,
                                                Glib::ustring const &tool);

/**
 * The tool shown on the group button.
 *
 * Returns the remembered /toolbox/flyouts/<id>/active member when it is a
 * visible member; otherwise the first visible member; otherwise "".
 * An empty @a visible predicate means every member is visible.
 */
Glib::ustring active_tool(ToolGroup const &group,
                          std::function<bool(Glib::ustring const &)> const &visible = {});

/// Remember @a tool as the group face; ignored when it is not a member.
void set_active_tool(ToolGroup const &group, Glib::ustring const &tool);

} // namespace Inkscape::UI::Toolbar

#endif // SEEN_UI_TOOLBAR_TOOLBOX_GROUPS_H
