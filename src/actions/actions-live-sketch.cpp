// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for Live Sketch strokes, application level and headless.
 *
 * app.live-sketch fits a freehand point list into one plain, open svg:path of
 * smooth cubic nodes (the raw polyline at smoothing 0) and selects it.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "actions-live-sketch.h"

#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>
#include <giomm.h>

#include <2geom/affine.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>

#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "object/object-set.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "path/live-sketch-geometry.h"
#include "selection.h"
#include "svg/svg-length.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "util/cast.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace {

using Inkscape::LiveSketchOps::CreateResult;
using Inkscape::LiveSketchOps::Options;

std::string trim(std::string const &input)
{
    auto const first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    auto const last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

std::string string_param(Glib::VariantBase const &value)
{
    return Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();
}

SPDocument *active_document(InkscapeApplication *app, char const *action)
{
    auto *doc = app->get_active_document();
    if (!doc) {
        g_warning("%s: no active document", action);
    }
    return doc;
}

/// Strict classic-locale number: full token consumed, finite.
bool parse_number(std::string const &text, double &out)
{
    std::string const trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }
    char *end = nullptr;
    double const value = g_ascii_strtod(trimmed.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

/// #rrggbb only; an unstroked open sketch would be invisible, so "none" fails.
bool valid_color(std::string const &value)
{
    if (value.size() != 7 || value[0] != '#') {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        if (!g_ascii_isxdigit(value[i])) {
            return false;
        }
    }
    return true;
}

std::string object_id(SPObject const *object)
{
    auto const *id = object ? object->getId() : nullptr;
    return id ? std::string(id) : std::string("-");
}

/// Pixels per parent-local user unit.
double i2doc_scale(SPItem const *item)
{
    double const factor = item ? item->i2doc_affine().descrim() : 1.0;
    return std::isfinite(factor) && factor > 0.0 ? factor : 1.0;
}

std::string format_number(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

/**
 * The new path's style.
 *
 * A tool style (Options::style) is written verbatim, except that an absolute
 * stroke-width is interpreted as a VISUAL pixel width and rewritten in
 * parent-local units; relative units are left as written. Without a tool
 * style the locked minimal style is used, with the requested visual width.
 */
std::string style_for(Options const &options, SPItem const *parent_item)
{
    double const scale = i2doc_scale(parent_item);

    if (options.style) {
        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_attr_add_from_string(css, options.style->c_str());
        if (char const *width = sp_repr_css_property(css, "stroke-width", nullptr)) {
            SVGLength length;
            if (length.read(width) && svg_length_absolute_unit(length.unit)) {
                std::string const local = format_number(length.computed / scale);
                sp_repr_css_set_property(css, "stroke-width", local.c_str());
            }
        }
        Glib::ustring text;
        sp_repr_css_write_string(css, text);
        sp_repr_css_attr_unref(css);
        return text.raw();
    }

    return "fill:none;stroke:" + options.stroke + ";stroke-opacity:1;stroke-width:" +
           format_number(options.stroke_width_mm * Inkscape::LiveSketch::kPxPerMm / scale) +
           ";stroke-linecap:round;stroke-linejoin:round";
}

} // namespace

namespace Inkscape {
namespace LiveSketchOps {

std::optional<std::vector<Geom::Point>> parse_points(std::string_view spec, std::string &error)
{
    std::vector<double> numbers;
    std::size_t begin = 0;
    while (begin < spec.size()) {
        auto const end = spec.find_first_of(" \t\r\n", begin);
        std::string const token =
            std::string(spec.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (!token.empty()) {
            double number = 0.0;
            if (!parse_number(token, number)) {
                error = "bad number '" + token + "'";
                return std::nullopt;
            }
            numbers.push_back(number);
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = spec.find_first_not_of(" \t\r\n", end);
        if (begin == std::string_view::npos) {
            break;
        }
    }

    if (numbers.size() % 2 != 0) {
        error = "odd number of coordinates";
        return std::nullopt;
    }

    std::vector<Geom::Point> points;
    points.reserve(numbers.size() / 2);
    for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
        points.emplace_back(numbers[i], numbers[i + 1]);
    }
    return points;
}

std::optional<Options> parse_options(std::string_view spec, std::string &error)
{
    Options options;

    std::size_t begin = 0;
    while (begin <= spec.size()) {
        auto const comma = spec.find(',', begin);
        std::string const part =
            std::string(spec.substr(begin, comma == std::string_view::npos ? std::string_view::npos
                                                                          : comma - begin));
        if (!part.empty()) {
            auto const equals = part.find('=');
            if (equals == std::string::npos) {
                error = "unknown option '" + trim(part) + "'";
                return std::nullopt;
            }
            std::string const key = trim(part.substr(0, equals));
            std::string const value = trim(part.substr(equals + 1));

            if (key == "smoothing") {
                double number = 0.0;
                if (!parse_number(value, number)) {
                    error = "bad number '" + value + "'";
                    return std::nullopt;
                }
                if (!(number >= 0.0 && number <= 100.0)) {
                    error = "smoothing must be between 0 and 100";
                    return std::nullopt;
                }
                options.smoothing = number;
            } else if (key == "stroke") {
                if (!valid_color(value)) {
                    error = "bad color '" + value + "'";
                    return std::nullopt;
                }
                options.stroke = value;
            } else if (key == "stroke-width") {
                double number = 0.0;
                if (!parse_number(value, number)) {
                    error = "bad number '" + value + "'";
                    return std::nullopt;
                }
                if (!(number > 0.0 && number <= 100.0)) {
                    error = "stroke-width must be > 0 and <= 100 mm";
                    return std::nullopt;
                }
                options.stroke_width_mm = number;
            } else if (key == "id") {
                if (!value.empty()) {
                    options.id = value;
                }
            } else if (key == "parent") {
                if (!value.empty()) {
                    options.parent = value;
                }
            } else {
                error = "unknown option '" + key + "'";
                return std::nullopt;
            }
        }

        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }

    return options;
}

CreateResult create(SPDocument *doc, std::span<Geom::Point const> points_uu, Options const &options,
                    SPObject *parent_or_null)
{
    CreateResult result;
    auto const fail = [&result](std::string const &reason) {
        result.error = "live-sketch: " + reason;
        return result;
    };

    if (!doc || !doc->getRoot()) {
        return fail("no document");
    }
    doc->ensureUpToDate();

    // Document user units to document pixels; anything outside the SVG plane
    // fails before the document is touched.
    std::vector<Geom::Point> points_px;
    points_px.reserve(points_uu.size());
    for (auto const &p : points_uu) {
        Geom::Point const px = p * doc->getRoot()->c2p;
        if (!LiveSketch::in_plane(px)) {
            return fail("coordinates out of range");
        }
        points_px.push_back(px);
    }

    // Parent: explicit override, else options.parent, else the last layer,
    // else the root.
    SPObject *parent = parent_or_null;
    if (parent) {
        if (!is<SPGroup>(parent)) {
            return fail("parent '" + object_id(parent) + "' is not a group");
        }
    } else if (options.parent) {
        parent = doc->getObjectById(options.parent->c_str());
        if (!is<SPGroup>(parent)) {
            return fail("parent '" + *options.parent + "' is not a group");
        }
    } else {
        for (auto *child : doc->getRoot()->childList(false)) {
            if (is<SPGroup>(child) &&
                g_strcmp0(child->getAttribute("inkscape:groupmode"), "layer") == 0) {
                parent = child; // document order: the last layer wins
            }
        }
        if (!parent) {
            parent = doc->getRoot();
        }
    }
    auto *parent_item = cast<SPItem>(parent);
    if (!parent_item) {
        return fail("parent is not an item");
    }
    if (parent_item->i2doc_affine().isSingular()) {
        return fail("parent '" + object_id(parent) + "' has a degenerate transform");
    }
    if (options.id && doc->getObjectById(options.id->c_str())) {
        return fail("id '" + *options.id + "' already exists");
    }

    double const tolerance_px =
        LiveSketch::smoothing_to_tolerance_mm(options.smoothing) * LiveSketch::kPxPerMm;
    auto const fitted = LiveSketch::fit_whole(points_px, tolerance_px);
    if (!fitted) {
        return fail("needs at least two distinct points");
    }
    Geom::Path const path_local = *fitted * parent_item->i2doc_affine().inverse();
    std::string const style = style_for(options, parent_item);

    Inkscape::XML::Document *xml_doc = doc->getReprDoc();
    auto *path_repr = xml_doc->createElement("svg:path");
    if (options.id) {
        path_repr->setAttribute("id", options.id->c_str());
    }
    path_repr->setAttribute("inkscape:label", "Live Sketch");
    path_repr->setAttribute("style", style.c_str());
    path_repr->setAttribute("d", sp_svg_write_path(Geom::PathVector{path_local}).c_str());

    parent->getRepr()->appendChild(path_repr);
    Inkscape::GC::release(path_repr);
    doc->ensureUpToDate();

    auto *path = cast<SPPath>(doc->getObjectByRepr(path_repr));
    if (!path) {
        // Roll the uncommitted append back: no document change, no undo step.
        DocumentUndo::cancel(doc);
        return fail("could not create the path");
    }

    DocumentUndo::done(doc, RC_("Undo", "Live sketch"), INKSCAPE_ICON("sketch"));
    doc->ensureUpToDate();

    result.path = path;
    return result;
}

} // namespace LiveSketchOps
} // namespace Inkscape

// ---------------------------------------------------------------------------
// Gio actions
// ---------------------------------------------------------------------------

namespace {

const Glib::ustring SECTION = NC_("Action Section", "Live Sketch");

std::vector<std::vector<Glib::ustring>> raw_data_live_sketch = {
    // clang-format off
    {"app.live-sketch", N_("Live Sketch"), SECTION, N_("Create a smoothed freehand stroke through points: <x1> <y1> <x2> <y2> ...[,key=value]*")},
    // clang-format on
};

void live_sketch(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *doc = active_document(app, "live-sketch");
    if (!doc) {
        return;
    }

    std::string const arg = string_param(value);
    auto const comma = arg.find(',');
    std::string const points_part = comma == std::string::npos ? arg : arg.substr(0, comma);
    std::string const options_part =
        comma == std::string::npos ? std::string() : arg.substr(comma + 1);

    std::string error;
    auto points = Inkscape::LiveSketchOps::parse_points(points_part, error);
    if (!points) {
        g_warning("live-sketch: %s", error.c_str());
        return;
    }

    Inkscape::LiveSketchOps::Options options;
    if (!options_part.empty()) {
        auto parsed = Inkscape::LiveSketchOps::parse_options(options_part, error);
        if (!parsed) {
            g_warning("live-sketch: %s", error.c_str());
            return;
        }
        options = std::move(*parsed);
    }

    auto const result = Inkscape::LiveSketchOps::create(doc, *points, options, nullptr);
    if (!result.error.empty()) {
        g_warning("%s", result.error.c_str());
        return;
    }
    if (result.path) {
        if (auto *selection = app->get_active_selection()) {
            selection->set(result.path);
        }
    }
}

} // namespace

void add_actions_live_sketch(InkscapeApplication *app)
{
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    gapp->add_action_with_parameter("live-sketch", String, sigc::bind(sigc::ptr_fun(&live_sketch), app));

    app->get_action_extra_data().add_data(raw_data_live_sketch);
}

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
