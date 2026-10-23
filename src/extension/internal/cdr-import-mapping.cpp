// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Map the flat colours of an imported drawing into the document's output
 * (CMYK) profile.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "cdr-import-mapping.h"

#include <string>

#include <glib.h>
#include <glibmm/fileutils.h>

#include "colors/document-cms.h"
#include "colors/cms/profile.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "document-undo.h"
#include "object/color-profile.h"
#include "object/sp-defs.h"
#include "object/sp-metadata.h"
#include "object/sp-namedview.h"
#include "object/sp-object.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "preferences.h"
#include "style.h"
#include "style-internal.h"
#include "util/cast.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal::CdrImport {

namespace {

using Inkscape::Colors::DocumentCMS;

/**
 * Convert one set paint property of an object into the output space.
 *
 * Only a colour that is set on the object itself and still in its original
 * space is converted; inherited paints, paint servers, currentColor and
 * colours already in the output space are left alone.
 *
 * @returns 1 when the property was written, 0 otherwise.
 */
int convert_paint(SPObject *object, DocumentCMS &cms, char const *property, SPIPaint &paint)
{
    if (!paint.set || paint.inherit || !paint.isColor() || paint.isDerived() ||
        paint.paintOrigin != SP_CSS_PAINT_ORIGIN_NORMAL) {
        return 0;
    }

    auto const color = paint.getColor();
    auto const current = color.getSpace();
    auto const output = cms.getOutputSpace();
    if (current && output && (current == output || current->getName() == output->getName())) {
        return 0;
    }

    auto const converted = cms.convertToOutputSpace(color);
    if (!converted) {
        return 0;
    }

    SPCSSAttr *css = sp_repr_css_attr(object->getRepr(), "style");
    sp_repr_css_set_property(css, property, converted->toString().c_str());
    sp_repr_css_change(object->getRepr(), css, "style");
    sp_repr_css_attr_unref(css);
    return 1;
}

/// Convert the flat paints of an object and of its descendants, skipping defs.
int convert_object(SPObject *object, DocumentCMS &cms)
{
    if (is<SPDefs>(object) || is<SPNamedView>(object) || is<SPMetadata>(object)) {
        return 0;
    }

    int converted = 0;
    if (object->style) {
        converted += convert_paint(object, cms, "fill", object->style->fill);
        converted += convert_paint(object, cms, "stroke", object->style->stroke);
    }

    // The child of a clone is a view of the referenced element (often in defs).
    if (is<SPUse>(object)) {
        return converted;
    }

    for (auto &child : object->children) {
        converted += convert_object(&child, cms);
    }

    return converted;
}

} // namespace

std::optional<std::string> attach_output_profile(SPDocument *doc, std::string const &uri,
                                                 Colors::RenderingIntent intent)
{
    if (!doc || uri.empty() || !Glib::file_test(uri, Glib::FileTest::EXISTS)) {
        return {};
    }

    // Same sequence as InkscapeApplication::document_new.
    Inkscape::DocumentUndo::ScopedInsensitive no_undo(doc);
    if (auto profile = Colors::CMS::Profile::create_from_uri(uri)) {
        auto &cms = doc->getDocumentCMS();
        auto name = cms.attachProfileToDoc(*profile, ColorProfileStorage::HREF_FILE, intent);
        doc->ensureUpToDate();
        cms.setOutputProfile(name);
        return name;
    }

    g_warning("Output profile '%s' is not a valid ICC profile", uri.c_str());
    return {};
}

int convert_flat_paints(SPDocument *doc)
{
    if (!doc) {
        return 0;
    }

    auto &cms = doc->getDocumentCMS();
    if (!cms.getOutputSpace()) {
        return 0;
    }

    Inkscape::DocumentUndo::ScopedInsensitive no_undo(doc);
    return convert_object(doc->getRoot(), cms);
}

Mapping map_to_output_profile(SPDocument *doc, std::string const &uri, Colors::RenderingIntent intent)
{
    Mapping mapping;
    if (!doc) {
        return mapping;
    }

    mapping.profile = attach_output_profile(doc, uri, intent);
    if (mapping.profile) {
        mapping.converted = convert_flat_paints(doc);
    }
    return mapping;
}

Mapping map_to_preferred_output_profile(SPDocument *doc)
{
    if (!doc) {
        return {};
    }

    auto prefs = Inkscape::Preferences::get();
    auto uri = prefs->getString("/options/document/outputprofile/uri");
    auto intent = static_cast<Colors::RenderingIntent>(prefs->getInt("/options/document/outputprofile/intent"));
    return map_to_output_profile(doc, uri.raw(), intent);
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
