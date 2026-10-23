// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Smart Fill creation: collect visible object outlines, take the face at a
 * point and append one plain svg:path covering it.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "actions-smart-fill.h"

#include <algorithm>
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
#include <2geom/transforms.h>

#include "document-undo.h"
#include "document.h"
#include "helper/geom.h"
#include "inkscape-application.h"
#include "object/object-set.h"
#include "object/sp-flowtext.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "path/smart-fill-geometry.h"
#include "selection.h"
#include "style-enums.h"
#include "style.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "util/cast.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace {

using Inkscape::SmartFillOps::Candidate;
using Inkscape::SmartFillOps::CreateResult;
using Inkscape::SmartFillOps::Options;

/// Millimetres to CSS pixels.
constexpr double PX_PER_MM = 96.0 / 25.4;

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

std::string num(double value)
{
    return sp_svg_number_write_de(value, 8, -4);
}

std::string num_fixed(double value, int digits)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof(buffer), digits == 3 ? "%.3f" : "%.6g", value);
    return buffer;
}

bool valid_color(std::string const &value)
{
    if (value == "none" || value == "source") {
        return true;
    }
    if (value.size() != 7 || value[0] != '#') {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        char const c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
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

double i2doc_scale(SPItem const *item)
{
    double const factor = item ? item->i2doc_affine().descrim() : 1.0;
    return std::isfinite(factor) && factor > 0.0 ? factor : 1.0;
}

/// Parent-local user units for a visual millimetre length.
double mm_to_local(SPItem const *item, double mm)
{
    return mm * PX_PER_MM / i2doc_scale(item);
}

Inkscape::SmartFill::Rule rule_of(SPItem const *item)
{
    if (!item || !item->style) {
        return Inkscape::SmartFill::Rule::NonZero;
    }
    return item->style->fill_rule.computed == SP_WIND_RULE_EVENODD
               ? Inkscape::SmartFill::Rule::EvenOdd
               : Inkscape::SmartFill::Rule::NonZero;
}

/// Visible, unlocked, not display/visibility hidden.
bool counts_as_boundary(SPItem *item)
{
    return item && !item->isHidden() && !item->isLocked() && item->style &&
           item->style->visibility.computed == SP_CSS_VISIBILITY_VISIBLE;
}

std::optional<Geom::PathVector> clip_of(SPItem *item, SPItem *clip_root)
{
    if (clip_root && clip_root != item) {
        return item->getClipPathVector(clip_root);
    }
    return item->getClipPathVector();
}

/**
 * Append every visible leaf shape of @a item into one pathvector.
 *
 * Dispatch copied from extract_pathvectors_recursive()
 * (src/ui/tools/booleans-subitems.cpp:78-108), with a flowtext branch added.
 */
void append_geometry(SPItem *item, SPItem *clip_root, Geom::Affine const &transform,
                     Geom::PathVector &out)
{
    if (!counts_as_boundary(item)) {
        return;
    }

    if (auto *group = cast<SPGroup>(item)) {
        for (auto &child : group->children) {
            if (auto *child_item = cast<SPItem>(&child)) {
                append_geometry(child_item, clip_root, child_item->transform * transform, out);
            }
        }
    } else if (auto *image = cast<SPImage>(item)) {
        if (auto clip = clip_of(image, clip_root)) {
            auto const clipped = *clip * transform;
            out.insert(out.end(), clipped.begin(), clipped.end());
        } else if (auto curve = image->get_curve()) {
            auto const transformed = *curve * transform;
            out.insert(out.end(), transformed.begin(), transformed.end());
        }
    } else if (auto *shape = cast<SPShape>(item)) {
        if (auto curve = shape->curve()) {
            auto const transformed = *curve * transform;
            out.insert(out.end(), transformed.begin(), transformed.end());
        }
    } else if (auto *text = cast<SPText>(item)) {
        auto const transformed = text->getNormalizedBpath() * transform;
        out.insert(out.end(), transformed.begin(), transformed.end());
    } else if (auto *flow = cast<SPFlowtext>(item)) {
        auto const transformed = flow->getNormalizedBpath() * transform;
        out.insert(out.end(), transformed.begin(), transformed.end());
    } else if (auto *use = cast<SPUse>(item)) {
        if (use->child) {
            append_geometry(use->child, clip_root,
                            use->child->transform *
                                Geom::Translate(use->x.computed, use->y.computed) * transform,
                            out);
        }
    }
}

void collect_walk(SPItem *item, SPItem *clip_root, Geom::Affine const &transform,
                  std::vector<Candidate> &out)
{
    if (!counts_as_boundary(item)) {
        return;
    }

    if (auto *group = cast<SPGroup>(item)) {
        for (auto &child : group->children) {
            if (auto *child_item = cast<SPItem>(&child)) {
                collect_walk(child_item, clip_root, child_item->transform * transform, out);
            }
        }
        return;
    }

    if (auto *use = cast<SPUse>(item)) {
        // A clone is one candidate: all of its geometry shares the use's style.
        Geom::PathVector geometry;
        auto *original = use->get_original();
        if (auto clip = clip_of(use, clip_root); clip && is<SPImage>(original)) {
            geometry = *clip * transform;
        } else if (use->child) {
            append_geometry(use->child, clip_root,
                            use->child->transform *
                                Geom::Translate(use->x.computed, use->y.computed) * transform,
                            geometry);
        }
        if (!geometry.empty()) {
            out.push_back({item, std::move(geometry), rule_of(item)});
        }
        return;
    }

    Geom::PathVector geometry;
    if (auto *image = cast<SPImage>(item)) {
        if (auto clip = clip_of(image, clip_root)) {
            geometry = *clip * transform;
        } else if (auto curve = image->get_curve()) {
            geometry = *curve * transform;
        }
    } else if (auto *shape = cast<SPShape>(item)) {
        if (auto curve = shape->curve()) {
            geometry = *curve * transform;
        }
    } else if (auto *text = cast<SPText>(item)) {
        geometry = text->getNormalizedBpath() * transform;
    } else if (auto *flow = cast<SPFlowtext>(item)) {
        geometry = flow->getNormalizedBpath() * transform;
    }

    if (!geometry.empty()) {
        out.push_back({item, std::move(geometry), rule_of(item)});
    }
}

} // namespace

namespace Inkscape::SmartFillOps {

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

            if (key == "fill" || key == "stroke") {
                if (!valid_color(value)) {
                    error = "bad color '" + value + "'";
                    return std::nullopt;
                }
                (key == "fill" ? options.fill : options.stroke) = value;
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

std::vector<Candidate> collect_candidates(SPDocument *doc)
{
    std::vector<Candidate> result;
    if (!doc || !doc->getRoot()) {
        return result;
    }
    doc->ensureUpToDate();

    for (auto *child : doc->getRoot()->childList(false)) {
        auto *item = cast<SPItem>(child);
        if (!item) {
            continue;
        }
        // Document order is bottom-first; reverse at the end for topmost first.
        collect_walk(item, item, item->transform, result);
    }

    std::reverse(result.begin(), result.end());
    return result;
}

CreateResult create(SPDocument *doc, Geom::Point const &point_uu, Options const &options,
                    SPObject *parent_or_null)
{
    CreateResult result;
    auto const fail = [&result](std::string const &reason,
                                SmartFill::Status status = SmartFill::Status::NoRegion) {
        result.error = "smart-fill: " + reason;
        result.status = status;
        return result;
    };

    if (!doc || !doc->getRoot()) {
        return fail("no document");
    }

    // Parent: explicit override, else options.parent, else the last layer, else root.
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

    if (options.id && doc->getObjectById(options.id->c_str())) {
        return fail("id '" + *options.id + "' already exists");
    }

    doc->ensureUpToDate();
    auto const candidates = collect_candidates(doc);

    std::vector<SmartFill::Source> sources;
    sources.reserve(candidates.size());
    for (auto const &candidate : candidates) {
        sources.push_back({candidate.path_uu, candidate.rule});
    }

    auto const face = SmartFill::face_at(sources, point_uu);
    std::string const coordinates =
        num_fixed(point_uu.x(), 3) + ", " + num_fixed(point_uu.y(), 3);
    if (face.status == SmartFill::Status::OnBoundary) {
        return fail("point (" + coordinates + ") lies on an object boundary", face.status);
    }
    if (face.status != SmartFill::Status::Ok) {
        return fail("no enclosed area at (" + coordinates + ")", face.status);
    }

    SPItem *source = face.source >= 0 && face.source < static_cast<int>(candidates.size())
                         ? candidates[face.source].item
                         : nullptr;

    // Fill: explicit > source flat colour > fallback.
    std::string fill_value;
    double fill_opacity = 1.0;
    if (options.fill == "none") {
        fill_value = "none";
    } else if (options.fill != "source") {
        fill_value = options.fill;
    } else if (source && source->style && source->style->fill.isColor()) {
        fill_value = source->style->fill.getColor().toString(false);
        fill_opacity = source->style->fill_opacity.as_double();
    } else {
        fill_value = options.fallback_fill;
    }
    if (!std::isfinite(fill_opacity)) {
        fill_opacity = 1.0;
    }
    fill_opacity = std::clamp(fill_opacity, 0.0, 1.0);

    // Outline: explicit > source stroke > none (faithful copy) > fallback.
    std::string stroke_value;
    double stroke_opacity = 1.0;
    bool source_width = false;
    if (options.stroke == "none") {
        stroke_value = "none";
    } else if (options.stroke != "source") {
        stroke_value = options.stroke;
    } else if (source && source->style) {
        if (source->style->stroke.isColor()) {
            stroke_value = source->style->stroke.getColor().toString(false);
            stroke_opacity = source->style->stroke_opacity.as_double();
            source_width = !options.stroke_width_mm.has_value();
        } else if (source->style->stroke.isPaintserver()) {
            stroke_value = options.fallback_stroke;
        } else {
            stroke_value = "none";
        }
    } else {
        stroke_value = options.fallback_stroke;
    }
    if (!std::isfinite(stroke_opacity)) {
        stroke_opacity = 1.0;
    }
    stroke_opacity = std::clamp(stroke_opacity, 0.0, 1.0);

    std::string style = "fill:" + fill_value + ";fill-opacity:" + num_fixed(fill_opacity, 6) +
                        ";fill-rule:evenodd;stroke:" + stroke_value;
    if (stroke_value != "none") {
        double width_uu;
        if (source_width && source) {
            double const source_px = source->style->stroke_width.computed * i2doc_scale(source);
            width_uu = source_px / i2doc_scale(parent_item);
        } else {
            width_uu = mm_to_local(parent_item, options.stroke_width_mm.value_or(0.25));
        }
        style += ";stroke-opacity:" + num_fixed(stroke_opacity, 6);
        style += ";stroke-width:" + num(width_uu);
    }

    Geom::Affine const uu2parent =
        (parent_item->i2doc_affine() * doc->getRoot()->c2p.inverse()).inverse();
    Geom::PathVector const region_parent = face.region * uu2parent;

    // Reprs -----------------------------------------------------------------
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();
    auto *path_repr = xml_doc->createElement("svg:path");
    if (options.id) {
        path_repr->setAttribute("id", options.id->c_str());
    }
    path_repr->setAttribute("inkscape:label", "Smart Fill");
    path_repr->setAttribute("style", style.c_str());
    path_repr->setAttribute("d",
                            sp_svg_write_path(pathv_to_linear_and_cubic_beziers(region_parent)).c_str());

    parent->getRepr()->appendChild(path_repr);
    Inkscape::GC::release(path_repr);
    doc->ensureUpToDate();

    char const *created_id = path_repr->attribute("id");
    auto *path = created_id ? cast<SPPath>(doc->getObjectById(created_id)) : nullptr;
    if (!path) {
        // Roll the uncommitted append back: no document change, no undo step.
        DocumentUndo::cancel(doc);
        return fail("could not create the path");
    }

    DocumentUndo::done(doc, RC_("Undo", "Smart fill"), INKSCAPE_ICON("fill-between-many"));
    doc->ensureUpToDate();

    result.path = created_id ? cast<SPPath>(doc->getObjectById(created_id)) : nullptr;
    result.status = SmartFill::Status::Ok;
    return result;
}

} // namespace Inkscape::SmartFillOps

// ---------------------------------------------------------------------------
// Gio actions
// ---------------------------------------------------------------------------

namespace {

const Glib::ustring SECTION = NC_("Action Section", "Smart Fill");

std::vector<std::vector<Glib::ustring>> raw_data_smart_fill = {
    // clang-format off
    {"app.smart-fill", N_("Smart Fill"), SECTION, N_("Create a filled object covering the area enclosed by object outlines around a point: <x> <y>[,key=value]*")},
    // clang-format on
};

void smart_fill(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *doc = active_document(app, "smart-fill");
    if (!doc) {
        return;
    }

    std::string const arg = string_param(value);
    auto const comma = arg.find(',');
    std::string const point_part = comma == std::string::npos ? arg : arg.substr(0, comma);
    std::string const options_part =
        comma == std::string::npos ? std::string() : arg.substr(comma + 1);

    std::vector<double> numbers;
    std::size_t begin = 0;
    while (begin < point_part.size()) {
        auto const end = point_part.find_first_of(" \t\r\n", begin);
        std::string const token =
            point_part.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!token.empty()) {
            double number = 0.0;
            if (!parse_number(token, number)) {
                g_warning("smart-fill: bad point '%s'", trim(point_part).c_str());
                return;
            }
            numbers.push_back(number);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = point_part.find_first_not_of(" \t\r\n", end);
        if (begin == std::string::npos) {
            break;
        }
    }
    if (numbers.size() != 2) {
        g_warning("smart-fill: bad point '%s'", trim(point_part).c_str());
        return;
    }

    Inkscape::SmartFillOps::Options options;
    if (!options_part.empty()) {
        std::string error;
        auto parsed = Inkscape::SmartFillOps::parse_options(options_part, error);
        if (!parsed) {
            g_warning("smart-fill: %s", error.c_str());
            return;
        }
        options = std::move(*parsed);
    }

    auto const result = Inkscape::SmartFillOps::create(
        doc, Geom::Point(numbers[0], numbers[1]), options, nullptr);
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

void add_actions_smart_fill(InkscapeApplication *app)
{
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    gapp->add_action_with_parameter("smart-fill", String, sigc::bind(sigc::ptr_fun(&smart_fill), app));

    app->get_action_extra_data().add_data(raw_data_smart_fill);
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
