// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Rosepen drawing export model: an SPDocument as the
 * "rosepen-cdr-export/1" JSON that the TraceDraw bridge's writer consumes.
 *
 * See SCHEMA.md beside the bridge's C header for the model contract. The
 * mapping rules, and the fallback each one warns about, are:
 *
 *  - pages (document units to page millimetres, Y down), layers in z-order;
 *    a layer lands on every page that holds at least one of its items, an
 *    item goes to the page holding its visual bbox centre, else to the page
 *    with the largest overlap, else to page 1 (warning);
 *  - a hidden layer is exported visible with a warning, because the format's
 *    writer stores no hidden layers (their shapes would be dropped);
 *  - rect/ellipse keep their kind only under an axis-aligned transform and
 *    without a live path effect, and plain horizontal text keeps its kind;
 *    everything else becomes a path;
 *  - flat sRGB paints stay RGB, CMYK (icc-color of a CMYK profile or
 *    device-cmyk) travels verbatim, spot swatches become named inks;
 *  - gradients become the mean of their stop colours, patterns lose their
 *    fill, images are skipped, clips/masks/filters/markers are dropped, and
 *    clones expand to groups — each with a warning naming the element.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "cdr-export-model.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>


#include <glib.h>
#include <glibmm/ustring.h>

#include <2geom/affine.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/rect.h>

#include "colors/color.h"
#include "colors/document-cms.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/enum.h"
#include "colors/spot-swatch.h"
#include "document.h"
#include "object/master-layer.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-ellipse.h"
#include "object/sp-flowtext.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-lpe-item.h"
#include "object/sp-mask.h"
#include "object/sp-metadata.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-rect.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-stop.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "object/sp-textpath.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "style.h"
#include "style-internal.h"
#include "svg/svg.h"
#include "util/cast.h"
#include "util/units.h"
#include "xml/node.h"

