// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for live dimensions, application level and headless.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "actions-dimension.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>
#include <giomm.h>

#include <2geom/path.h>
#include <2geom/pathvector.h>

#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "live_effects/dimension-geometry.h"
#include "live_effects/effect-enum.h"
#include "live_effects/lpe-dimension.h"
#include "object/sp-defs.h"
#include "object/sp-item-group.h"
#include "object/sp-lpe-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "util/units.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace {

using Inkscape::DimensionOps::Ref;
using Inkscape::DimensionOps::Row;
using Inkscape::DimensionOps::Spec;
using LPEDim = Inkscape::LivePathEffect::LPEDimension;

std::string trim(std::string const &input)
{
    auto const first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    auto const last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

std::vector<std::string> split(std::string const &input, char sep)
{
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin <= input.size()) {
        std::size_t const end = input.find(sep, begin);
        out.push_back(input.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return out;
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

bool parse_bool_value(std::string const &text, bool &out)
{
    std::string const t = trim(text);
    if (t == "1" || t == "true" || t == "yes" || t == "on") {
        out = true;
        return true;
    }
    if (t == "0" || t == "false" || t == "no" || t == "off") {
        out = false;
        return true;
    }
    return false;
}

std::string num(double v)
{
    return sp_svg_number_write_de(v, 8, -4);
}

std::string num_fixed(double v, int digits)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof(buffer), digits == 6 ? "%.6f" : "%.3f", v);
    return buffer;
}

// ---------------------------------------------------------------------------
// Spec parsing (action layer)
// ---------------------------------------------------------------------------

std::optional<Ref> parse_ref(std::string const &token, std::string &error)
{
    Ref ref;
    auto const at = token.find('@');
    if (at != std::string::npos) {
        ref.id = trim(token.substr(0, at));
        ref.anchor = trim(token.substr(at + 1));
        if (ref.id.empty() || ref.anchor.empty()) {
            error = "bad ref '" + token + "' (expected <id>@<anchor>)";
            return std::nullopt;
        }
        return ref;
    }

    // Free point: "<x> <y>" in document user units.
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin < token.size() && parts.size() < 3) {
        auto const end = token.find_first_of(" \t\r\n", begin);
        parts.push_back(token.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = token.find_first_not_of(" \t\r\n", end);
        if (begin == std::string::npos) {
            break;
        }
    }
    double x = 0;
    double y = 0;
    if (parts.size() != 2 || !parse_number(parts[0], x) || !parse_number(parts[1], y)) {
        error = "bad ref '" + token + "' (expected <id>@<anchor> or \"<x> <y>\")";
        return std::nullopt;
    }
    ref.point = Geom::Point(x, y);
    return ref;
}

/// Parse "<kind|auto>,<ref>[,<ref>][,<ref>][,key=value]*"; the segment
/// shorthand "segment,<id>,<k>" is also accepted.
bool parse_add_arg(std::string const &arg, Spec &spec, std::string &error)
{
    auto const fields = split(arg, ',');
    if (fields.empty() || trim(fields[0]).empty()) {
        error = "empty argument";
        return false;
    }
    spec.kind = trim(fields[0]);

    std::size_t index = 1;
    double index_number = 0;
    std::string const first = trim(fields[index < fields.size() ? index : 0]);
    if (spec.kind == "segment" && fields.size() >= 3 && first.find('@') == std::string::npos &&
        first.find_first_of(" \t\r\n") == std::string::npos && parse_number(trim(fields[2]), index_number) &&
        index_number == std::floor(index_number) && index_number >= 0) {
        // segment,<id>,<k>: measure nodes k and k+1 of one item.
        Ref ref;
        ref.id = first;
        ref.anchor = "node:" + std::to_string(static_cast<int>(index_number));
        spec.refs.push_back(ref);
        index = 3;
    } else {
        std::size_t const max_refs = (spec.kind == "angular") ? 3 : 2;
        while (index < fields.size() && spec.refs.size() < max_refs &&
               fields[index].find('=') == std::string::npos) {
            auto ref = parse_ref(fields[index], error);
            if (!ref) {
                return false;
            }
            spec.refs.push_back(*ref);
            ++index;
        }
    }

    for (; index < fields.size(); ++index) {
        auto const eq = fields[index].find('=');
        if (eq == std::string::npos) {
            error = "bad option '" + fields[index] + "' (expected key=value)";
            return false;
        }
        std::string const key = trim(fields[index].substr(0, eq));
        std::string const value = trim(fields[index].substr(eq + 1));
        if (key.empty()) {
            error = "bad option '" + fields[index] + "'";
            return false;
        }
        spec.options.emplace_back(key, value);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Dimension internals shared by create/list/set
// ---------------------------------------------------------------------------

/// The rosepen-dimension-line path child of a dimension group.
SPPath *dimension_path(SPObject *group)
{
    if (!group) {
        return nullptr;
    }
    for (auto &child : group->children) {
        auto *path = cast<SPPath>(&child);
        if (!path) {
            continue;
        }
        if (char const *cls = path->getRepr()->attribute("class")) {
            if (std::strstr(cls, "rosepen-dimension-line")) {
                return path;
            }
        }
    }
    return nullptr;
}

/// The label text repr of a dimension group (first svg:text sibling of the path).
Inkscape::XML::Node *dimension_label_repr(SPObject *group)
{
    if (!group) {
        return nullptr;
    }
    for (auto &child : group->children) {
        auto *repr = child.getRepr();
        if (repr && std::strcmp(repr->name(), "svg:text") == 0) {
            if (char const *cls = repr->attribute("class")) {
                if (std::strstr(cls, "rosepen-dimension-label")) {
                    return repr;
                }
            }
        }
    }
    return nullptr;
}

LPEDim *dimension_effect(SPObject *group)
{
    auto *path = dimension_path(group);
    if (!path) {
        return nullptr;
    }
    auto *effect = path->getFirstPathEffectOfType(static_cast<int>(Inkscape::LivePathEffect::EffectType::DIMENSION));
    return dynamic_cast<LPEDim *>(effect);
}

std::string label_text(SPObject *group)
{
    auto *label = dimension_label_repr(group);
    if (!label) {
        return {};
    }
    for (auto *span = label->firstChild(); span; span = span->next()) {
        if (std::strcmp(span->name(), "svg:tspan") != 0) {
            continue;
        }
        for (auto *text = span->firstChild(); text; text = text->next()) {
            if (text->type() == Inkscape::XML::NodeType::TEXT_NODE && text->content()) {
                return text->content();
            }
        }
    }
    return {};
}

/// Collect dimension groups (class rosepen-dimension) in document order.
void collect_dimensions(SPObject *obj, std::vector<SPGroup *> &out)
{
    for (auto *child : obj->childList(false)) {
        if (auto *group = cast<SPGroup>(child)) {
            if (char const *cls = group->getRepr()->attribute("class")) {
                if (std::strstr(cls, "rosepen-dimension")) {
                    out.push_back(group);
                }
            }
        }
        collect_dimensions(child, out);
    }
}

struct Options {
    std::optional<double> offset;
    std::optional<int> precision;
    std::string unit = "mm";
    std::optional<double> scale;
    std::string text = "above";
    std::optional<bool> horizontal;
    std::optional<bool> units;
    std::string prefix;
    std::string suffix;
    std::optional<double> font_size;
    std::string id;
};

bool apply_option(std::string const &key, std::string const &value, Options &opts, std::string &error)
{
    double number = 0;
    bool flag = false;
    if (key == "offset") {
        if (!parse_number(value, number)) {
            error = "unparsable number '" + value + "' for key offset";
            return false;
        }
        opts.offset = number;
    } else if (key == "precision") {
        if (!parse_number(value, number) || number != std::floor(number) || number < 0 || number > 6) {
            error = "precision must be an integer in 0..6, got '" + value + "'";
            return false;
        }
        opts.precision = static_cast<int>(number);
    } else if (key == "unit") {
        if (!Inkscape::Util::UnitTable::get().hasUnit(value)) {
            error = "unknown unit '" + value + "'";
            return false;
        }
        opts.unit = value;
    } else if (key == "scale") {
        if (!parse_number(value, number)) {
            error = "unparsable number '" + value + "' for key scale";
            return false;
        }
        opts.scale = number;
    } else if (key == "text") {
        if (value != "above" && value != "inline" && value != "below") {
            error = "text must be above, inline or below, got '" + value + "'";
            return false;
        }
        opts.text = value;
    } else if (key == "horizontal" || key == "units") {
        if (!parse_bool_value(value, flag)) {
            error = "key " + key + " expects a boolean, got '" + value + "'";
            return false;
        }
        (key == "horizontal" ? opts.horizontal : opts.units) = flag;
    } else if (key == "prefix") {
        opts.prefix = value;
    } else if (key == "suffix") {
        opts.suffix = value;
    } else if (key == "font-size") {
        if (!parse_number(value, number) || number <= 0) {
            error = "font-size must be a positive number, got '" + value + "'";
            return false;
        }
        opts.font_size = number;
    } else if (key == "id") {
        if (value.empty() || value.find_first_of(" \t\r\n") != std::string::npos) {
            error = "bad id '" + value + "'";
            return false;
        }
        opts.id = value;
    } else {
        error = "unknown key '" + key + "'";
        return false;
    }
    return true;
}

/// Validate one dimension-set option and map it to its effect attribute and value.
bool set_option_attr(std::string const &key, std::string const &value, std::string &attr, std::string &out,
                     std::string &error)
{
    double number = 0;
    bool flag = false;
    if (key == "kind") {
        if (value != "aligned" && value != "horizontal" && value != "vertical") {
            error = "kind must be aligned, horizontal or vertical, got '" + value + "'";
            return false;
        }
        attr = "kind";
        out = value;
    } else if (key == "offset") {
        if (!parse_number(value, number)) {
            error = "unparsable number '" + value + "' for key offset";
            return false;
        }
        attr = "offset";
        out = num(number);
    } else if (key == "precision") {
        if (!parse_number(value, number) || number != std::floor(number) || number < 0 || number > 6) {
            error = "precision must be an integer in 0..6, got '" + value + "'";
            return false;
        }
        attr = "precision";
        out = std::to_string(static_cast<int>(number));
    } else if (key == "unit") {
        if (!Inkscape::Util::UnitTable::get().hasUnit(value)) {
            error = "unknown unit '" + value + "'";
            return false;
        }
        attr = "unit";
        out = value;
    } else if (key == "scale") {
        if (!parse_number(value, number)) {
            error = "unparsable number '" + value + "' for key scale";
            return false;
        }
        attr = "scale";
        out = num(number);
    } else if (key == "text") {
        if (value != "above" && value != "inline" && value != "below") {
            error = "text must be above, inline or below, got '" + value + "'";
            return false;
        }
        attr = "text_position";
        out = value;
    } else if (key == "horizontal" || key == "units") {
        if (!parse_bool_value(value, flag)) {
            error = "key " + key + " expects a boolean, got '" + value + "'";
            return false;
        }
        attr = key == "horizontal" ? "text_horizontal" : "show_units";
        out = flag ? "true" : "false";
    } else if (key == "prefix") {
        attr = "prefix";
        out = value;
    } else if (key == "suffix") {
        attr = "suffix";
        out = value;
    } else if (key == "font-size") {
        if (!parse_number(value, number) || number <= 0) {
            error = "font-size must be a positive number, got '" + value + "'";
            return false;
        }
        attr = "font_size";
        out = num(number);
    } else {
        error = "unknown key '" + key + "'";
        return false;
    }
    return true;
}

/// mm -> document user units.
double mm_to_uu(SPDocument *doc, double mm)
{
    double const doc_scale = std::max(doc->getDocumentScale()[Geom::X], 1e-9);
    return Inkscape::Util::Quantity::convert(mm, "mm", "px") / doc_scale;
}

std::string label_content(SPDocument *doc, Inkscape::LivePathEffect::Dimension::Kind kind, double value_uu_or_deg,
                          Options const &opts)
{
    bool const angular = kind == Inkscape::LivePathEffect::Dimension::Kind::Angular;
    double v = value_uu_or_deg;
    char const *unit_abbr = angular ? "" : opts.unit.c_str();
    if (!angular) {
        v = Inkscape::LivePathEffect::Dimension::to_unit(v, std::max(doc->getDocumentScale()[Geom::X], 1e-9),
                                                         unit_abbr, opts.scale.value_or(1.0));
    }
    return Inkscape::LivePathEffect::Dimension::format_label(
                   v, opts.precision.value_or(2), unit_abbr, opts.units.value_or(true), opts.prefix, opts.suffix,
                   angular)
        .raw();
}

} // namespace

namespace Inkscape {

namespace DimensionOps {

SPGroup *create(SPDocument *doc, Spec const &spec, SPObject *parent_or_null)
{
    auto fail = [](std::string const &reason) -> SPGroup * {
        g_warning("dimension-add: %s", reason.c_str());
        return nullptr;
    };
    if (!doc || !doc->getRoot()) {
        return fail("no document");
    }

    // Validate everything before touching the document.
    Options opts;
    std::string error;
    for (auto const &[key, value] : spec.options) {
        if (!apply_option(key, value, opts, error)) {
            return fail(error);
        }
    }

    using Inkscape::LivePathEffect::Dimension::Kind;

    Kind kind = Kind::Aligned;
    bool const is_auto = spec.kind == "auto";
    if (!is_auto) {
        if (auto parsed = Inkscape::LivePathEffect::Dimension::parse_kind(spec.kind)) {
            kind = *parsed;
        } else {
            return fail("unknown kind '" + spec.kind + "'");
        }
    }

    std::vector<Ref> refs = spec.refs;
    if (kind == Kind::Segment && refs.size() == 1 && !refs[0].id.empty() && refs[0].anchor.rfind("node:", 0) == 0) {
        // "segment,<id>,<k>" shorthand: measure nodes k and k+1 of one item.
        std::string const k = refs[0].anchor.substr(5);
        double index = 0;
        if (!parse_number(k, index) || index != std::floor(index) || index < 0) {
            return fail("bad segment index '" + k + "'");
        }
        Ref end;
        end.id = refs[0].id;
        end.anchor = "node:" + std::to_string(static_cast<int>(index) + 1);
        refs.push_back(end);
    }

    std::size_t const needed = kind == Kind::Angular ? 3 : 2;
    if (refs.size() != needed) {
        return fail("kind " + (is_auto ? std::string("auto") : spec.kind) + " expects " +
                    std::to_string(needed) + " refs, got " + std::to_string(refs.size()));
    }

    // Resolve the measured points in document coordinates.
    std::vector<Geom::Point> pts;
    SPItem *first_item = nullptr;
    for (auto const &ref : refs) {
        if (ref.id.empty()) {
            // Free points are given in document user units; resolve_anchor()
            // and the LPE work in document coordinates (px).
            pts.push_back(ref.point * doc->getRoot()->c2p);
            continue;
        }
        SPObject *obj = doc->getObjectById(ref.id);
        if (!obj) {
            return fail("unknown id '" + ref.id + "'");
        }
        auto *item = cast<SPItem>(obj);
        if (!item) {
            return fail("'" + ref.id + "' is not a measurable item");
        }
        if (!first_item) {
            first_item = item;
        }
        auto anchor = Inkscape::LivePathEffect::Dimension::parse_anchor(ref.anchor);
        if (!anchor) {
            return fail("bad anchor '" + ref.anchor + "' on '" + ref.id + "'");
        }
        Geom::PathVector curve;
        if (auto *shape = cast<SPShape>(obj)) {
            if (auto const *c = shape->curve()) {
                curve = *c;
                curve *= item->i2doc_affine();
            }
        }
        auto point = Inkscape::LivePathEffect::Dimension::resolve_anchor(*anchor, item->documentGeometricBounds(), curve);
        if (!point) {
            return fail("anchor '" + ref.anchor + "' does not resolve on '" + ref.id + "'");
        }
        pts.push_back(*point);
    }
    if (is_auto) {
        kind = Inkscape::LivePathEffect::Dimension::resolve_auto(pts[0], pts[1]);
    }

    if (!opts.id.empty()) {
        for (auto const &derived : {opts.id, opts.id + "-line", opts.id + "-label", opts.id + "-lpe"}) {
            if (doc->getObjectById(derived)) {
                return fail("id '" + derived + "' already in use");
            }
        }
    }

    SPObject *parent = parent_or_null;
    if (!parent) {
        // The nearest group above the first measured item (a tspan's text,
        // a clone or a switch cannot host the dimension group).
        parent = first_item ? first_item->parent : nullptr;
        while (parent && !is<SPGroup>(parent)) {
            parent = parent->parent;
        }
        if (!parent) {
            parent = doc->getRoot();
        }
    }
    auto *parent_item = cast<SPItem>(parent);
    if (!parent_item) {
        return fail("parent is not an item");
    }
    Geom::Affine const doc2parent = parent_item->i2doc_affine().inverse();

    // Build the geometry once here so the initial d attribute and label are
    // correct even before the effect re-evaluates.
    double const doc_scale = std::max(doc->getDocumentScale()[Geom::X], 1e-9);
    Inkscape::LivePathEffect::Dimension::Style style;
    style.offset = mm_to_uu(doc, opts.offset.value_or(5.0));
    style.arrow_length = mm_to_uu(doc, 2.5);
    style.ext_gap = mm_to_uu(doc, 1.0);
    style.ext_overshoot = mm_to_uu(doc, 2.0);
    style.text_position = opts.text == "inline" ? Inkscape::LivePathEffect::Dimension::TextPosition::Inline
                          : opts.text == "below"
                              ? Inkscape::LivePathEffect::Dimension::TextPosition::Below
                              : Inkscape::LivePathEffect::Dimension::TextPosition::Above;
    style.text_horizontal = opts.horizontal.value_or(false);
    style.label_gap =
        0.35 * Inkscape::Util::Quantity::convert(opts.font_size.value_or(10.0), "pt", "px") / doc_scale;

    std::vector<Geom::Point> parent_pts;
    parent_pts.reserve(pts.size());
    for (auto const &p : pts) {
        parent_pts.push_back(p * doc2parent);
    }
    auto const result = Inkscape::LivePathEffect::Dimension::build(kind, parent_pts, style, true);

    // Reprs -----------------------------------------------------------------
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    auto *lpe_repr = xml_doc->createElement("inkscape:path-effect");
    lpe_repr->setAttribute("effect", "dimension");
    lpe_repr->setAttribute("kind", Inkscape::LivePathEffect::Dimension::kind_key(kind));
    lpe_repr->setAttribute("offset", num(opts.offset.value_or(5.0)));
    lpe_repr->setAttribute("precision", std::to_string(opts.precision.value_or(2)));
    lpe_repr->setAttribute("unit", opts.unit);
    lpe_repr->setAttribute("scale", num(opts.scale.value_or(1.0)));
    lpe_repr->setAttribute("text_position", opts.text);
    lpe_repr->setAttribute("text_horizontal", opts.horizontal.value_or(false) ? "true" : "false");
    lpe_repr->setAttribute("show_units", opts.units.value_or(true) ? "true" : "false");
    lpe_repr->setAttribute("prefix", opts.prefix);
    lpe_repr->setAttribute("suffix", opts.suffix);
    lpe_repr->setAttribute("font_size", num(opts.font_size.value_or(10.0)));
    lpe_repr->setAttribute("arrow_length", num(2.5));
    lpe_repr->setAttribute("ext_gap", num(1.0));
    lpe_repr->setAttribute("ext_overshoot", num(2.0));
    if (!opts.id.empty()) {
        lpe_repr->setAttribute("id", opts.id + "-lpe");
    }
    doc->getDefs()->getRepr()->appendChild(lpe_repr);
    Inkscape::GC::release(lpe_repr);
    char const *lpe_id = lpe_repr->attribute("id");
    if (!lpe_id) {
        return fail("could not create the dimension effect");
    }

    auto *group_repr = xml_doc->createElement("svg:g");
    group_repr->setAttribute("class", "rosepen-dimension");
    group_repr->setAttribute("inkscape:label", "Dimension");
    if (!opts.id.empty()) {
        group_repr->setAttribute("id", opts.id);
    }

    double const stroke_width = mm_to_uu(doc, 0.2);
    auto *path_repr = xml_doc->createElement("svg:path");
    path_repr->setAttribute("class", "rosepen-dimension-line");
    path_repr->setAttribute("style",
                            "fill:#000000;stroke:#000000;stroke-width:" + num(stroke_width) + ";stroke-linecap:butt");
    path_repr->setAttribute("inkscape:path-effect", std::string("#") + lpe_id);
    {
        // original-d visits the measured points in path coordinates so the
        // free-point fallback stays sane; angular order is arm A, vertex, arm B.
        Geom::PathVector original;
        Geom::Path path(parent_pts[0]);
        for (std::size_t i = 1; i < parent_pts.size(); ++i) {
            path.appendNew<Geom::LineSegment>(parent_pts[i]);
        }
        original.push_back(path);
        path_repr->setAttribute("inkscape:original-d", sp_svg_write_path(original));
        path_repr->setAttribute("d", sp_svg_write_path(result.path));
    }

    auto *text_repr = xml_doc->createElement("svg:text");
    text_repr->setAttribute("class", "rosepen-dimension-label");
    text_repr->setAttribute("xml:space", "preserve");
    text_repr->setAttribute(
        "style", "text-anchor:middle;font-size:" + num(opts.font_size.value_or(10.0)) + "pt;fill:#000000");
    text_repr->setAttributeSvgDouble("x", result.label_pos.x());
    text_repr->setAttributeSvgDouble("y", result.label_pos.y());
    if (result.valid && std::fabs(result.label_angle_deg) > 1e-9) {
        text_repr->setAttribute("transform", "rotate(" + num(result.label_angle_deg) + " " + num(result.label_pos.x()) +
                                                 " " + num(result.label_pos.y()) + ")");
    }
    {
        auto *tspan = xml_doc->createElement("svg:tspan");
        tspan->setAttribute("sodipodi:role", "line");
        // The label lives in a text child node: setting content on the tspan
        // element itself never reaches the SVG writer nor the SPString model.
        auto *text = xml_doc->createTextNode(label_content(doc, kind, result.valid ? result.value_uu_or_deg : 0.0, opts).c_str());
        tspan->addChild(text, nullptr);
        Inkscape::GC::release(text);
        text_repr->appendChild(tspan);
        Inkscape::GC::release(tspan);
    }

    group_repr->appendChild(path_repr);
    Inkscape::GC::release(path_repr);
    group_repr->appendChild(text_repr);
    Inkscape::GC::release(text_repr);

    parent->getRepr()->appendChild(group_repr);
    Inkscape::GC::release(group_repr);

    // Build the objects so ids are assigned and the effect has evaluated.
    doc->ensureUpToDate();

    // The satellite refs must be written only once the path is a registered
    // user of the effect: SatelliteParam::param_readSVGValue drops hrefs
    // that arrive before any LPE item exists (document-load order guard).
    bool const angular = kind == Inkscape::LivePathEffect::Dimension::Kind::Angular;
    std::string persisted_refs;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].id.empty()) {
            continue;
        }
        // Angular refs arrive as A, V, B: the effect stores arm A as
        // start_ref, the vertex as vertex_ref and arm B as end_ref. Every
        // other kind measures from start_ref to end_ref.
        char const *ref_key = i == 0 ? "start_ref" : (angular && i == 1 ? "vertex_ref" : "end_ref");
        char const *anchor_key = i == 0 ? "start_anchor" : (angular && i == 1 ? "vertex_anchor" : "end_anchor");
        lpe_repr->setAttribute(ref_key, ("#" + refs[i].id).c_str());
        lpe_repr->setAttribute(anchor_key, refs[i].anchor.c_str());
        persisted_refs += std::string(ref_key) + "=#" + refs[i].id + ";";
    }
    // A forked or reloaded effect can lose the satellite hrefs (upstream
    // param_readSVGValue clears them when the effect is not applied yet), so
    // persist the ids for the effect to restore them.
    lpe_repr->setAttribute("ref_ids", persisted_refs.c_str());
    doc->ensureUpToDate();

    char const *group_id = group_repr->attribute("id");
    DocumentUndo::done(doc, RC_("Undo", "Add dimension"), INKSCAPE_ICON("measure-segments"));
    doc->ensureUpToDate();

    // The undo commit re-writes the effect repr, which discards and
    // recreates the Effect instance; arm the fresh one for live updates.
    auto *created_path = dimension_path(group_id ? doc->getObjectById(group_id) : nullptr);
    if (created_path) {
        if (auto *effect = created_path->getFirstPathEffectOfType(
                static_cast<int>(Inkscape::LivePathEffect::EffectType::DIMENSION))) {
            if (auto *dimension = dynamic_cast<LPEDim *>(effect)) {
                dimension->arm_live_updates();
            }
        }
    }

    return group_id ? cast<SPGroup>(doc->getObjectById(group_id)) : nullptr;
}

std::vector<Row> list(SPDocument *doc)
{
    std::vector<Row> rows;
    if (!doc || !doc->getRoot()) {
        return rows;
    }
    doc->ensureUpToDate();
    std::vector<SPGroup *> groups;
    collect_dimensions(doc->getRoot(), groups);
    for (auto *group : groups) {
        // A dimension loaded from a file is not evaluated until something
        // changes; evaluate it here so the listed value is current (the
        // effect also arms its live-update watchers).
        if (auto *path = dimension_path(group)) {
            sp_lpe_item_update_patheffect(path, false, false, true);
        }
        Row row;
        auto const *id = group->getId();
        row.id = id ? id : "-";
        if (auto *effect = dimension_effect(group)) {
            row.kind = Inkscape::LivePathEffect::Dimension::kind_key(effect->dimension_kind());
            row.value = effect->value();
        } else {
            row.kind = "-";
        }
        row.label = label_text(group);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string list_tsv(SPDocument *doc)
{
    std::string out;
    for (auto const &row : list(doc)) {
        out += row.id;
        out += '\t';
        out += row.kind;
        out += '\t';
        out += num_fixed(row.value, 6);
        out += '\t';
        out += row.label;
        out += '\n';
    }
    return out;
}

bool set(SPGroup *group, std::string const &key, std::string const &value)
{
    auto fail = [](std::string const &reason) {
        g_warning("dimension-set: %s", reason.c_str());
        return false;
    };
    if (!group) {
        return fail("no dimension group");
    }
    auto *path = dimension_path(group);
    if (!path) {
        return fail("'" + std::string(group->getId() ? group->getId() : "-") + "' is not a dimension group");
    }
    auto *effect = path->getFirstPathEffectOfType(static_cast<int>(Inkscape::LivePathEffect::EffectType::DIMENSION));
    if (!effect || !effect->getLPEObj()) {
        return fail("dimension group has no dimension effect");
    }
    auto *lpe_repr = effect->getLPEObj()->getRepr();

    std::string attr;
    std::string out;
    std::string error;
    if (!set_option_attr(key, value, attr, out, error)) {
        return fail(error);
    }

    lpe_repr->setAttribute(attr, out);
    // Force the effect to re-read its parameters and re-evaluate.
    sp_lpe_item_update_patheffect(path, false, true, true);
    if (auto *dimension = dynamic_cast<LPEDim *>(effect)) {
        dimension->arm_live_updates();
    }
    return true;
}

} // namespace DimensionOps

std::string dimension_list(SPDocument *doc)
{
    return DimensionOps::list_tsv(doc);
}

} // namespace Inkscape

namespace {

const Glib::ustring SECTION = NC_("Action Section", "Dimension");

std::vector<std::vector<Glib::ustring>> raw_data_dimension = {
    // clang-format off
    {"app.dimension-add",  N_("Add Dimension"),  SECTION, N_("Add a live dimension: <kind|auto>,<ref>,<ref>[,<ref>][,key=value]*")},
    {"app.dimension-list", N_("List Dimensions"), SECTION, N_("Print one line per dimension group: id, kind, value and label")},
    {"app.dimension-set",  N_("Change Dimension"), SECTION, N_("Change options on a dimension group: <group-id>,key=value[,key=value]*")},
    // clang-format on
};

void dimension_add(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *doc = active_document(app, "dimension-add");
    if (!doc) {
        return;
    }
    std::string const arg = string_param(value);
    Spec spec;
    std::string error;
    if (!parse_add_arg(arg, spec, error)) {
        g_warning("dimension-add: %s", error.c_str());
        return;
    }
    // The action hosts the dimension next to the measured geometry (or at
    // the root); the interactive tool passes the current layer instead.
    Inkscape::DimensionOps::create(doc, spec, nullptr);
}

void dimension_list_action(InkscapeApplication *app)
{
    auto *doc = active_document(app, "dimension-list");
    if (!doc) {
        return;
    }
    std::cout << Inkscape::dimension_list(doc);
    std::cout.flush();
}

void dimension_set(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *doc = active_document(app, "dimension-set");
    if (!doc) {
        return;
    }
    std::string const arg = string_param(value);
    auto const comma = arg.find(',');
    if (comma == std::string::npos) {
        g_warning("dimension-set: expected '<group-id>,key=value[,key=value]*'");
        return;
    }
    std::string const id = trim(arg.substr(0, comma));
    SPObject *obj = id.empty() ? nullptr : doc->getObjectById(id);
    auto *group = cast<SPGroup>(obj);
    if (!group) {
        g_warning("dimension-set: no dimension group with id '%s'", id.c_str());
        return;
    }
    if (char const *cls = group->getRepr()->attribute("class")) {
        if (!std::strstr(cls, "rosepen-dimension")) {
            g_warning("dimension-set: object '%s' is not a dimension group", id.c_str());
            return;
        }
    }

    // Validate every option first: a bad option must leave the document
    // untouched, never half-applied outside an undo step.
    std::vector<std::pair<std::string, std::string>> options;
    for (auto const &field : split(arg.substr(comma + 1), ',')) {
        auto const eq = field.find('=');
        if (eq == std::string::npos) {
            g_warning("dimension-set: bad option '%s' (expected key=value)", field.c_str());
            return;
        }
        std::string const key = trim(field.substr(0, eq));
        std::string const key_value = trim(field.substr(eq + 1));
        std::string attr, out, error;
        if (!set_option_attr(key, key_value, attr, out, error)) {
            g_warning("dimension-set: %s", error.c_str());
            return;
        }
        options.emplace_back(key, key_value);
    }

    bool ok = true;
    for (auto const &[key, key_value] : options) {
        if (!Inkscape::DimensionOps::set(group, key, key_value)) {
            ok = false;
            break;
        }
    }
    if (ok) {
        doc->ensureUpToDate();
        DocumentUndo::done(doc, RC_("Undo", "Change dimension"), INKSCAPE_ICON("measure-segments"));
    }
}

} // namespace

void add_actions_dimension(InkscapeApplication *app)
{
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    // clang-format off
    gapp->add_action_with_parameter("dimension-add",  String, sigc::bind(sigc::ptr_fun(&dimension_add), app));
    gapp->add_action(                "dimension-list",          sigc::bind(sigc::ptr_fun(&dimension_list_action), app));
    gapp->add_action_with_parameter("dimension-set",  String, sigc::bind(sigc::ptr_fun(&dimension_set), app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_dimension);
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
