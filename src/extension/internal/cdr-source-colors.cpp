// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Turn the source colours an imported drawing carries into document paints:
 * CMYK and spot inks instead of their sRGB preview.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "cdr-source-colors.h"

#include <charconv>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>

#include "colors/color.h"
#include "colors/document-cms.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/enum.h"
#include "colors/spot-swatch.h"
#include "document.h"
#include "document-undo.h"
#include "object/sp-defs.h"
#include "object/sp-gradient.h"
#include "object/sp-metadata.h"
#include "object/sp-namedview.h"
#include "object/sp-object.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "style.h"
#include "util/cast.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal::CdrImport {

namespace {

using Inkscape::Colors::Space::Type;

/// One parsed data-rp-* value.
struct SourcePaint
{
    bool spot = false;
    double tint = 1.0;
    double c = 0.0;
    double m = 0.0;
    double y = 0.0;
    double k = 0.0;
    std::string ink; ///< percent-decoded ink name, for a spot
};

/// The inverse of the reader's percent-encoding.
std::string percent_decode(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            unsigned byte = 0;
            auto const hex = value.substr(i + 1, 2);
            auto const result = std::from_chars(hex.data(), hex.data() + 2, byte, 16);
            if (result.ec == std::errc() && result.ptr == hex.data() + 2) {
                out.push_back(static_cast<char>(byte));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

/// Read one number, advancing @a pos past it.
bool next_number(std::string_view text, std::size_t &pos, double &out)
{
    auto const first = text.data() + pos;
    auto const last = text.data() + text.size();
    auto const result = std::from_chars(first, last, out);
    if (result.ec != std::errc() || result.ptr == first) {
        return false;
    }
    pos = static_cast<std::size_t>(result.ptr - text.data());
    return out >= 0.0 && out <= 1.0;
}

/// Read a comma, if one follows.
bool comma(std::string_view text, std::size_t &pos)
{
    if (pos < text.size() && text[pos] == ',') {
        ++pos;
        return true;
    }
    return false;
}

/**
 * Parse "cmyk:c,m,y,k" or "spot:tint,c,m,y,k,<percent-encoded ink name>".
 *
 * Strict: every number is 0..1, the separators are exactly the commas the
 * grammar has, and nothing may follow the last field.
 */
std::optional<SourcePaint> parse_source_paint(std::string_view value)
{
    SourcePaint paint;
    std::size_t pos = 0;
    if (value.substr(0, 5) == "cmyk:") {
        pos = 5;
    } else if (value.substr(0, 5) == "spot:") {
        pos = 5;
        paint.spot = true;
        if (!next_number(value, pos, paint.tint) || !comma(value, pos)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    if (!next_number(value, pos, paint.c) || !comma(value, pos) ||
        !next_number(value, pos, paint.m) || !comma(value, pos) ||
        !next_number(value, pos, paint.y) || !comma(value, pos) ||
        !next_number(value, pos, paint.k)) {
        return std::nullopt;
    }

    if (paint.spot) {
        if (!comma(value, pos) || pos >= value.size()) {
            return std::nullopt;
        }
        paint.ink = percent_decode(value.substr(pos));
        if (paint.ink.empty()) {
            return std::nullopt;
        }
    } else if (pos != value.size()) {
        return std::nullopt;
    }
    return paint;
}

/// The `#rrggbb` preview of a paint (the first token, when it already has one).
std::string preview_hex(Colors::Color const &color)
{
    auto const text = color.toString(false);
    auto const space = text.find(' ');
    return space == std::string::npos ? text : text.substr(0, space);
}

/// `icc-color(<profile>, c, m, y, k)`, formatted the way Inkscape writes it.
std::string icc_paint(std::shared_ptr<Colors::Space::AnySpace> const &space,
                      std::vector<double> values)
{
    Colors::Color const color(space, std::move(values));
    auto const text = color.toString(false);
    auto const space_at = text.find(' ');
    return space_at == std::string::npos ? std::string{} : text.substr(space_at + 1);
}

/// Write one paint property on an object, keeping the rest of its style.
void write_property(SPObject *object, char const *property, std::string const &value)
{
    SPCSSAttr *css = sp_repr_css_attr(object->getRepr(), "style");
    sp_repr_css_set_property(css, property, value.c_str());
    sp_repr_css_change(object->getRepr(), css, "style");
    sp_repr_css_attr_unref(css);
}

struct Walker
{
    SPDocument *doc;
    std::shared_ptr<Colors::Space::AnySpace> output;
    SourceColorStats stats;
};

/// Apply one data-rp-* attribute of an object; returns nothing.
void apply_paint(Walker &walker, SPObject *object, char const *property, char const *attribute)
{
    auto *repr = object->getRepr();
    char const *raw = repr->attribute(attribute);
    if (raw == nullptr) {
        return;
    }

    // Whatever happens, the annotation does not belong in the document.
    repr->removeAttribute(attribute);

    auto const paint = parse_source_paint(raw);
    if (!paint) {
        g_info("cdr import: ignoring malformed source colour %s=\"%s\"", attribute, raw);
        ++walker.stats.untouched;
        return;
    }

    SPIPaint *current = nullptr;
    if (object->style) {
        if (std::string_view(property) == "fill") {
            current = &object->style->fill;
        } else {
            current = &object->style->stroke;
        }
    }
    // The reader annotates flat paints only; anything else keeps its paint.
    if (current == nullptr || !walker.output || !current->isColor()) {
        ++walker.stats.untouched;
        return;
    }

    // The paints keep the reader's sRGB preview as their fallback, so the
    // document looks the same while the exact numbers travel beside it.
    auto const hex = preview_hex(current->getColor());

    if (!paint->spot) {
        auto const value = hex + " " + icc_paint(walker.output, {paint->c, paint->m, paint->y, paint->k});
        write_property(object, property, value);
        ++walker.stats.cmyk;
        return;
    }

    std::vector<double> stand_in = {paint->c, paint->m, paint->y, paint->k};
    if (paint->tint >= 1.0) {
        Colors::Color const swatch_alternate(walker.output, stand_in);
        if (auto *spot = Colors::Spot::ensure_spot(walker.doc, paint->ink, swatch_alternate)) {
            write_property(object, property, std::string("url(#") + spot->getId() + ")");
            ++walker.stats.spot;
            return;
        }
        ++walker.stats.untouched;
        return;
    }

    for (auto &component : stand_in) {
        component *= paint->tint;
    }
    write_property(object, property, hex + " " + icc_paint(walker.output, stand_in));
    ++walker.stats.flattened;
}

void walk(Walker &walker, SPObject *object)
{
    if (is<SPDefs>(object) || is<SPNamedView>(object) || is<SPMetadata>(object)) {
        return;
    }

    apply_paint(walker, object, "fill", "data-rp-fill");
    apply_paint(walker, object, "stroke", "data-rp-stroke");

    // The child of a clone is a view of the referenced element (often in defs).
    if (is<SPUse>(object)) {
        return;
    }

    for (auto &child : object->children) {
        walk(walker, &child);
    }
}

} // namespace

SourceColorStats apply_source_colors(SPDocument *doc)
{
    if (!doc) {
        return {};
    }

    auto &cms = doc->getDocumentCMS();
    // getType() of a CMS space is Type::CMS; the profile's component type is
    // what says whether the output space is CMYK.
    auto output = cms.getOutputSpace();
    if (output && output->getComponentType() != Type::CMYK) {
        output = nullptr;
    }

    Walker walker{doc, std::move(output), {}};
    Inkscape::DocumentUndo::ScopedInsensitive no_undo(doc);
    walk(walker, doc->getRoot());

    if (walker.stats.flattened > 0) {
        g_message("cdr import: %d spot colour(s) flattened to their process stand-in "
                  "(a spot tint is not kept)", walker.stats.flattened);
    }
    return walker.stats;
}

} // namespace Inkscape::Extension::Internal::CdrImport

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
