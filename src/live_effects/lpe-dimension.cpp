// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live "dimension" path effect: a persistent, live-linked dimension line.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "live_effects/lpe-dimension.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>

#include <2geom/path.h>

#include "document-undo.h"
#include "document.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "object/sp-shape.h"
#include "svg/svg.h"
#include "util/enums.h"
#include "util/units.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape {
namespace LivePathEffect {

using Dimension::Anchor;
using Dimension::Kind;
using Dimension::TextPosition;

namespace {

/// Points closer than this to their cached value count as unchanged.
constexpr double UNCHANGED_EPS = 1e-6;

static const Util::EnumData<DimKindParam> DimKindData[] = {
    { DK_ALIGNED,    NC_("path effect", "Aligned"),    "aligned"    },
    { DK_HORIZONTAL, NC_("path effect", "Horizontal"), "horizontal" },
    { DK_VERTICAL,   NC_("path effect", "Vertical"),   "vertical"   },
    { DK_ANGULAR,    NC_("path effect", "Angular"),    "angular"    },
    { DK_SEGMENT,    NC_("path effect", "Segment"),    "segment"    },
};
static const Util::EnumDataConverter<DimKindParam> DimKindConverter(DimKindData, DK_END);

static const Util::EnumData<DimTextPos> DimTextPosData[] = {
    { DTP_ABOVE,  NC_("path effect", "Above"),  "above"  },
    { DTP_INLINE, NC_("path effect", "Inline"), "inline" },
    { DTP_BELOW,  NC_("path effect", "Below"),  "below"  },
};
static const Util::EnumDataConverter<DimTextPos> DimTextPosConverter(DimTextPosData, DTP_END);

Kind dimension_kind_from_param(DimKindParam v)
{
    switch (v) {
        case DK_HORIZONTAL: return Kind::Horizontal;
        case DK_VERTICAL:   return Kind::Vertical;
        case DK_ANGULAR:    return Kind::Angular;
        case DK_SEGMENT:    return Kind::Segment;
        default:            return Kind::Aligned;
    }
}

TextPosition text_position_from_param(DimTextPos v)
{
    switch (v) {
        case DTP_INLINE: return TextPosition::Inline;
        case DTP_BELOW:  return TextPosition::Below;
        default:         return TextPosition::Above;
    }
}

/// The measured item's shape curve in document coordinates (empty when it has none).
Geom::PathVector item_curve_doc(SPItem const *item)
{
    Geom::PathVector curve;
    auto const *shape = cast<SPShape>(const_cast<SPItem *>(item));
    if (!shape) {
        return curve;
    }
    if (auto const *c = shape->curve()) {
        curve = *c;
        curve *= item->i2doc_affine();
    }
    return curve;
}

/**
 * Node spans of a pathvector, with the same node accounting as
 * Dimension::curve_nodes(): a subpath covers [first, last], where a closed
 * subpath's wrap-around end is not counted twice.
 */
struct NodeSpan {
    std::size_t first = 0;
    std::size_t last = 0;
    bool closed = false;
};

std::vector<NodeSpan> node_spans(Geom::PathVector const &pv)
{
    std::vector<NodeSpan> spans;
    std::size_t base = 0;
    for (auto const &path : pv) {
        if (path.empty()) {
            continue;
        }
        std::size_t segments = path.size();
        bool const closed = path.closed() && segments > 0 &&
                            Geom::are_near(path.curveAt(segments - 1).finalPoint(), path.initialPoint());
        if (closed) {
            --segments;
        }
        NodeSpan span;
        span.first = base;
        span.last = base + segments;
        span.closed = closed;
        spans.push_back(span);
        base = span.last + 1;
    }
    return spans;
}

/// Format one double for SVG attributes (short round-trip form, classic locale).
std::string num(double v)
{
    return sp_svg_number_write_de(v, 8, -4);
}

/// Set an x/y attribute, but only when the value actually changes.
void set_pos_attr(Inkscape::XML::Node *repr, char const *key, double value)
{
    if (char const *old = repr->attribute(key)) {
        if (std::fabs(g_ascii_strtod(old, nullptr) - value) < UNCHANGED_EPS) {
            return;
        }
    }
    repr->setAttributeSvgDouble(key, value);
}

/// The first sibling svg:text carrying the rosepen-dimension-label class.
Inkscape::XML::Node *find_label_repr(Inkscape::XML::Node *parent_repr)
{
    if (!parent_repr) {
        return nullptr;
    }
    for (auto *child = parent_repr->firstChild(); child; child = child->next()) {
        if (std::strcmp(child->name(), "svg:text") != 0) {
            continue;
        }
        if (char const *cls = child->attribute("class")) {
            if (std::strstr(cls, "rosepen-dimension-label")) {
                return child;
            }
        }
    }
    return nullptr;
}

std::vector<Geom::Point> parse_points(Glib::ustring const &value)
{
    std::vector<Geom::Point> points;
    std::string const raw = value.raw();
    std::size_t begin = 0;
    while (begin < raw.size()) {
        std::size_t const sep = raw.find(';', begin);
        std::string const part = raw.substr(begin, sep == std::string::npos ? std::string::npos : sep - begin);
        begin = (sep == std::string::npos) ? raw.size() : sep + 1;
        char *end = nullptr;
        double const x = g_ascii_strtod(part.c_str(), &end);
        if (!end || end == part.c_str() || *end != ',') {
            return {}; // malformed entry: drop the whole cache
        }
        double const y = g_ascii_strtod(end + 1, &end);
        if (!end || *end != '\0' || !std::isfinite(x) || !std::isfinite(y)) {
            return {};
        }
        points.emplace_back(x, y);
    }
    return points;
}

std::string format_points(std::vector<Geom::Point> const &points)
{
    std::string out;
    for (auto const &p : points) {
        if (!out.empty()) {
            out += ';';
        }
        out += num(p.x()) + ',' + num(p.y());
    }
    return out;
}

/// Look up "<key>=#id" in the persisted ref_ids value.
Glib::ustring persisted_ref_href(Glib::ustring const &ref_ids, char const *ref_key)
{
    std::string const raw = ref_ids.raw();
    std::size_t begin = 0;
    while (begin < raw.size()) {
        std::size_t const sep = raw.find(';', begin);
        std::string const entry = raw.substr(begin, sep == std::string::npos ? std::string::npos : sep - begin);
        begin = (sep == std::string::npos) ? raw.size() : sep + 1;
        auto const eq = entry.find('=');
        if (eq == std::string::npos || entry.substr(0, eq) != ref_key) {
            continue;
        }
        std::string const value = entry.substr(eq + 1);
        if (!value.empty() && value[0] == '#') {
            return value;
        }
    }
    return {};
}

/// A change signature of a measured item: bounds, transform and path data.
std::string item_signature(SPItem *item)
{
    std::string sig;
    if (auto box = item->documentGeometricBounds()) {
        sig += num(box->min().x()) + ',' + num(box->min().y()) + ',' + num(box->max().x()) + ',' +
               num(box->max().y()) + ';';
    }
    if (auto const *transform = item->getAttribute("transform")) {
        sig += transform;
    }
    sig += ';';
    if (auto *shape = cast<SPShape>(item)) {
        if (auto const *d = shape->getRepr()->attribute("d")) {
            sig += d;
        }
    }
    return sig;
}

} // namespace

LPEDimension::LPEDimension(LivePathEffectObject *lpeobject)
    : Effect(lpeobject)
    , kind(_("Dimension type:"), _("What the dimension measures"), "kind", DimKindConverter, &wr, this, DK_ALIGNED)
    , start_ref(_("Start point:"), _("Object holding the first measured point"), "start_ref", &wr, this)
    , end_ref(_("End point:"), _("Object holding the second measured point"), "end_ref", &wr, this)
    , vertex_ref(_("Vertex point:"), _("Object holding the angular vertex"), "vertex_ref", &wr, this)
    , start_anchor(_("Start anchor"), _("Anchor of the first point on its object (bbox:r, node:0, rel:0/1)"), "start_anchor", &wr, this, "")
    , end_anchor(_("End anchor"), _("Anchor of the second point on its object"), "end_anchor", &wr, this, "")
    , vertex_anchor(_("Vertex anchor"), _("Anchor of the angular vertex on its object"), "vertex_anchor", &wr, this, "")
    , offset(_("Offset"), _("Distance of the dimension line from the measured points, in mm; arc radius for angular dimensions"), "offset", &wr, this, 5.0)
    , precision(_("Precision"), _("Digits after the decimal point of the label"), "precision", &wr, this, 2)
    , unit(_("Unit"), _("Unit of the label value"), "unit", &wr, this, "mm")
    , scale(_("Scale"), _("Drawing scale factor applied to the measured value"), "scale", &wr, this, 1.0)
    , text_position(_("Label position"), _("Where the label sits relative to the dimension line"), "text_position", DimTextPosConverter, &wr, this, DTP_ABOVE)
    , text_horizontal(_("Horizontal label"), _("Keep the label horizontal instead of parallel to the dimension line"), "text_horizontal", &wr, this, false)
    , show_units(_("Show units"), _("Append the unit to the label value"), "show_units", &wr, this, true)
    , prefix(_("Prefix"), _("Text prepended to the label value"), "prefix", &wr, this, "")
    , suffix(_("Suffix"), _("Text appended to the label"), "suffix", &wr, this, "")
    , font_size(_("Label size"), _("Label font size in points, used when the dimension is created"), "font_size", &wr, this, 10.0)
    , arrow_length(_("Arrow length"), _("Arrowhead length in mm"), "arrow_length", &wr, this, 2.5)
    , ext_gap(_("Extension gap"), _("Gap between the measured point and its extension line, in mm"), "ext_gap", &wr, this, 1.0)
    , ext_overshoot(_("Extension overshoot"), _("How far the extension lines pass the dimension line, in mm"), "ext_overshoot", &wr, this, 2.0)
    , last_points(_("Last points"), _("Last resolved measured points (freeze state)"), "last_points", &wr, this, "")
    , ref_ids(_("Reference ids"), _("Persisted references (restored after fork or reload)"), "ref_ids", &wr, this, "")
{
    precision.param_set_range(0, 6);
    precision.param_make_integer();

    registerParameter(&kind);
    registerParameter(&start_ref);
    registerParameter(&end_ref);
    registerParameter(&vertex_ref);
    registerParameter(&start_anchor);
    registerParameter(&end_anchor);
    registerParameter(&vertex_anchor);
    registerParameter(&offset);
    registerParameter(&precision);
    registerParameter(&unit);
    registerParameter(&scale);
    registerParameter(&text_position);
    registerParameter(&text_horizontal);
    registerParameter(&show_units);
    registerParameter(&prefix);
    registerParameter(&suffix);
    registerParameter(&font_size);
    registerParameter(&arrow_length);
    registerParameter(&ext_gap);
    registerParameter(&ext_overshoot);
    registerParameter(&last_points);
    registerParameter(&ref_ids);

    // An effect instance created for a reloaded or forked LPE object is not
    // evaluated until something changes, so arming from doBeforeEffect alone
    // would deadlock. The LPE object reports activity (e.g. after
    // set(PATH_EFFECT) or a document update); use that to arm the watchers.
    _lpeobj_modified_connection = lpeobject->connectModified(
        [this](SPObject *, unsigned) { arm_live_updates(); });
}

LPEDimension::~LPEDimension()
{
    _parent_transform_connection.disconnect();
    _lpeobj_modified_connection.disconnect();
    disconnect_item_watchers();
}

Kind LPEDimension::dimension_kind() const
{
    return dimension_kind_from_param(static_cast<DimKindParam>(kind));
}

Glib::ustring LPEDimension::ref_href(char const *ref_key)
{
    if (auto *lpeobj = getLPEObj()) {
        if (char const *attr = lpeobj->getRepr()->attribute(ref_key); attr && attr[0] == '#') {
            return attr;
        }
    }
    // A forked or reloaded effect can lose the href: SatelliteParam's
    // param_readSVGValue clears it when the effect is not applied to any item
    // yet. Fall back to the persisted ref_ids copy.
    return persisted_ref_href(ref_ids.get_value(), ref_key);
}

SPObject *LPEDimension::resolve_ref_object(SatelliteParam &ref, char const *ref_key)
{
    if (auto *object = ref.getObject()) {
        return object;
    }
    Glib::ustring const href = ref_href(ref_key);
    // Re-reading is only safe once the effect is applied; otherwise the
    // satellite parameter writes the empty value back to the repr.
    if (href.empty() || getCurrrentLPEItems().empty()) {
        return nullptr;
    }
    ref.param_readSVGValue(href.c_str());
    return ref.getObject();
}

std::optional<Geom::Point> LPEDimension::resolve_ref(SatelliteParam &ref, char const *ref_key,
                                                     TextParam const &anchor_param)
{
    auto *item = cast<SPItem>(resolve_ref_object(ref, ref_key));
    auto anchor = Dimension::parse_anchor(anchor_param.get_value().raw());
    if (!item || !anchor) {
        return std::nullopt;
    }
    auto const point = Dimension::resolve_anchor(*anchor, item->documentGeometricBounds(), item_curve_doc(item));
    if (point) {
        return point;
    }
    // A segment dimension's end anchor names node k+1; when node k is the
    // last node of a closed subpath, k+1 is out of range and wraps to the
    // subpath start.
    if (dimension_kind() == Kind::Segment && anchor->type == Anchor::Node) {
        auto const curve = item_curve_doc(item);
        auto const nodes = Dimension::curve_nodes(curve);
        std::size_t const m = static_cast<std::size_t>(anchor->node);
        for (auto const &span : node_spans(curve)) {
            if (span.closed && m == span.last + 1 && span.first < nodes.size()) {
                return nodes[span.first];
            }
        }
    }
    return std::nullopt;
}

std::vector<Geom::Point> LPEDimension::original_d_nodes(SPLPEItem const *lpeitem) const
{
    auto const *path = cast<SPPath>(const_cast<SPLPEItem *>(lpeitem));
    if (!path) {
        return {};
    }
    auto const *curve = path->curveBeforeLPE();
    if (!curve) {
        return {};
    }
    return Dimension::curve_nodes(*curve);
}

std::optional<Geom::Point> LPEDimension::fallback_point(SPLPEItem const *lpeitem, std::size_t index,
                                                        bool prefer_original_d) const
{
    // A free point lives on the effect's own path, so the original-d nodes
    // follow the dimension when its group moves. The freeze cache is only for
    // referenced points whose object disappeared.
    if (!prefer_original_d && index < _last_points_cache.size()) {
        return _last_points_cache[index];
    }
    auto const nodes = original_d_nodes(lpeitem);
    if (index < nodes.size()) {
        return nodes[index] * lpeitem->i2doc_affine(); // original-d is in path coordinates
    }
    if (index < _last_points_cache.size()) {
        return _last_points_cache[index];
    }
    return std::nullopt;
}

void LPEDimension::resolve_points(SPLPEItem const *lpeitem)
{
    if (_last_points_cache.empty()) {
        _last_points_cache = parse_points(last_points.get_value());
    }

    bool const angular = dimension_kind() == Kind::Angular;
    std::size_t const needed = angular ? 3 : 2;
    // Angular points arrive in arm A, vertex, arm B order; the parameters
    // store start_ref, end_ref and vertex_ref.
    struct Binding {
        SatelliteParam *ref;
        char const *key;
        TextParam *anchor;
    };
    Binding const bindings[3] = {
        {&start_ref, "start_ref", &start_anchor},
        {angular ? &vertex_ref : &end_ref, angular ? "vertex_ref" : "end_ref",
         angular ? &vertex_anchor : &end_anchor},
        {&end_ref, "end_ref", &end_anchor},
    };

    std::vector<Geom::Point> resolved;
    for (std::size_t i = 0; i < needed; ++i) {
        auto point = resolve_ref(*bindings[i].ref, bindings[i].key, *bindings[i].anchor);
        if (!point) {
            bool const free_point = ref_href(bindings[i].key).empty();
            point = fallback_point(lpeitem, i, free_point);
        }
        if (!point) {
            return; // unresolvable and nothing to freeze onto
        }
        resolved.push_back(*point);
    }
    _doc_points = std::move(resolved);
}

void LPEDimension::store_last_points()
{
    if (_updating_last_points || _doc_points.empty()) {
        return;
    }
    bool changed = _last_points_cache.size() != _doc_points.size();
    if (!changed) {
        for (std::size_t i = 0; i < _doc_points.size(); ++i) {
            if ((_last_points_cache[i] - _doc_points[i]).length() > UNCHANGED_EPS) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) {
        return;
    }
    auto *doc = getSPDoc();
    if (!doc) {
        return;
    }
    _updating_last_points = true;
    {
        DocumentUndo::ScopedInsensitive insensitive(doc);
        last_points.param_setValue(format_points(_doc_points), true);
        _last_points_cache = _doc_points;
    }
    _updating_last_points = false;
}

void LPEDimension::update_label(SPLPEItem const *lpeitem)
{
    auto *item = const_cast<SPLPEItem *>(lpeitem);
    auto *doc = getSPDoc();
    auto *parent = cast<SPItem>(item->parent);
    if (!doc || !parent) {
        return;
    }
    Inkscape::XML::Node *label = find_label_repr(parent->getRepr());
    if (!label) {
        if (_label_debug_emitted++ == 0) {
            g_debug("LPEDimension: no rosepen-dimension-label sibling found");
        }
        return;
    }

    DocumentUndo::ScopedInsensitive insensitive(doc);

    bool const angular = dimension_kind() == Kind::Angular;
    auto const *unit_abbr = angular ? "" : unit.get_abbreviation();
    auto const precision_digits = static_cast<int>(static_cast<double>(precision));

    // Label content; an invalid build marks the label with an em dash.
    Glib::ustring content = "—";
    if (_result.valid) {
        double v = _result.value_uu_or_deg;
        if (!angular) {
            v = Dimension::to_unit(v * _length_factor, doc->getDocumentScale()[Geom::X], unit_abbr, scale);
        }
        content = Dimension::format_label(v, precision_digits, unit_abbr, show_units, prefix.get_value(),
                                          suffix.get_value(), angular);
    }

    Inkscape::XML::Node *tspan = nullptr;
    for (auto *child = label->firstChild(); child; child = child->next()) {
        if (std::strcmp(child->name(), "svg:tspan") == 0) {
            tspan = child;
            break;
        }
    }
    if (!tspan) {
        tspan = doc->getReprDoc()->createElement("svg:tspan");
        label->addChild(tspan, nullptr);
        Inkscape::GC::release(tspan);
    }
    // The text belongs to a text child node; content stored on the tspan
    // element itself is invisible to the SVG writer and to the SPString model.
    Inkscape::XML::Node *text = nullptr;
    for (auto *child = tspan->firstChild(); child; child = child->next()) {
        if (child->type() == Inkscape::XML::NodeType::TEXT_NODE) {
            text = child;
            break;
        }
    }
    if (!text) {
        text = doc->getReprDoc()->createTextNode(content.c_str());
        tspan->addChild(text, nullptr);
        Inkscape::GC::release(text);
    } else if (!text->content() || content != text->content()) {
        text->setContent(content.c_str());
    }

    if (!_result.valid) {
        return; // keep the last position/rotation of a frozen label
    }

    // Position in the parent's coordinates (the label is a path sibling).
    Geom::Point const lp = _result.label_pos * item->i2doc_affine() * parent->i2doc_affine().inverse();
    set_pos_attr(label, "x", lp.x());
    set_pos_attr(label, "y", lp.y());

    double const angle = _result.label_angle_deg;
    if (std::fabs(angle) > 1e-9) {
        std::string const rot = "rotate(" + num(angle) + " " + num(lp.x()) + " " + num(lp.y()) + ")";
        if (char const *old = label->attribute("transform")) {
            if (rot == old) {
                return;
            }
        }
        label->setAttribute("transform", rot);
    } else if (label->attribute("transform")) {
        label->removeAttribute("transform");
    }
}

void LPEDimension::connect_parent_transform(SPLPEItem const *lpeitem)
{
    auto *parent = cast<SPItem>(lpeitem->parent);
    if (parent == _parent_item) {
        return;
    }
    _parent_transform_connection.disconnect();
    _parent_item = parent;
    if (!parent) {
        return;
    }
    // Moving the dimension group must not drag a linked dimension away from
    // the measured geometry: re-evaluate so the output stays glued in
    // document coordinates. Free dimensions recompute to the same path
    // coordinates and simply travel with the group.
    _parent_transform_connection = parent->connectTransformed(
        sigc::mem_fun(*this, &LPEDimension::on_parent_transformed));
}

void LPEDimension::on_parent_transformed(Geom::Affine const * /*rel_transf*/, SPItem * /*moved*/)
{
    if (auto *lpeobj = getLPEObj()) {
        lpeobj->requestModified(SP_OBJECT_MODIFIED_FLAG);
    }
}

void LPEDimension::disconnect_item_watchers()
{
    for (auto &connection : _item_watchers) {
        connection.disconnect();
    }
    _item_watchers.clear();
    _watched_items.clear();
    _item_signatures.clear();
}

void LPEDimension::refresh_item_watchers()
{
    SatelliteParam *const refs[3] = {&start_ref, &end_ref, &vertex_ref};
    char const *const keys[3] = {"start_ref", "end_ref", "vertex_ref"};
    std::vector<SPItem *> items;
    for (int i = 0; i < 3; ++i) {
        if (auto *item = cast<SPItem>(resolve_ref_object(*refs[i], keys[i]))) {
            if (std::find(items.begin(), items.end(), item) == items.end()) {
                items.push_back(item);
            }
        }
    }
    if (items == _watched_items) {
        return;
    }

    disconnect_item_watchers();
    for (auto *item : items) {
        _watched_items.push_back(item);
        _item_signatures.push_back(item_signature(item));
        _item_watchers.push_back(item->connectModified([this](SPObject *, unsigned) { on_ref_item_changed(); }));
        _item_watchers.push_back(item->connectTransformed([this](Geom::Affine const *, SPItem *) { on_ref_item_changed(); }));
        // A deleted item must never be dereferenced by a later change signal
        // of another watched item.
        _item_watchers.push_back(item->connectRelease([this](SPObject *released) {
            std::replace(_watched_items.begin(), _watched_items.end(), static_cast<SPItem *>(released),
                         static_cast<SPItem *>(nullptr));
        }));
    }
}

void LPEDimension::on_ref_item_changed()
{
    if (_evaluating) {
        return;
    }
    for (std::size_t i = 0; i < _watched_items.size(); ++i) {
        if (!_watched_items[i]) {
            continue;
        }
        std::string const signature = item_signature(_watched_items[i]);
        if (signature != _item_signatures[i]) {
            _item_signatures[i] = signature;
            if (auto *lpeobj = getLPEObj()) {
                lpeobj->requestModified(SP_OBJECT_MODIFIED_FLAG);
            }
            return;
        }
    }
}

void LPEDimension::arm_live_updates()
{
    SatelliteParam *const refs[3] = {&start_ref, &end_ref, &vertex_ref};
    char const *const keys[3] = {"start_ref", "end_ref", "vertex_ref"};
    for (int i = 0; i < 3; ++i) {
        if (auto *object = resolve_ref_object(*refs[i], keys[i])) {
            if (!refs[i]->isConnected()) {
                refs[i]->start_listening(object);
            }
        }
    }
    refresh_item_watchers();
}

void LPEDimension::doBeforeEffect(SPLPEItem const *lpeitem)
{
    auto *item = const_cast<SPLPEItem *>(lpeitem);
    auto *doc = getSPDoc();
    if (!item || !doc) {
        _result = Dimension::Result{};
        return;
    }

    connect_parent_transform(item);
    arm_live_updates();

    _evaluating = true;
    resolve_points(item);

    Geom::Affine const i2doc = item->i2doc_affine();
    Geom::Affine const doc2path = i2doc.inverse();
    double const doc_scale = std::max(doc->getDocumentScale()[Geom::X], 1e-9);
    // Style lengths are given in document units; the path may itself be
    // scaled relative to the document, so compensate only that scale (the
    // document scale is already divided out by mm_to_uu below).
    double const det = i2doc[0] * i2doc[3] - i2doc[1] * i2doc[2];
    double const len_scale = std::fabs(det) > 1e-12 ? doc_scale / std::sqrt(std::fabs(det)) : 1.0;
    // The value is measured in path coordinates; a scaled parent (group or
    // layer) must not change what the label reads.
    _length_factor = 1.0 / len_scale;
    auto mm_to_uu = [&](double mm) { return Inkscape::Util::Quantity::convert(mm, "mm", "px") / doc_scale; };

    Dimension::Style style;
    style.offset = mm_to_uu(offset) * len_scale;
    style.arrow_length = mm_to_uu(arrow_length) * len_scale;
    style.ext_gap = mm_to_uu(ext_gap) * len_scale;
    style.ext_overshoot = mm_to_uu(ext_overshoot) * len_scale;
    style.text_position = text_position_from_param(static_cast<DimTextPos>(text_position));
    style.text_horizontal = text_horizontal;
    double const font_uu = Inkscape::Util::Quantity::convert(font_size, "pt", "px") / doc_scale;
    style.label_gap = 0.35 * font_uu * len_scale;

    std::vector<Geom::Point> pts;
    pts.reserve(_doc_points.size());
    for (auto const &p : _doc_points) {
        pts.push_back(p * doc2path);
    }

    _result = Dimension::build(dimension_kind(), pts, style, true);

    store_last_points();
    update_label(item);
    _evaluating = false;
}

Geom::PathVector LPEDimension::doEffect_path(Geom::PathVector const &path_in)
{
    if (!_result.valid) {
        return path_in; // keep the previous output; the label shows an em dash
    }
    return _result.path;
}

bool LPEDimension::doOnOpen(SPLPEItem const * /*lpeitem*/)
{
    // Reloaded documents must stay live. The hrefs are resolved lazily by
    // arm_live_updates(), which covers both the normal load order (defs
    // precede the drawing) and in-memory documents that never run the
    // file-open fixups.
    arm_live_updates();
    return false;
}

void LPEDimension::doOnRemove(SPLPEItem const * /*lpeitem*/)
{
    // Intentionally empty: removing the effect breaks the dimension apart.
    // The label stays a plain text object and the path keeps its last
    // output, so upstream object-to-path flattening keeps working.
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
