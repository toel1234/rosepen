// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WorkspaceDefaults - pure workspace model for the Corel-style workspace.
 *
 * Every decision here (which menubar file to load, where the colour palette
 * lives and how many lines it shows, which key profile is a default, and the
 * Rosepen/Inkscape preset sets) is a plain function so it can be locked by a
 * headless gtest.  The header stays free of Gtk/glibmm; only the .cpp touches
 * Preferences, in apply_workspace().
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_UI_WORKSPACE_DEFAULTS_H
#define SEEN_UI_WORKSPACE_DEFAULTS_H

#include <span>
#include <string>
#include <string_view>

namespace Inkscape::UI::Workspace {

/// Preference holding the menubar layout ("corel" or "classic").
inline constexpr char const *menubar_layout_pref = "/options/menubar/layout";

/// Preference holding the colour palette placement ("bottom" or "right").
inline constexpr char const *palette_placement_pref = "/embedded/swatches/placement";

/// Show the document tab bar even with a single document open.
inline constexpr char const *document_tabs_pref = "/options/documenttabs/always";

/// The Rosepen key profile; a default, so no keys warning is shown for it.
inline constexpr char const *rosepen_keys_file = "rosepen.xml";

/// The commands bar for a /toolbox/layout value: the Corel standard bar
/// unless the classic (upstream) toolbox layout is chosen.
std::string commandbar_ui_file(std::string_view toolbox_layout);

/**
 * The GtkBuilder file for a menubar layout value.
 *
 * Exactly "classic" selects the upstream menus.ui; anything else, including an
 * empty or unknown value, selects the CorelDRAW-order menus-corel.ui.
 */
std::string menubar_ui_file(std::string_view layout_value);

// Convenience overloads so callers can pass Preferences::getString() results
// (Glib::ustring) or plain strings without this header depending on glibmm.
inline std::string menubar_ui_file(std::string const &layout_value)
{
    return menubar_ui_file(std::string_view(layout_value));
}

inline std::string menubar_ui_file(char const *layout_value)
{
    return menubar_ui_file(std::string_view(layout_value ? layout_value : ""));
}

/// Where the palette strip is packed in the drawing window.
enum class PalettePlacement
{
    Right,  ///< CorelDRAW's classic vertical strip at the right edge.
    Bottom, ///< The reference screenshot's horizontal row; upstream packing.
};

/// Right only for exactly "right"; everything else is Bottom.
PalettePlacement palette_placement(std::string_view value);

/// Convenience overloads for plain strings and Glib::ustring callers.
inline PalettePlacement palette_placement(std::string const &value)
{
    return palette_placement(std::string_view(value));
}

inline PalettePlacement palette_placement(char const *value)
{
    return palette_placement(std::string_view(value ? value : ""));
}

/// The preference key holding the palette line count for @a placement.
char const *palette_lines_key(PalettePlacement placement);

/// The default line count for @a placement (rows for Bottom, columns for Right).
int default_palette_lines(PalettePlacement placement);

/**
 * Whether @a file is one of the default key profiles, for which the welcome
 * screen shows no keys warning. An empty value means the shipped default.
 */
bool is_default_keys_file(std::string_view file);

/// Convenience overloads for plain strings and Glib::ustring callers.
inline bool is_default_keys_file(std::string const &file)
{
    return is_default_keys_file(std::string_view(file));
}

inline bool is_default_keys_file(char const *file)
{
    return is_default_keys_file(std::string_view(file ? file : ""));
}

/// One preference to set; a null value means "remove the preference".
struct PrefValue
{
    char const *path;
    char const *value;
};

/// The complete Rosepen (Corel-style) workspace preset.
std::span<PrefValue const> rosepen_workspace();

/// The complete Inkscape (upstream) workspace preset.
std::span<PrefValue const> inkscape_workspace();

/**
 * Apply a preset. "true"/"false" are written as booleans, everything else as a
 * string. A null value means "the upstream default": the preference is
 * removed, and if it is still set afterwards (the skeleton pins it, or
 * Preferences::remove() cannot delete an attribute on a group with children,
 * as for /window/interface_mode) it is written as @a computed_default instead.
 * The only null entry is /window/interface_mode, whose upstream default is the
 * screen-aspect widescreen default the caller computes.
 */
void apply_workspace(std::span<PrefValue const> values, bool computed_default = false);

/// Marker recording which Rosepen workspace revision a profile has received.
inline constexpr char const *workspace_revision_pref = "/options/workspace/rosepen-revision";

/// Bump when the Rosepen preset gains values that existing profiles must get.
inline constexpr int workspace_revision = 3;

/**
 * Apply the Rosepen workspace once per revision, so that existing profiles
 * (which store older values such as the dark theme) start with the Rosepen
 * look without a manual "Use Rosepen workspace" click. Later user changes,
 * including "Use Inkscape workspace", are kept. Returns true if it applied.
 */
bool apply_workspace_revision();

} // namespace Inkscape::UI::Workspace

#endif // SEEN_UI_WORKSPACE_DEFAULTS_H

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
