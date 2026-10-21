// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SpotSwatch - Document-level storage of named (spot) inks.
 *
 * A spot is a solid document swatch gradient carrying its ink name in the
 * `inkscape:spot-name` attribute. The gradient's single stop is the
 * alternate color used for display and for non-separated output.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_COLORS_SPOT_SWATCH_H
#define SEEN_COLORS_SPOT_SWATCH_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class SPDocument;
class SPGradient;

namespace Inkscape::Colors {

class Color;

namespace Spot {

/**
 * Raw repr attribute holding the spot ink name.
 *
 * Deliberately not a SPAttr: gradients write their repr unchanged apart from
 * the known SVG attributes, so the value round-trips without parser changes.
 */
inline constexpr char const *SPOT_ATTR = "inkscape:spot-name";

/**
 * Return the normalized ink name for a solid swatch gradient, if it is a spot.
 */
std::optional<std::string> get_spot_name(SPGradient const *gradient);

/**
 * Find an existing spot swatch by its normalized ink name.
 */
SPGradient *find_spot(SPDocument *document, std::string_view name);

/**
 * Find or create the spot swatch for an ink name.
 *
 * An existing spot is returned unchanged (its alternate is not overwritten).
 * A new one is created as a solid swatch with a deterministic id; id
 * collisions get a `-2`, `-3`, ... suffix.
 */
SPGradient *ensure_spot(SPDocument *document, std::string_view name, Color const &alternate);

/**
 * Mark or unmark a swatch as a spot ink.
 */
void set_spot_name(SPGradient *gradient, std::optional<std::string_view> name);

/**
 * All spot swatches in the document, in resource order.
 */
std::vector<SPGradient *> list_spots(SPDocument *document);

} // namespace Spot
} // namespace Inkscape::Colors

#endif // SEEN_COLORS_SPOT_SWATCH_H
