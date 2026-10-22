// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live "artistic media" path effect: a live brush stroke drawn along the
 * path's own editable skeleton.
 *
 * An artistic media object is a single svg:path whose inkscape:original-d IS
 * the skeleton and whose d is this effect's output (the filled brush
 * outline, stroke:none). Editing the skeleton with the Node tool, moving or
 * scaling the object, and changing width / mode / shape / nib angle all
 * re-render the brush; "Break Artistic Media Apart" freezes it. The geometry
 * lives in artistic-media-geometry.h and the built-in shapes in
 * artistic-media-library.h.
 *
 * The supported entry points are the Artistic Media tool, its property bar
 * and the app.artistic-* actions; the effect is hidden from the default LPE
 * gallery.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_LPE_ARTISTIC_MEDIA_H
#define INKSCAPE_LPE_ARTISTIC_MEDIA_H

#include <string>

#include <2geom/affine.h>
#include <2geom/pathvector.h>

#include "live_effects/artistic-media-geometry.h"
#include "live_effects/artistic-media-library.h"
#include "live_effects/effect.h"
#include "live_effects/parameter/bool.h"
#include "live_effects/parameter/enum.h"
#include "live_effects/parameter/hidden.h"
#include "live_effects/parameter/parameter.h"
#include "live_effects/parameter/text.h"

namespace Inkscape {
namespace LivePathEffect {

/** Stored artistic media mode (EnumParam needs a plain enum with a data table). */
enum ArtisticMediaModeParam {
    AMK_PRESET,
    AMK_BRUSH,
    AMK_CALLIGRAPHIC,
    AMK_END
};

class LPEArtisticMedia final : public Effect {
public:
    LPEArtisticMedia(LivePathEffectObject *lpeobject);
    ~LPEArtisticMedia() override = default;

    void doBeforeEffect(SPLPEItem const *lpeitem) override;
    Geom::PathVector doEffect_path(Geom::PathVector const &path_in) override;
    bool doOnOpen(SPLPEItem const *lpeitem) override;
    void doOnApply(SPLPEItem const *lpeitem) override;
    void doOnRemove(SPLPEItem const *lpeitem) override;
    void transform_multiply(Geom::Affine const &postmul, bool set) override;

    /// The mode currently stored in the "mode" parameter.
    ArtisticMedia::Mode mode() const;
    /// The stored shape name (the library name, "-" worthy for calligraphic).
    std::string shape_name() const;
    /// True when the last evaluation had to draw Uniform because the shape is unknown.
    bool shape_is_fallback() const { return _fallback; }
    /// The stored stroke width in millimetres (path-local, before ancestor scaling).
    double width_mm() const;
    /// The stored nib angle, normalised to [0, 180).
    double angle_deg() const;

private:
    LPEArtisticMedia(const LPEArtisticMedia &) = delete;
    LPEArtisticMedia &operator=(const LPEArtisticMedia &) = delete;

    void resolve_shape();
    void repair_shape_d(char const *unit_d);

    EnumParam<ArtisticMediaModeParam> mode_param;
    TextParam shape;
    ScalarParam width;
    ScalarParam angle;
    BoolParam scale_with_object;
    HiddenParam shape_d;

    Geom::PathVector _unit_shape;  ///< resolved unit-space shape for this evaluation
    bool _fallback = false;        ///< last evaluation fell back to Uniform
    bool _fallback_warned = false; ///< one warning per effect instance
    double _width_uu = 0.0;        ///< width in the path's local user units
    double _angle_deg = 30.0;      ///< normalised nib angle
};

} // namespace LivePathEffect
} // namespace Inkscape

#endif // INKSCAPE_LPE_ARTISTIC_MEDIA_H

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
