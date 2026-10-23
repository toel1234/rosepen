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

#include "cdr-input.h"

#include <libcdr/libcdr.h>

#include "extension/input.h"
#include "extension/internal/cdr-import-mapping.h"
#include "extension/internal/rvng-import-dialog.h"
#include "extension/system.h"

namespace Inkscape::Extension::Internal {

std::unique_ptr<SPDocument> CdrInput::open(Inkscape::Extension::Input *, char const *uri, bool is_importing)
{
    RvngOpenOptions options;
    options.all_pages_headless = true;
    auto doc = rvng_open(uri, libcdr::CDRDocument::isSupported, libcdr::CDRDocument::parse, options);

    // Opening a drawing imports every page and maps its flat colours into the
    // preferred output profile; importing into an existing document leaves that
    // document's own output profile in charge.
    if (doc) {
        // The profile is stored as an href relative to the drawing's directory,
        // so the document must know its filename before the profile is attached.
        doc->setDocumentFilename(uri);
        if (!is_importing) {
            CdrImport::map_to_preferred_output_profile(doc.get());
        }
    }

    return doc;
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
