// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Built-in artistic media library: the Rosepen-authored preset and brush shapes.
 *
 * A shape is a closed path in unit space u in [0, 1] (along the skeleton,
 * u = 0 at its start) and v in [-0.5, 0.5] (across it). The mapping onto a
 * skeleton is in artistic-media-geometry.h. Shapes are NOT renormalised by
 * their own bounds: every entry spans the full u/v square, so the stroke
 * width is exactly the shape's maximum width and the area on a straight
 * skeleton is area_fraction * length * width.
 *
 * This header depends only on the C++ standard library and glib i18n macros
 * (no Gtk, no SPDocument) so the library is unit-testable in isolation.
 *
 * The shape names and unit-space data are Rosepen's own; no third-party
 * brush artwork or data is used.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_LIVE_EFFECTS_ARTISTIC_MEDIA_LIBRARY_H
#define SEEN_LIVE_EFFECTS_ARTISTIC_MEDIA_LIBRARY_H

#include <optional>
#include <span>
#include <string_view>

namespace Inkscape::LivePathEffect::ArtisticMedia {

/// What an artistic media object draws along its skeleton.
enum class Mode {
    Preset,      ///< A tapered/lens/teardrop stroke profile.
    Brush,       ///< A multi-part textured stroke.
    Calligraphic, ///< The exact sweep of a flat nib held at a fixed angle.
};

/// Parse a stored mode key (preset|brush|calligraphic); anything else is nullopt.
std::optional<Mode> parse_mode(std::string_view key);

/// The canonical key of a mode: preset|brush|calligraphic.
char const *mode_key(Mode mode);

/// One built-in shape.
struct LibraryEntry {
    Mode mode;
    char const *name;          ///< Stored name ("lens", "dry-brush", ...).
    char const *label;         ///< N_() translated label.
    char const *unit_d;        ///< Unit-space path data, stored verbatim in shape_d.
    double area_fraction;      ///< Signed area in (u, v); outer subpaths positive, holes negative.
};

/// All built-in shapes, presets first, then brushes, in library order.
std::span<LibraryEntry const> library();

/// The entry called @a name for @a mode; nullptr for calligraphic or unknown.
LibraryEntry const *find(Mode mode, std::string_view name);

/// The shape a mode uses when none is given: "lens", "dry-brush", nullptr.
char const *default_shape(Mode mode);

} // namespace Inkscape::LivePathEffect::ArtisticMedia

#endif // SEEN_LIVE_EFFECTS_ARTISTIC_MEDIA_LIBRARY_H

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
