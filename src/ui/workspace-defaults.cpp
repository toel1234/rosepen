// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WorkspaceDefaults - pure workspace model for the Corel-style workspace.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/workspace-defaults.h"

#include <cstring>

#include "preferences.h"

namespace Inkscape::UI::Workspace {

namespace {

// Both presets cover the same paths in the same order so that either one
// resets every workspace choice, including a manual palette placement.
constexpr PrefValue rosepen_values[] = {
    {"/toolbox/layout", "corel"},
    {"/toolbox/propertybar/object-sections", "true"},
    {menubar_layout_pref, "corel"},
    {palette_placement_pref, "bottom"},
    {"/options/kbshortcuts/shortcutfile", rosepen_keys_file},
    {"/window/interface_mode", "false"},
    {"/theme/preferDarkTheme", "false"},
    {"/theme/darkTheme", "false"},
    {"/theme/symbolicIcons", "false"},
    {"/theme/iconTheme", "rosepen"},
    {"/options/notebooklabels/value", "1"},
    {"/toolbox/tools/iconsize", "24"},
    {"/toolbox/controlbars/iconsize", "20"},
    {document_tabs_pref, "true"},
};

constexpr PrefValue inkscape_values[] = {
    {"/toolbox/layout", "classic"},
    {"/toolbox/propertybar/object-sections", "false"},
    {menubar_layout_pref, "classic"},
    {palette_placement_pref, "bottom"},
    {"/options/kbshortcuts/shortcutfile", "inkscape.xml"},
    {"/window/interface_mode", nullptr},
    {"/theme/preferDarkTheme", "true"},
    {"/theme/darkTheme", "true"},
    {"/theme/symbolicIcons", "true"},
    {"/theme/iconTheme", "multicolor"},
    {"/options/notebooklabels/value", "0"},
    {"/toolbox/tools/iconsize", "16"},
    {"/toolbox/controlbars/iconsize", "16"},
    {document_tabs_pref, "false"},
};

bool is_bool_literal(char const *value)
{
    return std::strcmp(value, "true") == 0 || std::strcmp(value, "false") == 0;
}

} // namespace

std::string commandbar_ui_file(std::string_view toolbox_layout)
{
    return toolbox_layout == "classic" ? "toolbar-commands.ui" : "toolbar-commands-corel.ui";
}

std::string menubar_ui_file(std::string_view layout_value)
{
    return layout_value == "classic" ? "menus.ui" : "menus-corel.ui";
}

PalettePlacement palette_placement(std::string_view value)
{
    return value == "right" ? PalettePlacement::Right : PalettePlacement::Bottom;
}

char const *palette_lines_key(PalettePlacement placement)
{
    // The vertical strip keeps its own column count, so a saved horizontal
    // row count is never reinterpreted as a column count.
    return placement == PalettePlacement::Right ? "columns" : "rows";
}

int default_palette_lines(PalettePlacement)
{
    // One row (bottom) or one column (right): a slim single-line palette.
    return 1;
}

bool is_default_keys_file(std::string_view file)
{
    return file.empty() || file == "inkscape.xml" || file == "default.xml" || file == rosepen_keys_file;
}

std::span<PrefValue const> rosepen_workspace()
{
    return rosepen_values;
}

std::span<PrefValue const> inkscape_workspace()
{
    return inkscape_values;
}

void apply_workspace(std::span<PrefValue const> values, bool computed_default)
{
    auto *prefs = Preferences::get();
    for (auto const &entry : values) {
        if (!entry.value) {
            prefs->remove(entry.path);
            // The skeleton reloads under the user file on every start, so a
            // skeleton-pinned value cannot be cleared for good: write the
            // upstream default explicitly instead.
            if (prefs->getEntry(entry.path).isSet()) {
                prefs->setBool(entry.path, computed_default);
            }
        } else if (is_bool_literal(entry.value)) {
            prefs->setBool(entry.path, std::strcmp(entry.value, "true") == 0);
        } else {
            prefs->setString(entry.path, entry.value);
        }
    }
}

bool apply_workspace_revision()
{
    auto *prefs = Preferences::get();
    if (prefs->getInt(workspace_revision_pref, 0) >= workspace_revision) {
        return false;
    }
    apply_workspace(rosepen_workspace());
    prefs->setInt(workspace_revision_pref, workspace_revision);
    return true;
}

} // namespace Inkscape::UI::Workspace

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
