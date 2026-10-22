// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Built-in artistic media library: the Rosepen-authored preset and brush shapes.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live_effects/artistic-media-library.h"

#include <glibmm/i18n.h>

#include <cstring>

namespace Inkscape::LivePathEffect::ArtisticMedia {

namespace {

/**
 * The built-in shapes, in library order.
 *
 * Every entry spans exactly u [0, 1] x v [-0.5, 0.5]. Outer subpaths wind
 * like "M 0,-0.5 L 1,-0.5 L 1,0.5 L 0,0.5 Z" (positive signed area), holes
 * the other way, so a nonzero fill rule unions the outer parts and punches
 * the holes. area_fraction is the hand-derived signed area in (u, v):
 *
 *   uniform 1; taper-end/-start/-both 1/2; lens 2/3 (two parabolic segments,
 *   each 2/3 x 1 x 0.5); teardrop 7/12 (per half a 1/4 triangle plus a
 *   2/3 x 1/16 quadratic bulge); swell 0.7 (mean width of 0.4 -> 1 -> 0.4);
 *   dry-brush 0.45 (three 1 x 0.3 diamonds); split-stroke 0.8 (two 1 x 0.4
 *   bands); stipple 0.8 (four 0.2 x 1 bands); bristle 0.84 (1 x 1 minus a
 *   0.8 x 0.2 hole).
 */
constexpr LibraryEntry LIBRARY[] = {
    {Mode::Preset, "uniform", N_("Uniform"), "M 0,-0.5 L 1,-0.5 L 1,0.5 L 0,0.5 Z", 1.0},
    {Mode::Preset, "taper-end", N_("Taper End"), "M 0,-0.5 L 1,0 L 0,0.5 Z", 0.5},
    {Mode::Preset, "taper-start", N_("Taper Start"), "M 0,0 L 1,-0.5 L 1,0.5 Z", 0.5},
    {Mode::Preset, "taper-both", N_("Taper Both"), "M 0,0 L 0.5,-0.5 L 1,0 L 0.5,0.5 Z", 0.5},
    {Mode::Preset, "lens", N_("Lens"), "M 0,0 Q 0.5,-1 1,0 Q 0.5,1 0,0 Z", 2.0 / 3.0},
    {Mode::Preset, "teardrop", N_("Teardrop"), "M 0,0 Q 0,-0.5 0.25,-0.5 L 1,0 L 0.25,0.5 Q 0,0.5 0,0 Z",
     7.0 / 12.0},
    {Mode::Preset, "swell", N_("Swell"), "M 0,-0.2 L 0.5,-0.5 L 1,-0.2 L 1,0.2 L 0.5,0.5 L 0,0.2 Z", 0.7},
    {Mode::Brush, "dry-brush", N_("Dry Brush"),
     "M 0,-0.35 L 0.5,-0.5 L 1,-0.35 L 0.5,-0.2 Z M 0,0 L 0.5,-0.15 L 1,0 L 0.5,0.15 Z "
     "M 0,0.35 L 0.5,0.2 L 1,0.35 L 0.5,0.5 Z",
     0.45},
    {Mode::Brush, "split-stroke", N_("Split Stroke"),
     "M 0,-0.5 L 1,-0.5 L 1,-0.1 L 0,-0.1 Z M 0,0.1 L 1,0.1 L 1,0.5 L 0,0.5 Z", 0.8},
    {Mode::Brush, "stipple", N_("Stipple"),
     "M 0,-0.5 L 0.2,-0.5 L 0.2,0.5 L 0,0.5 Z M 0.27,-0.5 L 0.47,-0.5 L 0.47,0.5 L 0.27,0.5 Z "
     "M 0.53,-0.5 L 0.73,-0.5 L 0.73,0.5 L 0.53,0.5 Z M 0.8,-0.5 L 1,-0.5 L 1,0.5 L 0.8,0.5 Z",
     0.8},
    {Mode::Brush, "bristle", N_("Bristle"),
     "M 0,-0.5 L 1,-0.5 L 1,0.5 L 0,0.5 Z M 0.1,-0.1 L 0.1,0.1 L 0.9,0.1 L 0.9,-0.1 Z", 0.84},
};

} // namespace

std::optional<Mode> parse_mode(std::string_view key)
{
    if (key == "preset") {
        return Mode::Preset;
    }
    if (key == "brush") {
        return Mode::Brush;
    }
    if (key == "calligraphic") {
        return Mode::Calligraphic;
    }
    return std::nullopt;
}

char const *mode_key(Mode mode)
{
    switch (mode) {
        case Mode::Preset:
            return "preset";
        case Mode::Brush:
            return "brush";
        case Mode::Calligraphic:
            return "calligraphic";
    }
    return "";
}

std::span<LibraryEntry const> library()
{
    return LIBRARY;
}

LibraryEntry const *find(Mode mode, std::string_view name)
{
    if (mode == Mode::Calligraphic || name.empty()) {
        return nullptr;
    }
    for (auto const &entry : LIBRARY) {
        if (entry.mode == mode && name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

char const *default_shape(Mode mode)
{
    switch (mode) {
        case Mode::Preset:
            return "lens";
        case Mode::Brush:
            return "dry-brush";
        case Mode::Calligraphic:
            return nullptr;
    }
    return nullptr;
}

} // namespace Inkscape::LivePathEffect::ArtisticMedia

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