namespace Inkscape::Extension::Internal::CdrExport {

namespace {

using Inkscape::Colors::Color;
using Inkscape::Colors::Space::Type;

// ------------------------------------------------------------- json writing --

/// A JSON string literal per RFC 8259: quotes, backslashes and control
/// characters escaped, every other byte (UTF-8 included) passed through.
void append_json_string(std::string &out, std::string const &text)
{
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char escape[7];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                    out += escape;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

/// The shortest round-trip form of @a value, always a JSON float, never
/// locale dependent.
void append_json_number(std::string &out, double value)
{
    if (!std::isfinite(value)) {
        out += "0.0"; // NaN and infinities cannot be model values
        return;
    }
    if (value == 0.0) {
        value = 0.0; // normalise -0.0
    }
    char buffer[40];
    auto const result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    // to_chars writes no terminating NUL: look only at what it wrote.
    std::string_view const text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    out += text;
    if (text.find_first_of(".eE") == std::string_view::npos) {
        out += ".0";
    }
}

std::string json_string(std::string const &text)
{
    std::string out;
    append_json_string(out, text);
    return out;
}

std::string json_number(double value)
{
    std::string out;
    append_json_number(out, value);
    return out;
}

std::string json_number_array(std::vector<double> const &values)
{
    std::string out = "[";
    for (auto const value : values) {
        if (out.size() > 1) {
            out.push_back(',');
        }
        append_json_number(out, value);
    }
    out.push_back(']');
    return out;
}

// ------------------------------------------------------------------ helpers --

bool axis_aligned(Geom::Affine const &affine)
{
    return std::abs(affine[1]) < 1e-9 && std::abs(affine[2]) < 1e-9;
}

/// The text content of a text object (every string child, in order).
void collect_text(SPObject const *object, Glib::ustring &out)
{
    for (auto const &child : object->children) {
        if (auto const *string = cast<SPString>(&child)) {
            out += string->string;
        } else if (!is<SPTextPath>(&child)) {
            collect_text(&child, out);
        }
    }
}

bool has_text_path(SPObject const *object)
{
    for (auto const &child : object->children) {
        if (is<SPTextPath>(&child) || has_text_path(&child)) {
            return true;
        }
    }
    return false;
}

/// Whether two styles draw their text runs alike (font, size, weight, slant
/// and fill), so the runs can travel as one span.
bool same_run_style(SPStyle const *a, SPStyle const *b)
{
    auto const family = [](SPStyle const *style) {
        auto const *value = style->font_family.value();
        return std::string(value ? value : "");
    };
    auto const same_fill = [&] {
        auto const &fa = a->fill;
        auto const &fb = b->fill;
        if (fa.isPaintserver() || fb.isPaintserver()) {
            return fa.isPaintserver() && fb.isPaintserver() &&
                   fa.href->getObject() == fb.href->getObject();
        }
        if (fa.isColor() != fb.isColor()) {
            return false;
        }
        return !fa.isColor() || fa.getColor() == fb.getColor();
    };
    return std::abs(a->font_size.computed - b->font_size.computed) < 1e-9 &&
           family(a) == family(b) && a->font_weight.computed == b->font_weight.computed &&
           a->font_style.computed == b->font_style.computed && same_fill();
}

/**
 * Why a text cannot travel as one plain span (several lines, runs styled
 * apart, per-glyph offsets or rotations), or nothing when it can.
 */
std::optional<std::string> plain_text_obstacle(SPText *text)
{
    auto it = text->layout.begin();
    if (it.nextStartOfLine()) { // true only when a second line starts
        return "multi-line text exported as outlines";
    }
    std::optional<std::string> obstacle;
    std::function<void(SPObject *)> visit = [&](SPObject *object) {
        auto const *repr = object->getRepr();
        if (repr && (repr->attribute("dx") || repr->attribute("dy") || repr->attribute("rotate"))) {
            obstacle = "text with glyph offsets or rotations exported as outlines";
            return;
        }
        if (object != text && object->style && !is<SPString>(object) &&
            !same_run_style(object->style, text->style)) {
            obstacle = "text with differently styled runs exported as outlines";
            return;
        }
        for (auto &child : object->children) {
            if (obstacle) {
                return;
            }
            visit(&child);
        }
    };
    visit(text);
    return obstacle;
}

/// The mean sRGB colour of a gradient's stops, or nothing without stops.
std::optional<std::array<double, 3>> mean_stop_color(SPGradient const *gradient)
{
    unsigned count = 0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (auto const &child : gradient->children) {
        auto const *stop = cast<SPStop>(&child);
        if (!stop) {
            continue;
        }
        auto const rgba = stop->getColor().toRGBA();
        red += ((rgba >> 24) & 0xff) / 255.0;
        green += ((rgba >> 16) & 0xff) / 255.0;
        blue += ((rgba >> 8) & 0xff) / 255.0;
        ++count;
    }
    if (count == 0) {
        return std::nullopt;
    }
    return std::array<double, 3>{red / count, green / count, blue / count};
}

/// The sRGB components of a colour, alpha excluded.
std::array<double, 3> rgb_components(Color const &color)
{
    auto const rgba = color.toRGBA();
    return {((rgba >> 24) & 0xff) / 255.0,
            ((rgba >> 16) & 0xff) / 255.0,
            ((rgba >> 8) & 0xff) / 255.0};
}

/// The colour's own alpha, 1 when it does not carry one.
double color_alpha(Color const &color)
{
    return (color.toRGBA() & 0xff) / 255.0;
}

/// {"model":"rgb","r":..,"g":..,"b":..}
std::string json_rgb(std::array<double, 3> const &rgb)
{
    std::string out = R"({"model":"rgb","r":)";
    append_json_number(out, rgb[0]);
    out += R"(,"g":)";
    append_json_number(out, rgb[1]);
    out += R"(,"b":)";
    append_json_number(out, rgb[2]);
    out.push_back('}');
    return out;
}

/// {"model":"cmyk","c":..,"m":..,"y":..,"k":..}
std::string json_cmyk(std::vector<double> const &cmyk)
{
    std::string out = R"({"model":"cmyk","c":)";
    append_json_number(out, cmyk[0]);
    out += R"(,"m":)";
    append_json_number(out, cmyk[1]);
    out += R"(,"y":)";
    append_json_number(out, cmyk[2]);
    out += R"(,"k":)";
    append_json_number(out, cmyk[3]);
    out.push_back('}');
    return out;
}

/// {"model":"spot","ink":N,"tint":1.0} — the ink index is an integer, the
/// model's reader takes no floats for it.
std::string json_spot(std::size_t index)
{
    std::string out = R"({"model":"spot","ink":)" + std::to_string(index);
    out += R"(,"tint":1.0})";
    return out;
}

/// One named spot ink of the model's ink table.
struct InkEntry
{
    std::string name;
    std::vector<double> cmyk;
};

/// Millimetres per pixel (CSS px at 96 dpi) and per point.
double mm_per(char const *unit)
{
    return Inkscape::Util::Quantity::convert(1, unit, "mm");
}

/**
 * The export state of one document: the page geometry shared by every shape,
 * the collected ink table and the warnings.
 */
class Model
{
public:
    explicit Model(SPDocument *doc)
        : _doc(doc)
    {
    }

    Result run();

private:
    /// One export layer (or master layer) with its shapes per page.
    struct LayerOut
    {
        std::string name;
        std::vector<std::vector<std::string>> per_page; ///< in z-order

        void ensure_pages(std::size_t pages) { per_page.resize(pages); }
    };

    std::string item_shape(SPItem *item, Geom::Affine const &extra, int page);
    std::string use_shape(SPUse *use, int page);
    std::string group_children(SPObject *parent, Geom::Affine const &extra, int page, int depth);
    std::string paint_of(SPIPaint &paint, SPItem *item, double &alpha);
    std::string stroke_of(SPItem *item, Geom::Affine const &placed_affine, double &alpha);
    std::string shape_common(SPItem *item, std::string const &fill, std::string const &stroke,
                             double fill_alpha, double stroke_alpha, bool warn_mismatch);
    std::string fallback_path(SPItem *item, Geom::Affine const &full, std::string const &what,
                              char const *id);
    std::string item_curves(SPItem *item, Geom::Affine const &full, std::string const &fill,
                            std::string const &stroke, double fill_alpha, double stroke_alpha);
    int assign_page(SPItem *item);
    Geom::Affine placed(SPItem *item, Geom::Affine const &extra, int page) const;
    void warn(char const *id, std::string const &what);

    SPDocument *_doc;
    std::vector<Geom::Rect> _page_rects;  ///< document units, Y down
    std::vector<std::string> _page_names;
    double _sx = 0.0;                     ///< user units to millimetres, x
    double _sy = 0.0;                     ///< user units to millimetres, y
    std::vector<InkEntry> _inks;
    std::map<std::string, std::size_t> _ink_index;
    std::vector<std::string> _warnings;
    std::shared_ptr<Colors::Space::AnySpace> _output_space;
};

/// Rects and names of every page, or the viewBox (else the drawing bounds)
/// as the one page of a page-less document.
void collect_page_rects(SPDocument *doc, std::vector<Geom::Rect> &rects,
                        std::vector<std::string> &names)
{
    int index = 1;
    for (auto *page : doc->getPageManager().getPages()) {
        rects.push_back(page->getDocumentRect());
        auto const *label = page->getRepr()->attribute("inkscape:label");
        names.push_back(label && *label ? label : "Page " + std::to_string(index));
        ++index;
    }
    if (!rects.empty()) {
        return;
    }

    if (auto *root = doc->getRoot()) {
        if (root->viewBox_set) {
            rects.push_back(root->viewBox);
        } else {
            // A page-less document without a viewBox: its declared size is
            // the one page, whatever the content does.
            rects.emplace_back(Geom::Point(0, 0),
                               Geom::Point(root->width.computed, root->height.computed));
        }
    }
    if (rects.empty() || rects.front().hasZeroArea()) {
        rects.clear();
        rects.emplace_back(Geom::Point(0, 0), Geom::Point(1, 1));
    }
    names.push_back("Page 1");
}

Result Model::run()
{
    _doc->ensureUpToDate();

    collect_page_rects(_doc, _page_rects, _page_names);
    // Page rects (SPPage::getDocumentRect) and item coordinates
    // (i2doc_affine) are both in document pixels; only the px-to-mm factor
    // is still missing.
    auto const factor = mm_per("px");
    _sx = factor;
    _sy = factor;
    _output_space = _doc->getDocumentCMS().getOutputSpace();

    auto *root = _doc->getRoot();
    g_assert(root);

    // One walk, pre-order: export layers in z-order, master layers kept
    // apart, every item on the page holding it.
    std::vector<LayerOut> layers;
    std::vector<LayerOut> masters;
    std::vector<SPItem *> loose_items; // top-level items outside any layer

    std::function<void(SPObject *, LayerOut *, bool)> walk =
        [&](SPObject *parent, LayerOut *current, bool in_master) {
            for (auto &child : parent->children) {
                if (is<SPDefs>(&child) || is<SPNamedView>(&child) || is<SPMetadata>(&child)) {
                    continue;
                }
                if (auto *group = cast<SPGroup>(&child);
                    group && group->layerMode() == SPGroup::LAYER) {
                    bool const master = MasterLayer::is_master(group);
                    auto &store = master ? masters : layers;
                    store.push_back({group->label() ? group->label()
                                                    : (master ? "Master" : std::string()),
                                     std::vector<std::vector<std::string>>(_page_rects.size())});
                    // The format's writer cannot store a hidden layer (it
                    // would drop its shapes and the flag), so the layer goes
                    // out visible and the change is announced.
                    if (!master && group->isHidden()) {
                        warn(group->getId(),
                             "hidden layer " + store.back().name +
                                 " exported visible; the format stores no hidden layers");
                    }
                    walk(group, &store.back(), master);
                    continue;
                }
                if (auto *item = cast<SPItem>(&child)) {
                    if (parent == root) {
                        loose_items.push_back(item);
                    } else if (current && !item->isHidden()) {
                        // An individually hidden item is skipped silently.
                        int const page = in_master ? 0 : assign_page(item);
                        current->per_page[page].push_back(
                            item_shape(item, Geom::identity(), page));
                    }
                }
                if (!current && cast<SPGroup>(&child)) {
                    // From the root, layers may nest inside plain groups;
                    // inside a layer the descent would duplicate the group's
                    // items, which render as one shape.
                    walk(&child, nullptr, false);
                }
            }
        };
    walk(root, nullptr, false);

    // Top-level items outside any layer render as one nameless bottom layer.
    if (!loose_items.empty()) {
        LayerOut loose{"", std::vector<std::vector<std::string>>(_page_rects.size())};
        for (auto *item : loose_items) {
            if (item->isHidden()) {
                continue;
            }
            int const page = assign_page(item);
            loose.per_page[page].push_back(item_shape(item, Geom::identity(), page));
        }
        layers.insert(layers.begin(), std::move(loose));
    }

    // Assemble the model.
    std::string json = R"({"schema":"rosepen-cdr-export/1","title":)";
    std::string stem;
    if (char const *filename = _doc->getDocumentFilename(); filename && *filename) {
        stem = filename;
        auto const slash = stem.find_last_of("/\\");
        if (slash != std::string::npos) {
            stem.erase(0, slash + 1);
        }
        auto const dot = stem.find_last_of('.');
        if (dot != std::string::npos && dot > 0) {
            stem.erase(dot);
        }
    }
    json += json_string(stem);

    json += R"(,"inks":[)";
    for (std::size_t i = 0; i < _inks.size(); ++i) {
        if (i) {
            json.push_back(',');
        }
        json += R"({"name":)" + json_string(_inks[i].name) + R"(,"cmyk":)" +
                json_number_array(_inks[i].cmyk) + "}";
    }
    json += "],\"pages\":[";

    for (std::size_t page = 0; page < _page_rects.size(); ++page) {
        if (page) {
            json.push_back(',');
        }
        json += R"({"name":)" + json_string(_page_names[page]);
        json += R"(,"width_mm":)" + json_number(_page_rects[page].width() * _sx);
        json += R"(,"height_mm":)" + json_number(_page_rects[page].height() * _sy);
        json += R"(,"layers":[)";
        bool first_layer = true;
        for (auto const &layer : layers) {
            if (layer.per_page[page].empty()) {
                continue;
            }
            if (!first_layer) {
                json.push_back(',');
            }
            first_layer = false;
            // The format's writer has no hidden layers, so every layer goes
            // out visible; hiding one produces a warning at collection time.
            json += R"({"name":)" + json_string(layer.name) + R"(,"visible":true,"shapes":[)";
            bool first_shape = true;
            for (auto const &shape : layer.per_page[page]) {
                if (shape.empty()) {
                    continue;
                }
                if (!first_shape) {
                    json.push_back(',');
                }
                first_shape = false;
                json += shape;
            }
            json += "]}";
        }
        json += "]}";
    }
    json += "],\"master\":[";

    bool first_master = true;
    for (auto const &master : masters) {
        // The writer repeats master shapes on every page; the model carries
        // them once, in page 1's coordinates.
        std::string shapes;
        for (auto const &shape : master.per_page[0]) {
            if (shape.empty()) {
                continue;
            }
            if (!shapes.empty()) {
                shapes.push_back(',');
            }
            shapes += shape;
        }
        if (shapes.empty()) {
            continue;
        }
        if (!first_master) {
            json.push_back(',');
        }
        first_master = false;
        json += R"({"name":)" + json_string(master.name) +
                R"(,"visible":true,"shapes":[)" + shapes + "]}";
    }
    json += "]}";

    return Result{std::move(json), std::move(_warnings)};
}

void Model::warn(char const *id, std::string const &what)
{
    _warnings.push_back(std::string("cdr export: ") + (id && *id ? id : "item") + ": " + what);
}

/// The item's transform to document units, then to page millimetres (Y down).
Geom::Affine Model::placed(SPItem *item, Geom::Affine const &extra, int page) const
{
    auto const &rect = _page_rects[page];
    return item->i2doc_affine() * extra * Geom::Translate(-rect.left(), -rect.top()) *
           Geom::Scale(_sx, _sy);
}

/**
 * The page an item belongs to: the one holding its visual centre, else the
 * one with the largest overlap, else page 1 with a warning.
 */
int Model::assign_page(SPItem *item)
{
    char const *id = item->getId();
    auto const box = item->documentVisualBounds();
    if (!box) {
        warn(id, "item without a bounding box placed on page 1");
        return 0;
    }

    auto const centre = box->midpoint();
    for (std::size_t page = 0; page < _page_rects.size(); ++page) {
        if (_page_rects[page].contains(centre)) {
            return static_cast<int>(page);
        }
    }

    int best = -1;
    double best_area = 0.0;
    for (std::size_t page = 0; page < _page_rects.size(); ++page) {
        if (auto const overlap = Geom::intersect(_page_rects[page], *box)) {
            auto const area = overlap->width() * overlap->height();
            if (area > best_area) {
                best_area = area;
                best = static_cast<int>(page);
            }
        }
    }
    if (best >= 0) {
        return best;
    }
    warn(id, "off-page object placed on page 1");
    return 0;
}

/// The opacity and paint members every shape carries.
std::string Model::shape_common(SPItem *item, std::string const &fill, std::string const &stroke,
                                double fill_alpha, double stroke_alpha, bool warn_mismatch)
{
    auto *style = item->style;
    double const item_opacity = style ? style->opacity.as_double() : 1.0;
    double const fill_opacity = (style ? style->fill_opacity.as_double() : 1.0) * fill_alpha;
    double const stroke_opacity = (style ? style->stroke_opacity.as_double() : 1.0) * stroke_alpha;

    bool const has_fill = fill != "null";
    bool const has_stroke = stroke != "null";

    double const chosen = has_fill ? fill_opacity : stroke_opacity;
    if (warn_mismatch && has_fill && has_stroke &&
        std::abs(fill_opacity - stroke_opacity) > 1e-6) {
        char text[256];
        std::snprintf(text, sizeof(text),
                      "fill-opacity %.3g and stroke-opacity %.3g differ; only the fill's is kept",
                      fill_opacity, stroke_opacity);
        warn(item->getId(), text);
    }

    std::string out = R"("opacity":)";
    append_json_number(out, item_opacity * chosen);
    out += R"(,"fill":)" + (has_fill ? fill : std::string("null"));
    out += R"(,"stroke":)" + (has_stroke ? stroke : std::string("null"));
    return out;
}

/**
 * The paint of one fill/stroke property as model JSON, "null" when there is
 * none. @a alpha receives the paint colour's own alpha.
 */
std::string Model::paint_of(SPIPaint &paint, SPItem *item, double &alpha)
{
    alpha = 1.0;
    // The computed paint: the style cascade has already resolved inheritance
    // (a paint inherited from a group is not 'set' on the item) and the CSS
    // defaults (black fill, no stroke).
    if (paint.isContext() || paint.isNone()) {
        return "null";
    }

    if (paint.isPaintserver()) {
        auto *gradient = cast<SPGradient>(paint.href->getObject());
        if (!gradient) {
            warn(item->getId(), "pattern or mesh fill replaced with no fill");
            return "null";
        }

        if (auto const ink = Colors::Spot::get_spot_name(gradient)) {
            // A named ink: the swatch colour is its process stand-in.
            std::vector<double> cmyk{0.0, 0.0, 0.0, 1.0};
            bool have_cmyk = false;
            auto const alternate = gradient->getFirstStop()
                                       ? gradient->getFirstStop()->getColor()
                                       : Color(0x0);
            auto const alternate_space = alternate.getSpace();
            if (alternate_space && alternate_space->getComponentType() == Type::CMYK) {
                auto const values = alternate.getValues();
                if (values.size() >= 4) {
                    cmyk.assign(values.begin(), values.begin() + 4);
                    have_cmyk = true;
                }
            } else if (_output_space &&
                       _output_space->getComponentType() == Type::CMYK) {
                if (auto converted = alternate.converted(_output_space)) {
                    auto const values = converted->getValues();
                    if (values.size() >= 4) {
                        cmyk.assign(values.begin(), values.begin() + 4);
                        have_cmyk = true;
                    }
                }
            }
            if (!have_cmyk) {
                warn(item->getId(),
                     "spot ink " + *ink + " has no CMYK alternate; process black used");
            }
            auto position = _ink_index.find(*ink);
            if (position == _ink_index.end()) {
                position = _ink_index.emplace(*ink, _inks.size()).first;
                _inks.push_back({*ink, cmyk});
            }
            return json_spot(position->second);
        }

        // An ordinary gradient becomes the mean of its stop colours.
        if (auto const mean = mean_stop_color(gradient)) {
            warn(item->getId(), "gradient replaced with the mean of its stop colours");
            return json_rgb(*mean);
        }
        warn(item->getId(), "gradient without stops replaced with no fill");
        return "null";
    }

    if (!paint.isColor()) {
        return "null";
    }

    auto const color = paint.getColor();
    alpha = color_alpha(color);
    if (auto const space = color.getSpace(); space && space->getComponentType() == Type::CMYK) {
        // CMYK travels verbatim, whatever profile it belongs to.
        auto const values = color.getValues();
        if (values.size() < 4) {
            return "null";
        }
        return json_cmyk({values[0], values[1], values[2], values[3]});
    }
    return json_rgb(rgb_components(color));
}

/// The stroke model JSON of an item, or "null". @a alpha receives the stroke
/// colour's own alpha.
std::string Model::stroke_of(SPItem *item, Geom::Affine const &placed_affine, double &alpha)
{
    alpha = 1.0;
    auto *style = item->style;
    // Computed values: a stroke inherited from a group counts.
    if (!style || style->stroke.isNone() || !(style->stroke_width.computed > 0.0)) {
        return "null";
    }

    double paint_alpha = 1.0;
    auto const paint = paint_of(style->stroke, item, paint_alpha);
    if (paint == "null") {
        return "null";
    }
    alpha = paint_alpha;

    // The stroke scales with the shape; the areal scale is the fair guess
    // when the transform is not axis aligned.
    double const linear = std::sqrt(std::abs(placed_affine.det()));

    char const *cap = "butt";
    switch (style->stroke_linecap.as_enum()) {
        case SP_STROKE_LINECAP_ROUND: cap = "round"; break;
        case SP_STROKE_LINECAP_SQUARE: cap = "square"; break;
        default: break;
    }
    char const *join = "miter";
    switch (style->stroke_linejoin.as_enum()) {
        case SP_STROKE_LINEJOIN_ROUND: join = "round"; break;
        case SP_STROKE_LINEJOIN_BEVEL: join = "bevel"; break;
        default: break;
    }

    std::string dashes = "[]";
    if (style->stroke_dasharray.is_valid()) {
        auto const values = style->stroke_dasharray.get_computed();
        if (!values.empty()) {
            dashes = "[";
            for (auto const dash : values) {
                if (dashes.size() > 1) {
                    dashes.push_back(',');
                }
                append_json_number(dashes, dash * linear);
            }
            dashes.push_back(']');
        }
    }

    std::string out = R"({"paint":)" + paint;
    out += R"(,"width_mm":)" + json_number(style->stroke_width.computed * linear);
    out += R"(,"cap":")" + std::string(cap) + '"';
    out += R"(,"join":")" + std::string(join) + '"';
    out += R"(,"miter":)" + json_number(style->stroke_miterlimit.value);
    out += R"(,"dash_mm":)" + dashes;
    out.push_back('}');
    return out;
}

/// A curve-based shape JSON with the given paints (the common fallback tail).
std::string Model::item_curves(SPItem *item, Geom::Affine const &full, std::string const &fill,
                               std::string const &stroke, double fill_alpha, double stroke_alpha)
{
    auto *shape = cast<SPShape>(item);
    std::optional<Geom::PathVector> curve;
    if (shape) {
        if (auto const *existing = shape->curve()) {
            curve = *existing;
        }
    } else if (auto *text = cast<SPText>(item)) {
        curve = text->getNormalizedBpath();
    } else if (auto *flow = cast<SPFlowtext>(item)) {
        curve = flow->getNormalizedBpath();
    }
    if (!curve || curve->empty()) {
        return {};
    }

    auto path = *curve;
    path *= full;

    // Absolute path data; the model's reader parses no relative commands.
    std::string out = R"({"kind":"path","d":)" + json_string(sp_svg_write_path(path, true));
    out.push_back(',');
    out += shape_common(item, fill, stroke, fill_alpha, stroke_alpha, true);
    out.push_back('}');
    return out;
}

/// One item rendered as an outline path, with its paint if it has any.
std::string Model::fallback_path(SPItem *item, Geom::Affine const &full, std::string const &what,
                                 char const *id)
{
    double fill_alpha = 1.0;
    double stroke_alpha = 1.0;
    std::string fill = "null";
    std::string stroke = "null";
    if (item->style) {
        fill = paint_of(item->style->fill, item, fill_alpha);
        stroke = stroke_of(item, full, stroke_alpha);
    }
    if (fill == "null" && stroke == "null") {
        warn(id, what);
        return {};
    }
    auto path = item_curves(item, full, fill, stroke, fill_alpha, stroke_alpha);
    if (!path.empty()) {
        warn(id, what);
    }
    return path;
}

/// A clone: the referenced element rendered at the use's position.
std::string Model::use_shape(SPUse *use, int page)
{
    auto *original = use->get_original();
    if (!original || original == use) {
        return {};
    }
    warn(use->getId(), "clone expanded in place");

    // The clone displays the original at the use's position: undo the
    // original's own document placement and put the use's in its place.
    Geom::Affine const swap = use->i2doc_affine() *
                              Geom::Translate(use->x.computed, use->y.computed) *
                              original->i2doc_affine().inverse();
    return item_shape(original, swap, page);
}

/// The model JSON of one item, or "" when nothing is left of it.
std::string Model::item_shape(SPItem *item, Geom::Affine const &extra, int page)
{
    if (is<SPImage>(item)) {
        warn(item->getId(), "image skipped");
        return {};
    }
    if (auto *use = cast<SPUse>(item)) {
        return use_shape(use, page);
    }

    char const *id = item->getId();

    // Everything that travels only as a note.
    if (auto *style = item->style) {
        if (item->getClipObject()) {
            warn(id, "clip path ignored, the shape is exported unclipped");
        }
        if (item->getMaskObject()) {
            warn(id, "mask ignored, the shape is exported unmasked");
        }
        if (style->filter.set) {
            warn(id, "filter ignored");
        }
    }
    if (auto *shape = cast<SPShape>(item); shape && !shape->get_markers().empty()) {
        warn(id, "markers are not exported");
    }
    bool const has_path_effect = [&] {
        if (auto *lpe = cast<SPLPEItem>(item)) {
            return lpe->hasPathEffect();
        }
        return false;
    }();

    auto const full = placed(item, extra, page);

    if (auto *rect = cast<SPRect>(item); rect && axis_aligned(full) && !has_path_effect) {
        double fill_alpha = 1.0;
        double stroke_alpha = 1.0;
        auto const fill = paint_of(rect->style->fill, rect, fill_alpha);
        auto const stroke = stroke_of(rect, full, stroke_alpha);
        if (fill == "null" && stroke == "null") {
            return {};
        }

        double const x = rect->x.computed * full[0] + full[4];
        double const y = rect->y.computed * full[3] + full[5];
        double const w = rect->width.computed * full[0];
        double const h = rect->height.computed * full[3];
        // A mirrored rect has a negative scale; its corner radii do not.
        double const rx = std::clamp(rect->rx.computed * std::abs(full[0]), 0.0, std::abs(w) / 2.0);
        double const ry = std::clamp(rect->ry.computed * std::abs(full[3]), 0.0, std::abs(h) / 2.0);

        std::string out = R"({"kind":"rect","x":)" + json_number(x);
        out += R"(,"y":)" + json_number(y);
        out += R"(,"w":)" + json_number(w);
        out += R"(,"h":)" + json_number(h);
        out += R"(,"rx":)" + json_number((rx + ry) / 2.0);
        out.push_back(',');
        out += shape_common(rect, fill, stroke, fill_alpha, stroke_alpha, true);
        out.push_back('}');
        return out;
    }

    if (auto *ellipse = cast<SPGenericEllipse>(item)) {
        if (ellipse->type == SP_GENERIC_ELLIPSE_ARC || has_path_effect) {
            return fallback_path(item, full,
                                 has_path_effect ? "path effect baked into a path"
                                                 : "arc exported as a path",
                                 id);
        }
        if (axis_aligned(full)) {
            double fill_alpha = 1.0;
            double stroke_alpha = 1.0;
            auto const fill = paint_of(ellipse->style->fill, ellipse, fill_alpha);
            auto const stroke = stroke_of(ellipse, full, stroke_alpha);
            if (fill == "null" && stroke == "null") {
                return {};
            }

            std::string out = R"({"kind":"ellipse","cx":)";
            append_json_number(out, ellipse->cx.computed * full[0] + full[4]);
            out += R"(,"cy":)";
            append_json_number(out, ellipse->cy.computed * full[3] + full[5]);
            out += R"(,"rx":)";
            append_json_number(out, ellipse->rx.computed * full[0]);
            out += R"(,"ry":)";
            append_json_number(out, ellipse->ry.computed * full[3]);
            out.push_back(',');
            out += shape_common(ellipse, fill, stroke, fill_alpha, stroke_alpha, true);
            out.push_back('}');
            return out;
        }
        return fallback_path(item, full, "transformed ellipse exported as a path", id);
    }

    if (auto *text = cast<SPText>(item)) {
        bool const vertical =
            text->style && (text->style->writing_mode.as_enum() == SP_CSS_WRITING_MODE_TB_RL ||
                            text->style->writing_mode.as_enum() == SP_CSS_WRITING_MODE_TB_LR);
        if (has_text_path(text)) {
            return fallback_path(item, full, "text on a path exported as outlines", id);
        }
        if (vertical) {
            return fallback_path(item, full, "vertical text exported as outlines", id);
        }
        // A plain span has one size and no mirror: the scale must be upright
        // and uniform, or the glyphs would come out distorted.
        if (!axis_aligned(full) || !(full[0] > 0.0) || !(full[3] > 0.0) ||
            std::abs(full[0] - full[3]) > 1e-9 * std::max(full[0], full[3])) {
            return fallback_path(item, full, "transformed text exported as outlines", id);
        }
        if (has_path_effect) {
            return fallback_path(item, full, "path effect baked into a path", id);
        }
        if (auto const obstacle = plain_text_obstacle(text)) {
            return fallback_path(item, full, *obstacle, id);
        }
        auto const origin = text->getBaselinePoint();
        if (!origin) {
            return fallback_path(item, full, "text without layout exported as outlines", id);
        }

        Glib::ustring content;
        collect_text(text, content);
        if (content.empty()) {
            return {};
        }
        double fill_alpha = 1.0;
        double stroke_alpha = 1.0;
        auto const fill = paint_of(text->style->fill, text, fill_alpha);
        auto const stroke = stroke_of(text, full, stroke_alpha);
        if (fill == "null" && stroke == "null") {
            return {};
        }

        auto const anchor = text->style ? text->style->text_anchor.as_enum()
                                        : SP_CSS_TEXT_ANCHOR_START;
        char const *align = "left";
        if (anchor == SP_CSS_TEXT_ANCHOR_MIDDLE) {
            align = "center";
        } else if (anchor == SP_CSS_TEXT_ANCHOR_END) {
            align = "right";
        }

        auto const point = *origin * full;
        char const *family = "sans-serif";
        if (text->style && text->style->font_family.value() &&
            *text->style->font_family.value()) {
            family = text->style->font_family.value();
        }
        // The size rides the same scale as the geometry, but in points.
        double const size_pt = (text->style ? text->style->font_size.computed : 12.0) *
                               full[0] / mm_per("pt");
        bool const bold = text->style &&
                          (text->style->font_weight.as_enum() >= SP_CSS_FONT_WEIGHT_600 ||
                           text->style->font_weight.as_enum() == SP_CSS_FONT_WEIGHT_BOLD);
        bool const italic =
            text->style && text->style->font_style.as_enum() != SP_CSS_FONT_STYLE_NORMAL;

        std::string out = R"({"kind":"text","x":)" + json_number(point.x());
        out += R"(,"y":)" + json_number(point.y());
        out += R"(,"align":")" + std::string(align) + '"';
        out += R"(,"spans":[{"text":)" + json_string(content.raw());
        out += R"(,"family":)" + json_string(family);
        out += R"(,"size_pt":)" + json_number(size_pt);
        out += R"(,"bold":)" + std::string(bold ? "true" : "false");
        out += R"(,"italic":)" + std::string(italic ? "true" : "false");
        out += "}]";
        out.push_back(',');
        out += shape_common(text, fill, stroke, fill_alpha, stroke_alpha, true);
        out.push_back('}');
        return out;
    }

