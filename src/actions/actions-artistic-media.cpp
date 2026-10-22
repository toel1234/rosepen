// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for live artistic media strokes, application level and headless.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "actions-artistic-media.h"

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
#include "live_effects/artistic-media-geometry.h"
#include "live_effects/artistic-media-library.h"
#include "live_effects/effect-enum.h"
#include "live_effects/lpe-artistic-media.h"
#include "object/object-set.h"
#include "object/sp-defs.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-lpe-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h"
#include "ui/icon-names.h"
#include "util/units.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace {

using Inkscape::ArtisticOps::Options;
using Inkscape::ArtisticOps::Row;
using Inkscape::ArtisticOps::Verb;
using Inkscape::LivePathEffect::ArtisticMedia::Mode;
using Inkscape::LivePathEffect::LPEArtisticMedia;
namespace AM = Inkscape::LivePathEffect::ArtisticMedia;

/// What an artistic media object looks like in the UI.
constexpr char const *ARTISTIC_LABEL = "Artistic Media";
constexpr char const *ARTISTIC_CLASS = "rosepen-artistic";

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

double doc_scale(SPDocument *doc)
{
    return doc ? std::max(doc->getDocumentScale()[Geom::X], 1e-9) : 1.0;
}

/// Pixels per path-local user unit (i2doc includes the document scale).
double i2doc_scale(SPItem const *item)
{
    double const factor = item ? item->i2doc_affine().descrim() : 1.0;
    return std::isfinite(factor) && factor > 0.0 ? factor : 1.0;
}

/// Millimetres per path-local user unit.
double mm_per_local_uu(SPItem const *item)
{
    return i2doc_scale(item) / AM::kPxPerMm;
}

/// STORED width parameter (path-local user units) for a visual width in mm.
double mm_to_local(SPItem const *item, double mm)
{
    return mm / mm_per_local_uu(item);
}

/// Visual millimetres of a length given in path-local user units.
double local_to_mm(SPItem const *item, double local)
{
    return local * mm_per_local_uu(item);
}

std::string object_id(SPObject const *object)
{
    auto const *id = object ? object->getId() : nullptr;
    return id ? std::string(id) : std::string("-");
}

std::string unique_id(SPDocument *doc, std::string const &base)
{
    if (!doc || !doc->getObjectById(base.c_str())) {
        return base;
    }
    for (int i = 2; i < 10000; ++i) {
        std::string const candidate = base + "-" + std::to_string(i);
        if (!doc->getObjectById(candidate.c_str())) {
            return candidate;
        }
    }
    return base + "-x";
}

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

bool verb_allows(Verb verb, std::string const &key)
{
    switch (verb) {
        case Verb::Add:
            return key == "mode" || key == "shape" || key == "width" || key == "angle" ||
                   key == "scale-with-object" || key == "color" || key == "smoothing" || key == "id" ||
                   key == "parent";
        case Verb::Apply:
            return key == "mode" || key == "shape" || key == "width" || key == "angle" ||
                   key == "scale-with-object" || key == "color";
        case Verb::Set:
            return key == "mode" || key == "shape" || key == "width" || key == "angle" ||
                   key == "scale-with-object" || key == "skeleton";
    }
    return false;
}

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

/// "preset" / "brush" for the unknown-shape message of @a mode.
char const *shape_kind_word(Mode mode)
{
    return mode == Mode::Brush ? "brush" : "preset";
}

bool check_shape_name(Mode mode, std::string const &name, std::string &error)
{
    if (mode == Mode::Calligraphic) {
        error = "shape is not used by calligraphic";
        return false;
    }
    if (!AM::find(mode, name)) {
        error = std::string("unknown ") + shape_kind_word(mode) + " '" + name + "'";
        return false;
    }
    return true;
}

} // namespace

