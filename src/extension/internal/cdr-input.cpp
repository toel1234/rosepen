// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * This code abstracts the libwpg interfaces into the Inkscape
 * input extension interface.
 *
 * This file came from libwpg as a source, their utility wpg2svg
 * specifically.  It has been modified to work as an Inkscape extension.
 * The Inkscape extension code is covered by this copyright, but the
 * rest is covered by the one below.
 */
/* Authors:
 *   Fridrich Strba (fridrich.strba@bluewin.ch)
 *
 * Copyright (C) 2012 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

// WITH_LIBCDR lives in the generated configuration header; the Rust bridge's
// WITH_TRACECDR comes from the compile definitions.
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cdr-input.h"

#include <glib.h>

#include "document.h"
#include "extension/input.h"
#include "extension/internal/cdr-import-mapping.h"
#include "extension/internal/cdr-import-policy.h"
#include "extension/internal/cdr-source-colors.h"
#include "extension/internal/rvng-pages.h"
#include "extension/system.h"
#include "preferences.h"

#ifdef WITH_LIBCDR
#include <libcdr/libcdr.h>

#include "extension/internal/rvng-import-dialog.h"
#endif

#ifdef WITH_TRACECDR
#include "extension/internal/cdr-crate-import.h"
#endif

namespace Inkscape::Extension::Internal {

constexpr bool HAVE_CRATE =
#ifdef WITH_TRACECDR
    true;
#else
    false;
#endif

constexpr bool HAVE_LIBCDR =
#ifdef WITH_LIBCDR
    true;
#else
    false;
#endif

#ifdef WITH_LIBCDR
/**
 * The library reader's route (06-01): every page when there is no GUI, one page
 * of the user's choice when there is.
 */
static std::unique_ptr<SPDocument> open_with_libcdr(char const *uri)
{
    RvngOpenOptions options;
    options.all_pages_headless = true;
    return rvng_open(uri, libcdr::CDRDocument::isSupported, libcdr::CDRDocument::parse, options);
}
#endif

#ifdef WITH_TRACECDR
/**
 * The colour chain of a crate import: attach the preferred output profile,
 * apply the drawing's own CMYK and spot colours, then convert what is left.
 *
 * The profile and intent are the two preferences
 * CdrImport::map_to_preferred_output_profile() reads; the steps are called one
 * by one here because the source colours must land before the flat paints are
 * converted.
 */
static void map_crate_colours(SPDocument *doc)
{
    auto prefs = Inkscape::Preferences::get();
    auto const uri = prefs->getString("/options/document/outputprofile/uri");
    auto const intent = static_cast<Colors::RenderingIntent>(
        prefs->getInt("/options/document/outputprofile/intent"));
    CdrImport::attach_output_profile(doc, uri.raw(), intent);
    CdrImport::apply_source_colors(doc);
    CdrImport::convert_flat_paints(doc);
}
#endif

std::unique_ptr<SPDocument> CdrInput::open(Inkscape::Extension::Input *, char const *uri, bool is_importing)
{
    auto const prefs = Inkscape::Preferences::get();
    auto const engine = CdrImport::choose_engine(
        prefs->getString("/options/cdrimport/engine").raw(), g_getenv("ROSEPEN_CDR_IMPORTER"),
        HAVE_CRATE, HAVE_LIBCDR);
    [[maybe_unused]] bool use_libcdr = engine == CdrImport::Engine::LibCdr;

#ifdef WITH_TRACECDR
    if (engine != CdrImport::Engine::LibCdr) {
        auto outcome = CdrImport::open_with_crate(uri);
        CdrImport::CrateSummary summary{outcome.code, outcome.info.pages, outcome.info.shapes,
                                        outcome.info.skipped_objects, false};
        std::unique_ptr<SPDocument> doc;
        if (outcome.code == RP_OK) {
            auto result = RvngPages::document_from_pages(outcome.pages);
            for (int page : result.failed_pages) {
                if (page > 1) {
                    g_warning("rvng import: page %d could not be read; left empty", page);
                }
            }
            summary.page1_unreadable = !result.doc;
            doc = std::move(result.doc);
        }

        if (auto const reason = CdrImport::fallback_reason(summary)) {
            // Everything else is reported and kept: warnings and skipped
            // objects do not mean the library reader would do better.
            if (engine == CdrImport::Engine::Auto && HAVE_LIBCDR) {
                g_warning("cdr import: drawing reader failed (%s); trying libcdr", reason->c_str());
                use_libcdr = true;
            } else {
                g_warning("cdr import: drawing reader failed (%s)", reason->c_str());
                return nullptr;
            }
        } else {
            // The profile is stored as an href relative to the drawing's
            // directory, so the document must know its filename first.
            doc->setDocumentFilename(uri);
            if (is_importing) {
                // A free-standing import document has no output space to give
                // the source numbers: keep the sRGB preview, drop the markers.
                CdrImport::apply_source_colors(doc.get());
            } else {
                map_crate_colours(doc.get());
            }
            return doc;
        }
    }
#endif

#ifdef WITH_LIBCDR
    if (use_libcdr) {
        auto doc = open_with_libcdr(uri);
        // Opening a drawing imports every page and maps its flat colours into
        // the preferred output profile; importing into an existing document
        // leaves that document's own output profile in charge.
        if (doc) {
            doc->setDocumentFilename(uri);
            if (!is_importing) {
                CdrImport::map_to_preferred_output_profile(doc.get());
            }
        }
        return doc;
    }
#endif

    return nullptr;
}

#include "clear-n_.h"

void CdrInput::init()
{
    // clang-format off
    /* CDR */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("CDR Input") "</name>\n"
            "<id>org.inkscape.input.cdr</id>\n"
            "<input>\n"
                "<extension>.cdr</extension>\n"
                "<mimetype>image/x-xcdr</mimetype>\n"
                "<filetypename>" N_("CDR 7-X4 drawing files (*.cdr)") "</filetypename>\n"
                "<filetypetooltip>" N_("Open drawing files saved in CDR versions 7-X4") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<CdrInput>());
    /* CDT */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("CDR templates input") "</name>\n"
            "<id>org.inkscape.input.cdt</id>\n"
            "<input>\n"
                "<extension>.cdt</extension>\n"
                "<mimetype>application/x-xcdt</mimetype>\n"
                "<filetypename>" N_("CDR 7-13 template files (*.cdt)") "</filetypename>\n"
                "<filetypetooltip>" N_("Open template files saved in CDR versions 7-13") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<CdrInput>());
    /* CCX */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("CDR Compressed Exchange files input") "</name>\n"
            "<id>org.inkscape.input.ccx</id>\n"
            "<input>\n"
                "<extension>.ccx</extension>\n"
                "<mimetype>application/x-xccx</mimetype>\n"
                "<filetypename>" N_("CDR Compressed Exchange files (*.ccx)") "</filetypename>\n"
                "<filetypetooltip>" N_("Open compressed exchange files in CDR drawing format") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<CdrInput>());
    /* CMX */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("CDR Presentation Exchange files input") "</name>\n"
            "<id>org.inkscape.input.cmx</id>\n"
            "<input>\n"
                "<extension>.cmx</extension>\n"
                "<mimetype>application/x-xcmx</mimetype>\n"
                "<filetypename>" N_("CDR Presentation Exchange files (*.cmx)") "</filetypename>\n"
                "<filetypetooltip>" N_("Open presentation exchange files in CDR drawing format") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<CdrInput>());
    // clang-format on
}

} // namespace Inkscape::Extension::Internal

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
