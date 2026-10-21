// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SpotColors - Pure helpers for named (spot) inks.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_COLORS_SPOT_H
#define SEEN_COLORS_SPOT_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Inkscape::Colors {

class Color;

namespace Spot {

/**
 * Trim and collapse whitespace, then validate the ink name.
 *
 * Rejects empty names, control characters, invalid UTF-8 and names longer
 * than 127 bytes (the practical limit for a PDF separation name).
 */
std::optional<std::string> normalize_name(std::string_view name);

/**
 * Build a deterministic, XML-id-safe swatch id from an ink name.
 *
 * Every byte outside [A-Za-z0-9_-] is replaced with '_'. Collisions are
 * resolved by the document layer (see spot-swatch.h).
 */
std::string swatch_id(std::string_view name);

enum class Model
{
    CMYK,
    RGB
};

/**
 * The fallback color a spot ink is rendered with when it is not separated.
 */
struct Alternate
{
    Model model;
    std::vector<double> values;
};

/**
 * Pick a device alternate for the given color.
 *
 * DeviceCMYK and CMYK CMS colors keep their values as-is; everything else is
 * converted to RGB. Alpha is never part of the alternate.
 */
Alternate alternate_for(Color const &color);

} // namespace Spot
} // namespace Inkscape::Colors

#endif // SEEN_COLORS_SPOT_H