namespace Inkscape {

namespace ArtisticOps {

std::optional<Options> parse_options(std::string_view spec, Verb verb, std::string &error)
{
    Options options;
    std::string const text(spec);

    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find(',', begin);
        std::string token = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        auto const eq = token.find('=');
        if (eq == std::string::npos) {
            error = "bad option '" + token + "' (expected key=value)";
            return std::nullopt;
        }
        std::string const key = trim(token.substr(0, eq));
        std::string value = trim(token.substr(eq + 1));

        if (!verb_allows(verb, key)) {
            error = "unknown option '" + key + "'";
            return std::nullopt;
        }

        if (key == "skeleton") {
            // Takes the rest of the string, commas included.
            value = text.substr(begin + eq + 1);
            options.skeleton = value;
            break;
        }

        double number = 0.0;
        bool flag = false;
        if (key == "mode") {
            auto const mode = AM::parse_mode(value);
            if (!mode) {
                error = "unknown mode '" + value + "'";
                return std::nullopt;
            }
            options.mode = *mode;
        } else if (key == "shape") {
            options.shape = value;
        } else if (key == "width" || key == "angle" || key == "smoothing") {
            if (!parse_number(value, number)) {
                error = "bad number '" + value + "'";
                return std::nullopt;
            }
            (key == "width" ? options.width : key == "angle" ? options.angle : options.smoothing) = number;
        } else if (key == "scale-with-object") {
            if (!parse_bool_value(value, flag)) {
                error = "bad boolean '" + value + "'";
                return std::nullopt;
            }
            options.scale_with_object = flag;
        } else if (key == "color") {
            if (!valid_color(value)) {
                error = "bad color '" + value + "' (expected #rrggbb)";
                return std::nullopt;
            }
            options.color = value;
        } else if (key == "id") {
            if (value.empty() || value.find_first_of(" \t\r\n") != std::string::npos) {
                error = "bad id '" + value + "'";
                return std::nullopt;
            }
            options.id = value;
        } else if (key == "parent") {
            if (value.empty()) {
                error = "bad parent id ''";
                return std::nullopt;
            }
            options.parent = value;
        }

        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }

