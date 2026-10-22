// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live "artistic media" path effect: a live brush stroke drawn along the
 * path's own editable skeleton.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live_effects/lpe-artistic-media.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <glib.h>
#include <glibmm/i18n.h>

#include <2geom/affine.h>
#include <2geom/angle.h>
#include <2geom/point.h>

#include "desktop-style.h"
#include "document-undo.h"
#include "document.h"
#include "live_effects/fill-conversion.h"
#include "object/sp-item.h"
#include "object/sp-paint-server.h"
#include "object/sp-shape.h"
#include "style.h"
#include "util/enums.h"
#include "util/units.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape {
namespace LivePathEffect {

namespace {

using ArtisticMedia::Mode;

static const Util::EnumData<ArtisticMediaModeParam> ArtisticMediaModeData[] = {
    {AMK_PRESET,       NC_("path effect", "Preset"),       "preset"       },
    {AMK_BRUSH,        NC_("path effect", "Brush"),        "brush"        },
    {AMK_CALLIGRAPHIC, NC_("path effect", "Calligraphic"), "calligraphic" },
};
static const Util::EnumDataConverter<ArtisticMediaModeParam> ArtisticMediaModeConverter(ArtisticMediaModeData,
                                                                                       AMK_END);

} // namespace

LPEArtisticMedia::LPEArtisticMedia(LivePathEffectObject *lpeobject)
    : Effect(lpeobject)
    , mode_param(_("Mode:"), _("Preset shape, brush stroke or calligraphic nib"), "mode",
                 ArtisticMediaModeConverter, &wr, this, AMK_PRESET)
    , shape(_("Shape"), _("Name of the preset or brush in the built-in library"), "shape", &wr, this, "lens")
    , width(_("Width"), _("Stroke width in mm, measured in the path's own user units"), "width", &wr, this, 10.0)
    , angle(_("Nib angle"), _("Calligraphic nib angle in degrees, fixed on the page"), "angle", &wr, this, 30.0)
    , scale_with_object(_("Scale with object"), _("Scale the stroke width when the object is scaled"),
                        "scale_with_object", &wr, this, true)
    , shape_d(_("Shape data"), _("Embedded unit-space shape data"), "shape_d", &wr, this, "")
{
    // Stored in path-local user units: 1000 mm can be far more than 1000 uu
    // (px documents, scaled groups), so the 1000 mm limit lives in the actions.
    width.param_set_range(0.0, 1e9);
    width.param_set_digits(3);
    angle.param_set_range(0.0, 180.0);
    angle.param_set_digits(3);

    registerParameter(&mode_param);
    registerParameter(&shape);
    registerParameter(&width);
    registerParameter(&angle);
    registerParameter(&scale_with_object);
    registerParameter(&shape_d);
}

Mode LPEArtisticMedia::mode() const
{
    switch (static_cast<ArtisticMediaModeParam>(mode_param.get_value())) {
        case AMK_BRUSH:
            return Mode::Brush;
        case AMK_CALLIGRAPHIC:
            return Mode::Calligraphic;
        default:
            return Mode::Preset;
    }
}

std::string LPEArtisticMedia::shape_name() const
{
    return shape.get_value().raw();
}

double LPEArtisticMedia::width_mm() const
{
    return static_cast<double>(width);
}

double LPEArtisticMedia::angle_deg() const
{
    return ArtisticMedia::normalize_angle(static_cast<double>(angle));
}

void LPEArtisticMedia::repair_shape_d(char const *unit_d)
{
    if (!unit_d || shape_d.get_value().raw() == unit_d) {
        return;
    }
    // Load-time repair must not create an undo entry.
    if (auto *doc = getSPDoc()) {
        DocumentUndo::ScopedInsensitive insensitive(doc);
        shape_d.param_setValue(unit_d, true);
    } else {
        shape_d.param_setValue(unit_d, true);
    }
    g_debug("LPEArtisticMedia: repaired embedded shape data for '%s'", shape_name().c_str());
}

void LPEArtisticMedia::resolve_shape()
{
    Mode const m = mode();

    // Embedded data wins: the object looks the same whatever the library says.
    std::string const embedded = shape_d.get_value().raw();
    if (!embedded.empty()) {
        if (auto parsed = ArtisticMedia::parse_unit_shape(embedded)) {
            _unit_shape = std::move(*parsed);
            return;
        }
    }

    // A known name is repaired from the library (and re-embedded).
    if (auto const *entry = ArtisticMedia::find(m, shape_name())) {
        if (auto parsed = ArtisticMedia::parse_unit_shape(entry->unit_d)) {
            _unit_shape = std::move(*parsed);
            repair_shape_d(entry->unit_d);
            return;
        }
    }

    // Unknown name: draw Uniform, write nothing (a newer library may know it).
    _fallback = true;
    if (auto const *uniform = ArtisticMedia::find(Mode::Preset, "uniform")) {
        _unit_shape = ArtisticMedia::parse_unit_shape(uniform->unit_d).value_or(Geom::PathVector{});
    }
    if (!_fallback_warned) {
        _fallback_warned = true;
        g_warning("LPEArtisticMedia: unknown %s shape '%s', drawing Uniform", ArtisticMedia::mode_key(m),
                  shape_name().c_str());
    }
}

void LPEArtisticMedia::doBeforeEffect(SPLPEItem const *lpeitem)
{
    _unit_shape = {};
    _fallback = false;
    _width_uu = 0.0;
    _angle_deg = angle_deg();

    // The width parameter is a length in the path's own user units; the
    // actions and the property bar present it in millimetres.
    _width_uu = width_mm();

    if (mode() == Mode::Calligraphic) {
        // The nib is PAGE-fixed, but an LPE path keeps its transform attribute
        // (it is unoptimized), so the sweep is computed in path coordinates:
        // map the page nib (direction and visual length) back through i2doc.
        if (lpeitem) {
            Geom::Affine const linear = lpeitem->i2doc_affine().withoutTranslation();
            double const factor = linear.descrim();
            if (!linear.isSingular(1e-12) && std::isfinite(factor) && factor > 0.0) {
                double const rad = Geom::rad_from_deg(_angle_deg);
                Geom::Point const local = Geom::Point(std::cos(rad), -std::sin(rad)) * linear.inverse();
                double const length = local.length();
                if (std::isfinite(length) && length > 0.0) {
                    _angle_deg = ArtisticMedia::normalize_angle(
                        Geom::deg_from_rad(std::atan2(-local[Geom::Y], local[Geom::X])));
                    _width_uu = width_mm() * factor * length;
                }
            }
        }
        return; // the nib ignores shapes
    }
    resolve_shape();
}

Geom::PathVector LPEArtisticMedia::doEffect_path(Geom::PathVector const &path_in)
{
    ArtisticMedia::Result result;
    if (mode() == Mode::Calligraphic) {
        result = ArtisticMedia::calligraphic(path_in, _width_uu, _angle_deg);
    } else {
        result = ArtisticMedia::map_along(path_in, _unit_shape, _width_uu);
    }
    if (!result.valid) {
        return path_in; // degenerate skeleton or width: keep the input, never NaN
    }
    return result.path;
}

bool LPEArtisticMedia::doOnOpen(SPLPEItem const * /*lpeitem*/)
{
    // Load-time repair/fallback for the embedded shape data, run by
    // sp_file_fix_lpe inside an undo-insensitive scope: a document opened with
    // a missing or stale shape_d is fixed before its first evaluation and
    // without an undo entry.
    if (mode() != Mode::Calligraphic) {
        _unit_shape = {};
        _fallback = false;
        resolve_shape();
    }
    return false;
}

void LPEArtisticMedia::doOnApply(SPLPEItem const *lpeitem)
{
    auto *item = const_cast<SPLPEItem *>(lpeitem);
    auto *shape_item = cast<SPShape>(item);
    if (!shape_item || !shape_item->style) {
        return;
    }
    SPStyle *style = shape_item->style;

    SPCSSAttr *css = sp_repr_css_attr_new();
    bool have_fill = false;

    // The stroke paint becomes the brush fill; a fill-only path keeps its fill.
    if (style->stroke.isColor()) {
        auto color = style->stroke.getColor();
        color.addOpacity(static_cast<double>(style->stroke_opacity));
        sp_repr_css_set_property_string(css, "fill", color.toString());
        have_fill = true;
    } else if (style->stroke.isPaintserver()) {
        if (auto *server = style->getStrokePaintServer()) {
            sp_repr_css_set_property(css, "fill", ("url(#" + std::string(server->getId()) + ")").c_str());
            have_fill = true;
        }
    }
    if (!have_fill && !style->fill.isColor() && !style->fill.isPaintserver()) {
        sp_repr_css_set_property(css, "fill", "#000000");
    }
    sp_repr_css_set_property(css, "stroke", "none");
    sp_repr_css_set_property(css, "fill-rule", "nonzero");
    sp_desktop_apply_css_recursive(shape_item, css, true);
    sp_repr_css_attr_unref(css);
}

void LPEArtisticMedia::doOnRemove(SPLPEItem const *lpeitem)
{
    if (keep_paths) {
        return; // flatten / break apart: keep the outline and its style
    }
    auto *shape_item = cast<SPShape>(const_cast<SPLPEItem *>(lpeitem));
    if (!shape_item) {
        return;
    }
    lpe_shape_revert_stroke_and_fill(shape_item, width_mm());
}

void LPEArtisticMedia::transform_multiply(Geom::Affine const &postmul, bool /*set*/)
{
    // An LPE path is "unoptimized" (SPItem::unoptimized() is true for anything
    // carrying inkscape:path-effect), so the transform stays on the object and
    // scales the stroke visually through i2doc: the parameter needs no change
    // when the stroke scales with the object, and must be divided to hold the
    // visual width otherwise. SPItem::doWriteTransform notifies the NEW
    // absolute item transform before storing it, so the delta is taken against
    // the transform still on the item (a move after a scale must not shrink
    // the width again).
    if (sp_lpe_item && sp_lpe_item->unoptimized()) {
        if (scale_with_object) {
            return;
        }
        Geom::Affine const old_transform = sp_lpe_item->transform;
        if (old_transform.isSingular(1e-12)) {
            return;
        }
        double const factor = (old_transform.inverse() * postmul).descrim();
        if (!std::isfinite(factor) || factor <= 0.0 || Geom::are_near(factor, 1.0, 1e-12)) {
            return;
        }
        width.param_set_value(width_mm() / factor);
        width.write_to_SVG();
        return;
    }

    double const factor = postmul.descrim();
    if (!std::isfinite(factor) || factor <= 0.0) {
        return;
    }

    // A baked transform keeps the path coordinates and i2doc; scale the
    // parameter so the stroke follows the object.
    if (!scale_with_object) {
        return;
    }
    width.param_set_value(width_mm() * factor);
    width.write_to_SVG();
}

} // namespace LivePathEffect
} // namespace Inkscape

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