    if (is<SPFlowtext>(item)) {
        return fallback_path(item, full, "flowed text exported as outlines", id);
    }

    if (auto *group = cast<SPGroup>(item)) {
        std::string const children = group_children(group, extra, page, 0);
        if (children.empty()) {
            return {}; // an empty group is dropped
        }
        std::string out = R"({"kind":"group","children":[)" + children + "],";
        out += shape_common(group, "null", "null", 1.0, 1.0, false);
        out.push_back('}');
        return out;
    }

    // Every remaining shape: star, spiral, offset, polygon, plain path, and
    // any live path effect (the curve already carries the result).
    if (has_path_effect) {
        warn(id, "path effect baked into a path");
    }
    double fill_alpha = 1.0;
    double stroke_alpha = 1.0;
    auto const fill = paint_of(item->style->fill, item, fill_alpha);
    auto const stroke = stroke_of(item, full, stroke_alpha);
    if (fill == "null" && stroke == "null") {
        return {};
    }
    auto path = item_curves(item, full, fill, stroke, fill_alpha, stroke_alpha);
    return path;
}

/// The model JSON of a parent's item children, comma separated.
std::string Model::group_children(SPObject *parent, Geom::Affine const &extra, int page, int depth)
{
    if (depth > 16) {
        return {};
    }
    std::string out;
    for (auto &child : parent->children) {
        if (is<SPDefs>(&child) || is<SPNamedView>(&child) || is<SPMetadata>(&child)) {
            continue;
        }
        auto *item = cast<SPItem>(&child);
        if (!item || item->isHidden()) {
            continue;
        }
        auto shape = item_shape(item, extra, page);
        if (shape.empty()) {
            continue;
        }
        if (!out.empty()) {
            out.push_back(',');
        }
        out += shape;
    }
    return out;
}

} // namespace

Result build_json(SPDocument *doc)
{
    if (!doc) {
        return {R"({"schema":"rosepen-cdr-export/1","title":"","inks":[],"pages":[],"master":[]})",
                {}};
    }
    Model model(doc);
    return model.run();
}

} // namespace Inkscape::Extension::Internal::CdrExport

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