    if (options.width && !(*options.width > 0.0 && *options.width <= 1000.0)) {
        error = "width must be > 0 and <= 1000 mm";
        return std::nullopt;
    }
    // A shape name is checked against the library when the mode is part of the
    // same string; otherwise the operation checks it against the object's mode.
    if (options.shape && options.mode) {
        if (!check_shape_name(*options.mode, *options.shape, error)) {
            return std::nullopt;
        }
    }
    return options;
}

// ---------------------------------------------------------------------------
// Effect access
// ---------------------------------------------------------------------------

namespace {

LPEArtisticMedia *artistic_effect(SPPath *path)
{
    if (!path) {
        return nullptr;
    }
    auto *effect = path->getFirstPathEffectOfType(
        static_cast<int>(Inkscape::LivePathEffect::EffectType::ARTISTIC_MEDIA));
    return dynamic_cast<LPEArtisticMedia *>(effect);
}

Inkscape::XML::Node *effect_repr(SPPath *path)
{
    auto *effect = artistic_effect(path);
    if (!effect) {
        return nullptr;
    }
    auto *lpeobj = effect->getLPEObj();
    return lpeobj ? lpeobj->getRepr() : nullptr;
}

/// The skeleton curve: the path's original-d.
Geom::PathVector skeleton_of(SPPath const *path)
{
    if (!path) {
        return {};
    }
    if (auto const *curve = path->curveBeforeLPE()) {
        return *curve;
    }
    if (auto const *d = path->getAttribute("inkscape:original-d")) {
        return sp_svg_read_pathv(d);
    }
    return {};
}

Mode stored_mode(Inkscape::XML::Node *lpe_repr)
{
    if (lpe_repr) {
        if (auto const *value = lpe_repr->attribute("mode")) {
            if (auto const mode = AM::parse_mode(value)) {
                return *mode;
            }
        }
    }
    return Mode::Preset;
}

std::string stored_shape(Inkscape::XML::Node *lpe_repr)
{
    if (lpe_repr) {
        if (auto const *value = lpe_repr->attribute("shape")) {
            return value;
        }
    }
    return {};
}

bool stored_bool(Inkscape::XML::Node *lpe_repr, char const *key, bool fallback)
{
    if (lpe_repr) {
        if (auto const *value = lpe_repr->attribute(key)) {
            bool flag = fallback;
            if (parse_bool_value(value, flag)) {
                return flag;
            }
        }
    }
    return fallback;
}

double stored_number(Inkscape::XML::Node *lpe_repr, char const *key, double fallback)
{
    if (lpe_repr) {
        if (auto const *value = lpe_repr->attribute(key)) {
            double number = fallback;
            if (parse_number(value, number)) {
                return number;
            }
        }
    }
    return fallback;
}

/// The effective shape for a mode/option pair, or empty for calligraphic.
std::string resolve_shape_name(Mode mode, std::optional<std::string> const &given, std::string const &current,
                               bool mode_changed)
{
    if (mode == Mode::Calligraphic) {
        return {};
    }
    if (given) {
        return *given;
    }
    if (mode_changed || current.empty()) {
        auto const *fallback = AM::default_shape(mode);
        return fallback ? fallback : std::string();
    }
    return current;
}

/// Write the effect parameters; the caller has validated everything.
void write_effect_params(Inkscape::XML::Node *lpe_repr, Mode mode, std::string const &shape, double width_uu,
                         double angle_deg, bool scale_with_object, bool rewrite_shape_d)
{
    lpe_repr->setAttribute("mode", AM::mode_key(mode));
    lpe_repr->setAttribute("shape", shape.c_str());
    lpe_repr->setAttribute("width", num(width_uu).c_str());
    lpe_repr->setAttribute("angle", num(AM::normalize_angle(angle_deg)).c_str());
    lpe_repr->setAttribute("scale_with_object", scale_with_object ? "true" : "false");
    if (rewrite_shape_d) {
        if (auto const *entry = AM::find(mode, shape)) {
            lpe_repr->setAttribute("shape_d", entry->unit_d);
        } else {
            lpe_repr->removeAttribute("shape_d");
        }
    }
}

/// Create the inkscape:path-effect element in <defs>; returns its id.
std::string create_effect(SPDocument *doc, std::string const &requested_id, Mode mode, std::string const &shape,
                          double width_uu, double angle_deg, bool scale_with_object)
{
    auto *xml_doc = doc->getReprDoc();
    auto *lpe_repr = xml_doc->createElement("inkscape:path-effect");
    lpe_repr->setAttribute("effect", "artistic_media");
    lpe_repr->setAttribute("is_visible", "true");
    lpe_repr->setAttribute("lpeversion", "1");
    write_effect_params(lpe_repr, mode, shape, width_uu, angle_deg, scale_with_object, true);
    if (!requested_id.empty()) {
        lpe_repr->setAttribute("id", requested_id.c_str());
    }
    doc->getDefs()->getRepr()->appendChild(lpe_repr);
    Inkscape::GC::release(lpe_repr);

    if (char const *assigned = lpe_repr->attribute("id")) {
        return assigned;
    }
    // The document assigns ids when it builds the object; fall back to a
    // generated one when that has not happened (detached documents).
    std::string const generated = unique_id(doc, "artistic-media-lpe");
    lpe_repr->setAttribute("id", generated.c_str());
    return generated;
}

/// Structure is authoritative; the class and label are only UI hints.
void mark_artistic(SPPath *path)
{
    if (!path->getAttribute("class")) {
        path->setAttribute("class", ARTISTIC_CLASS);
    }
    if (!path->getAttribute("inkscape:label")) {
        path->setAttribute("inkscape:label", ARTISTIC_LABEL);
    }
}

/// Build the brush outline for a skeleton with the given parameters.
Geom::PathVector outline_for(Mode mode, std::string const &shape, Geom::PathVector const &skeleton,
                             double width_uu, double angle_deg)
{
    if (mode == Mode::Calligraphic) {
        return AM::calligraphic(skeleton, width_uu, angle_deg).path;
    }
    auto const *entry = AM::find(mode, shape);
    if (!entry) {
        return {};
    }
    auto unit_shape = AM::parse_unit_shape(entry->unit_d);
    if (!unit_shape) {
        return {};
    }
    return AM::map_along(skeleton, *unit_shape, width_uu).path;
}

/// Collect artistic media paths in document order (layers, groups, clip/mask contents).
void collect_artistic(SPObject *object, std::vector<SPPath *> &out)
{
    for (auto *child : object->childList(false)) {
        if (auto *path = cast<SPPath>(child)) {
            if (path->getFirstPathEffectOfType(
                    static_cast<int>(Inkscape::LivePathEffect::EffectType::ARTISTIC_MEDIA))) {
                out.push_back(path);
            }
        }
        collect_artistic(child, out);
    }
}

} // namespace

bool is_artistic(SPObject const *object)
{
    auto const *path = cast<SPPath>(object);
    return path && path->getFirstPathEffectOfType(
                       static_cast<int>(Inkscape::LivePathEffect::EffectType::ARTISTIC_MEDIA));
}

std::vector<Row> list(SPDocument *doc)
{
    std::vector<Row> rows;
    if (!doc || !doc->getRoot()) {
        return rows;
    }
    doc->ensureUpToDate();

    std::vector<SPPath *> paths;
    collect_artistic(doc->getRoot(), paths);

    for (auto *path : paths) {
        // A stroke loaded from a file is not evaluated until something
        // changes; evaluate it so the listed values are current (and so a
        // load-time shape repair happens here too).
        sp_lpe_item_update_patheffect(path, false, false, true);

        Row row;
        row.id = object_id(path);
        Mode const mode = stored_mode(effect_repr(path));
        auto *effect = artistic_effect(path);
        row.mode = AM::mode_key(mode);
        row.shape = mode == Mode::Calligraphic ? "-" : stored_shape(effect_repr(path));
        if (row.shape.empty() && mode != Mode::Calligraphic) {
            row.shape = "-";
        }

        double const width_uu = stored_number(effect_repr(path), "width", 10.0);
        row.width = local_to_mm(path, width_uu);
        row.angle = AM::normalize_angle(stored_number(effect_repr(path), "angle", 30.0));

        auto const skeleton = skeleton_of(path);
        row.nodes = AM::node_count(skeleton);
        row.length = local_to_mm(path, AM::skeleton_length(skeleton));

        if (mode == Mode::Calligraphic) {
            row.source = "-";
        } else {
            bool const fallback = effect ? effect->shape_is_fallback() : false;
            row.source = fallback ? "fallback" : "embedded";
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string list_tsv(SPDocument *doc)
{
    std::string out;
    auto const rows = list(doc);
    for (auto const &row : rows) {
        out += row.id;
        out += '\t';
        out += row.mode;
        out += '\t';
        out += row.shape;
        out += '\t';
        out += num_fixed(row.width, 6);
        out += '\t';
        out += num_fixed(row.angle, 6);
        out += '\t';
        out += std::to_string(row.nodes);
        out += '\t';
        out += num_fixed(row.length, 6);
        out += '\t';
        out += row.source;
        out += '\n';
    }
    out += "artistic-count " + std::to_string(rows.size()) + "\n";
    return out;
}

std::string library_tsv()
{
    std::string out;
    for (auto const &entry : AM::library()) {
        out += AM::mode_key(entry.mode);
        out += '\t';
        out += entry.name;
        out += '\t';
        out += _(entry.label);
        out += '\n';
    }
    return out;
}

SPPath *create(SPDocument *doc, std::vector<Geom::Point> const &points_doc, Options const &options,
               SPObject *parent_or_null, std::string &error)
{
    auto fail = [&error](std::string const &reason) -> SPPath * {
        error = "artistic-add: " + reason;
        return nullptr;
    };
    if (!doc || !doc->getRoot()) {
        return fail("no document");
    }

    Mode const mode = options.mode.value_or(Mode::Preset);
    std::string const shape = resolve_shape_name(mode, options.shape, {}, true);
    if (mode != Mode::Calligraphic) {
        std::string shape_error;
        if (!check_shape_name(mode, shape, shape_error)) {
            return fail(shape_error);
        }
    }

    // Parent: explicit group, else the last top-level layer, else the root.
    SPObject *parent = parent_or_null;
    if (parent) {
        if (!cast<SPGroup>(parent)) {
            return fail("parent '" + object_id(parent) + "' is not a group");
        }
    } else if (!options.parent.empty()) {
        parent = doc->getObjectById(options.parent.c_str());
        if (!cast<SPGroup>(parent)) {
            return fail("parent '" + options.parent + "' is not a group");
        }
    } else {
        for (auto *child : doc->getRoot()->childList(false)) {
            if (cast<SPGroup>(child) && g_strcmp0(child->getAttribute("inkscape:groupmode"), "layer") == 0) {
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
    Geom::Affine const doc2parent = parent_item->i2doc_affine().inverse();

    std::vector<Geom::Point> points;
    points.reserve(points_doc.size());
    for (auto const &p : points_doc) {
        // Action points are document user units; the path lives in its
        // parent's user units (i2doc includes the document scale, hence c2p).
        points.push_back(p * doc->getRoot()->c2p * doc2parent);
    }
    double const smoothing = options.smoothing.value_or(50.0);
    double const tolerance_uu = mm_to_local(parent_item, AM::smoothing_to_tolerance_mm(smoothing));
    auto const skeleton = AM::fit_stroke(points, tolerance_uu);
    if (!skeleton) {
        return fail("needs at least two distinct points");
    }

    if (!options.id.empty()) {
        for (auto const &derived : {options.id, options.id + "-lpe"}) {
            if (doc->getObjectById(derived.c_str())) {
                return fail("id '" + derived + "' already in use");
            }
        }
    }

    double const width_uu = mm_to_local(parent_item, options.width.value_or(10.0));
    double const angle = AM::normalize_angle(options.angle.value_or(30.0));
    bool const scale_with_object = options.scale_with_object.value_or(true);
    std::string const color = options.color.value_or(std::string("#000000"));

    auto const outline = outline_for(mode, shape, *skeleton, width_uu, angle);

    // The path element: skeleton in original-d, brush outline in d.
    auto *xml_doc = doc->getReprDoc();
    auto *path_repr = xml_doc->createElement("svg:path");
    if (!options.id.empty()) {
        path_repr->setAttribute("id", options.id.c_str());
    }
    path_repr->setAttribute("inkscape:label", ARTISTIC_LABEL);
    path_repr->setAttribute("class", ARTISTIC_CLASS);
    path_repr->setAttribute("style", ("fill:none;stroke:" + color + ";stroke-width:1").c_str());
    std::string const skeleton_d = sp_svg_write_path(*skeleton);
    path_repr->setAttribute("d", sp_svg_write_path(outline).c_str());
    parent->getRepr()->appendChild(path_repr);
    Inkscape::GC::release(path_repr);
    doc->ensureUpToDate();

    // The skeleton is written once the object exists: a pre-build
    // inkscape:original-d is dropped while the node is adopted by the tree
    // (the writer re-creates the element), and the effect would then treat the
    // brush outline as its skeleton.
    char const *created_id = path_repr->attribute("id");
    auto *path = created_id ? cast<SPPath>(doc->getObjectById(created_id)) : nullptr;
    if (!path) {
        return fail("could not create the path");
    }
    path->setAttribute("inkscape:original-d", skeleton_d.c_str());
    doc->ensureUpToDate();

    std::string const effect_id = options.id.empty() ? std::string() : options.id + "-lpe";
    std::string const lpe_id = create_effect(doc, effect_id, mode, shape, width_uu, angle, scale_with_object);
    doc->ensureUpToDate();
    path->addPathEffect("#" + lpe_id, false);
    doc->ensureUpToDate();

    DocumentUndo::done(doc, RC_("Undo", "Add artistic media stroke"), INKSCAPE_ICON("taper-stroke"));
    doc->ensureUpToDate();

    return cast<SPPath>(doc->getObjectById(created_id));
}

bool set(SPPath *path, Options const &options, std::string &error)
{
    if (!is_artistic(path)) {
        error = "artistic-set: no artistic media object '" + object_id(path) + "'";
        return false;
    }
    auto *doc = path->document;

    auto *lpe_repr = effect_repr(path);
    if (!lpe_repr) {
        error = "artistic-set: no artistic media effect on '" + object_id(path) + "'";
        return false;
    }

    Mode const current_mode = stored_mode(lpe_repr);
    std::string const current_shape = stored_shape(lpe_repr);
    Mode const mode = options.mode.value_or(current_mode);
    bool const mode_changed = mode != current_mode;
    std::string const shape = resolve_shape_name(mode, options.shape, current_shape, mode_changed);

    // Validate everything before any write.
    if (mode != Mode::Calligraphic) {
        std::string shape_error;
        if (!check_shape_name(mode, shape, shape_error)) {
            error = "artistic-set: " + shape_error;
            return false;
        }
    }
    Geom::PathVector parsed_skeleton;
    if (options.skeleton) {
        parsed_skeleton = sp_svg_read_pathv(options.skeleton->c_str());
        if (parsed_skeleton.empty()) {
            error = "artistic-set: bad skeleton path data";
            return false;
        }
    }

    double width_uu = stored_number(lpe_repr, "width", 10.0);
    if (options.width) {
        width_uu = mm_to_local(path, *options.width);
    }
    double const angle = options.angle ? *options.angle : stored_number(lpe_repr, "angle", 30.0);
    bool const scale_with_object = options.scale_with_object.value_or(stored_bool(lpe_repr, "scale_with_object", true));
    bool const rewrite_shape_d = mode_changed || options.shape.has_value() ||
                                 (mode != Mode::Calligraphic && lpe_repr->attribute("shape_d") == nullptr);

    // Everything is validated: only now fork a shared effect (a write) so
    // duplicates never share the change, and re-fetch its repr.
    path->forkPathEffectsIfNecessary(1);
    doc->ensureUpToDate();
    lpe_repr = effect_repr(path);
    if (!lpe_repr) {
        error = "artistic-set: no artistic media effect on '" + object_id(path) + "'";
        return false;
    }

    write_effect_params(lpe_repr, mode, shape, width_uu, angle, scale_with_object, rewrite_shape_d);
    if (options.skeleton) {
        path->setAttribute("inkscape:original-d", sp_svg_write_path(parsed_skeleton).c_str());
    }
    mark_artistic(path);

    sp_lpe_item_update_patheffect(path, false, true, true);
    doc->ensureUpToDate();
    return true;
}

int apply(ObjectSet &selection, Options const &options, std::string &errors)
{
    std::vector<SPObject *> const selected(selection.objects().begin(), selection.objects().end());
    int applied = 0;
    for (auto *object : selected) {
        auto *path = cast<SPPath>(object);
        if (!path) {
            errors += "artistic-apply: " + object_id(object) + " is not a path\n";
            continue;
        }
        if (is_artistic(path)) {
            std::string error;
            if (!set(path, options, error)) {
                // Report under the verb the user ran.
                constexpr std::string_view set_prefix = "artistic-set: ";
                if (error.starts_with(set_prefix)) {
                    error = "artistic-apply: " + error.substr(set_prefix.size());
                }
                errors += error + "\n";
            } else {
                if (options.color) {
                    // An existing stroke is already converted: the colour is its fill.
                    SPCSSAttr *css = sp_repr_css_attr(path->getRepr(), "style");
                    sp_repr_css_set_property(css, "fill", options.color->c_str());
                    sp_repr_css_set(path->getRepr(), css, "style");
                    sp_repr_css_attr_unref(css);
                }
                ++applied;
            }
            continue;
        }

        auto *doc = path->document;
        Mode const mode = options.mode.value_or(Mode::Preset);
        std::string const shape = resolve_shape_name(mode, options.shape, {}, true);
        std::string shape_error;
        if (mode != Mode::Calligraphic && !check_shape_name(mode, shape, shape_error)) {
            errors += "artistic-apply: " + shape_error + "\n";
            continue;
        }

        if (options.color) {
            // The stroke becomes the brush fill in doOnApply; only the paint
            // property is touched so the rest of the style survives.
            SPCSSAttr *css = sp_repr_css_attr(path->getRepr(), "style");
            sp_repr_css_set_property(css, "stroke", options.color->c_str());
            sp_repr_css_set(path->getRepr(), css, "style");
            sp_repr_css_attr_unref(css);
        }

        double const width_uu = mm_to_local(path, options.width.value_or(10.0));
        double const angle = options.angle.value_or(30.0);
        bool const scale_with_object = options.scale_with_object.value_or(true);

        std::string const effect_id = std::string(path->getId() ? path->getId() : "") + "-artistic";
        std::string const wanted = unique_id(doc, effect_id);
        std::string const lpe_id = create_effect(doc, wanted, mode, shape, width_uu, angle, scale_with_object);
        doc->ensureUpToDate();
        path->addPathEffect("#" + lpe_id, false);
        mark_artistic(path);
        doc->ensureUpToDate();
        ++applied;
    }
    return applied;
}

std::vector<SPPath *> break_apart(ObjectSet &selection)
{
    std::vector<SPPath *> targets;
    std::vector<SPObject *> const selected(selection.objects().begin(), selection.objects().end());
    for (auto *object : selected) {
        auto *path = cast<SPPath>(object);
        if (path && is_artistic(path)) {
            targets.push_back(path);
        }
    }
    if (targets.empty()) {
        return targets;
    }

    auto *doc = targets.front()->document;
    for (auto *path : targets) {
        auto const *skeleton_d = path->getAttribute("inkscape:original-d");
        std::string const skeleton = skeleton_d ? skeleton_d : "";
        auto const *transform_attr = path->getAttribute("transform");
        std::string const transform = transform_attr ? transform_attr : "";

        if (auto const *cls = path->getAttribute("class")) {
            std::string remaining(cls);
            auto const pos = remaining.find(ARTISTIC_CLASS);
            if (pos != std::string::npos) {
                remaining.erase(pos, std::strlen(ARTISTIC_CLASS));
                remaining = trim(remaining);
                // Collapse a doubled separator left in the middle.
                for (std::size_t i = 1; i + 1 < remaining.size();) {
                    if (remaining[i] == ' ' && remaining[i - 1] == ' ') {
                        remaining.erase(i, 1);
                    } else {
                        ++i;
                    }
                }
                if (remaining.empty()) {
                    path->removeAttribute("class");
                } else {
                    path->setAttribute("class", remaining.c_str());
                }
            }
        }

        // Keep the outline: freeze the effect without reverting the style.
        path->removeAllPathEffects(true);
        path->removeAttribute("inkscape:original-d");

        if (!skeleton.empty()) {
            auto *xml_doc = doc->getReprDoc();
            auto *skeleton_repr = xml_doc->createElement("svg:path");
            std::string const base = object_id(path) + "-skeleton";
            skeleton_repr->setAttribute("id", unique_id(doc, base).c_str());
            skeleton_repr->setAttribute("d", skeleton.c_str());
            // The skeleton is in the stroke's own coordinates, and an LPE path
            // keeps its transform attribute: the skeleton must carry it too.
            if (!transform.empty()) {
                skeleton_repr->setAttribute("transform", transform.c_str());
            }
            skeleton_repr->setAttribute("style", "fill:none;stroke:none");
            path->getRepr()->parent()->addChild(skeleton_repr, path->getRepr());
            Inkscape::GC::release(skeleton_repr);
        }
    }
    doc->ensureUpToDate();
    return targets;
}

} // namespace ArtisticOps

} // namespace Inkscape

// ---------------------------------------------------------------------------
// Gio actions
// ---------------------------------------------------------------------------

namespace {

const Glib::ustring SECTION = NC_("Action Section", "Artistic Media");

std::vector<std::vector<Glib::ustring>> raw_data_artistic_media = {
    // clang-format off
    {"app.artistic-add",     N_("Add Artistic Media Stroke"),    SECTION, N_("Draw an artistic media stroke: <x1> <y1> <x2> <y2> ... [,key=value]*")},
    {"app.artistic-apply",   N_("Apply Artistic Media"),         SECTION, N_("Turn the selected paths into artistic media strokes: key=value[,key=value]*")},
    {"app.artistic-set",     N_("Change Artistic Media"),        SECTION, N_("Change an artistic media object: <id>,key=value[,key=value]*")},
    {"app.artistic-list",    N_("List Artistic Media"),          SECTION, N_("Print one line per artistic media object")},
    {"app.artistic-break",   N_("Break Artistic Media Apart"),   SECTION, N_("Freeze the selected strokes into an outline plus its skeleton")},
    {"app.artistic-library", N_("List Artistic Media Library"),  SECTION, N_("Print the built-in presets and brushes")},
    // clang-format on
};

void report_error(std::string const &error)
{
    std::size_t begin = 0;
    while (begin < error.size()) {
        auto const end = error.find('\n', begin);
        std::string const line =
            error.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.empty()) {
            g_warning("%s", line.c_str());
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

void artistic_add(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *doc = active_document(app, "artistic-add");
    if (!doc) {
        return;
    }
    std::string const arg = string_param(value);
    auto const comma = arg.find(',');
    std::string const points_part = comma == std::string::npos ? arg : arg.substr(0, comma);
    std::string const options_part = comma == std::string::npos ? std::string() : arg.substr(comma + 1);

    std::vector<Geom::Point> points;
    {
        std::size_t begin = 0;
        std::vector<double> numbers;
        while (begin < points_part.size()) {
            auto const end = points_part.find_first_of(" \t\r\n", begin);
            std::string const token =
                points_part.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!token.empty()) {
                double number = 0.0;
                if (!parse_number(token, number)) {
                    g_warning("artistic-add: bad number '%s'", token.c_str());
                    return;
                }
                numbers.push_back(number);
            }
            if (end == std::string::npos) {
                break;
            }
            begin = points_part.find_first_not_of(" \t\r\n", end);
            if (begin == std::string::npos) {
                break;
            }
        }
        for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
            points.emplace_back(numbers[i], numbers[i + 1]);
        }
        if (numbers.size() % 2 != 0) {
            g_warning("artistic-add: needs at least two distinct points");
            return;
        }
    }

    Inkscape::ArtisticOps::Options options;
    if (!options_part.empty()) {
        std::string error;
        auto parsed = Inkscape::ArtisticOps::parse_options(options_part, Inkscape::ArtisticOps::Verb::Add, error);
        if (!parsed) {
            g_warning("artistic-add: %s", error.c_str());
            return;
        }
        options = std::move(*parsed);
    }

    std::string error;
    Inkscape::ArtisticOps::create(doc, points, options, nullptr, error);
    if (!error.empty()) {
        g_warning("%s", error.c_str());
    }
}

void artistic_apply(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *selection = app->get_active_selection();
    if (!selection) {
        g_warning("artistic-apply: no active selection");
        return;
    }
    std::string error;
    auto options = Inkscape::ArtisticOps::parse_options(string_param(value), Inkscape::ArtisticOps::Verb::Apply,
                                                        error);
    if (!options) {
        g_warning("artistic-apply: %s", error.c_str());
        return;
    }
    std::string errors;
    int const applied = Inkscape::ArtisticOps::apply(*selection, *options, errors);
    report_error(errors);
    if (applied > 0) {
        auto *doc = selection->document();
        DocumentUndo::done(doc, RC_("Undo", "Apply artistic media"), INKSCAPE_ICON("taper-stroke"));
        doc->ensureUpToDate();
    }
}

void artistic_set(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *doc = active_document(app, "artistic-set");
    if (!doc) {
        return;
    }
    std::string const arg = string_param(value);
    auto const comma = arg.find(',');
    std::string const id = comma == std::string::npos ? std::string() : trim(arg.substr(0, comma));
    std::string const options_part = comma == std::string::npos ? std::string() : arg.substr(comma + 1);

    auto *path = cast<SPPath>(id.empty() ? nullptr : doc->getObjectById(id.c_str()));
    if (!path || !Inkscape::ArtisticOps::is_artistic(path)) {
        g_warning("artistic-set: no artistic media object '%s'", id.c_str());
        return;
    }
    std::string error;
    auto options = Inkscape::ArtisticOps::parse_options(options_part, Inkscape::ArtisticOps::Verb::Set, error);
    if (!options) {
        g_warning("artistic-set: %s", error.c_str());
        return;
    }
    if (!Inkscape::ArtisticOps::set(path, *options, error)) {
        g_warning("%s", error.c_str());
        return;
    }
    doc->ensureUpToDate();
    DocumentUndo::done(doc, RC_("Undo", "Change artistic media"), INKSCAPE_ICON("taper-stroke"));
    doc->ensureUpToDate();
}

void artistic_list_action(InkscapeApplication *app)
{
    auto *doc = active_document(app, "artistic-list");
    if (!doc) {
        return;
    }
    std::cout << Inkscape::ArtisticOps::list_tsv(doc);
    std::cout.flush();
}

void artistic_break(InkscapeApplication *app)
{
    auto *selection = app->get_active_selection();
    if (!selection) {
        g_warning("artistic-break: no artistic media object selected");
        return;
    }
    auto broken = Inkscape::ArtisticOps::break_apart(*selection);
    if (broken.empty()) {
        g_warning("artistic-break: no artistic media object selected");
        return;
    }
    auto *doc = selection->document();
    doc->ensureUpToDate();
    DocumentUndo::done(doc, RC_("Undo", "Break artistic media apart"), INKSCAPE_ICON("taper-stroke"));
    doc->ensureUpToDate();
}

void artistic_library(InkscapeApplication * /*app*/)
{
    std::cout << Inkscape::ArtisticOps::library_tsv();
    std::cout.flush();
}

} // namespace

void add_actions_artistic_media(InkscapeApplication *app)
{
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    // clang-format off
    gapp->add_action_with_parameter("artistic-add",     String, sigc::bind(sigc::ptr_fun(&artistic_add), app));
    gapp->add_action_with_parameter("artistic-apply",   String, sigc::bind(sigc::ptr_fun(&artistic_apply), app));
    gapp->add_action_with_parameter("artistic-set",     String, sigc::bind(sigc::ptr_fun(&artistic_set), app));
    gapp->add_action(                "artistic-list",           sigc::bind(sigc::ptr_fun(&artistic_list_action), app));
    gapp->add_action(                "artistic-break",          sigc::bind(sigc::ptr_fun(&artistic_break), app));
    gapp->add_action(                "artistic-library",        sigc::bind(sigc::ptr_fun(&artistic_library), app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_artistic_media);
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
