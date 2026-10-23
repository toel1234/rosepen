// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The native drawing-format output extension: an SPDocument through the
 * bridge's export model and writer.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "cdr-output.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <glib.h>

#include "extension/output.h"
#include "extension/system.h"
#include "tracecdr-ffi.h"

#include "cdr-export-model.h"

namespace Inkscape {
namespace Extension {
namespace Internal {

/**
 * Write @a doc as a drawing-format file.
 *
 * The model's fallbacks and the writer's own are announced as messages (an
 * export with fallbacks is still a success); a failure to build or write the
 * file is a warning and a save_failed.
 */
void CdrOutput::save(Inkscape::Extension::Output *mod, SPDocument *doc, gchar const *filename)
{
    auto const model = CdrExport::build_json(doc);
    for (auto const &warning : model.warnings) {
        g_message("%s", warning.c_str());
    }

    int const version = mod->get_param_int("version");
    if (version < 14) {
        g_warning("cdr export: version %d is experimental", version);
    }

    if (char const *dump = getenv("RP_DUMP_MODEL")) {
        std::fprintf(stderr, "MODEL[%s]: %s\n", dump, model.json.c_str());
    }

    uint8_t *bytes = nullptr;
    std::size_t length = 0;
    uint8_t *writer_warnings = nullptr;
    std::size_t warnings_length = 0;
    int const code = rp_cdr_export(reinterpret_cast<uint8_t const *>(model.json.data()),
                                   model.json.size(), static_cast<uint16_t>(version), &bytes,
                                   &length, &writer_warnings, &warnings_length);
    if (code != RP_OK) {
        g_warning("cdr export: %s", rp_strerror(code));
        throw Output::save_failed();
    }

    if (writer_warnings && warnings_length > 0) {
        std::string const lines(reinterpret_cast<char const *>(writer_warnings), warnings_length);
        std::size_t start = 0;
        while (start < lines.size()) {
            auto const end = lines.find('\n', start);
            if (end == std::string::npos) {
                g_message("%s", lines.substr(start).c_str());
                break;
            }
            g_message("%s", lines.substr(start, end - start).c_str());
            start = end + 1;
        }
    }

    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    bool const open_ok = static_cast<bool>(out);
    if (open_ok) {
        out.write(reinterpret_cast<char const *>(bytes), static_cast<std::streamsize>(length));
        out.close();
    }
    rp_free(bytes, length);
    rp_free(writer_warnings, warnings_length);

    if (!open_ok) {
        g_warning("cdr export: cannot open '%s' for writing", filename);
        throw Output::save_failed();
    }
    if (!out) {
        g_warning("cdr export: writing '%s' failed", filename);
        std::remove(filename); // a truncated file is no export
        throw Output::save_failed();
    }
}

#include "clear-n_.h"

/**
 * This is the definition of the drawing-format output.
 */
void CdrOutput::init()
{
    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("Drawing Output") "</name>\n"
            "<id>org.inkscape.output.cdr</id>\n"
            "<param name=\"version\" gui-text=\"" N_("File version") "\" type=\"int\" "
                "min=\"7\" max=\"27\">14</param>\n"
            "<output>\n"
                "<extension>.cdr</extension>\n"
                "<mimetype>image/x-xcdr</mimetype>\n"
                "<filetypename>" N_("CDR drawing (*.cdr)") "</filetypename>\n"
                "<filetypetooltip>" N_("A drawing file readable by the format's versions 7 to 27") "</filetypetooltip>\n"
                "<dataloss>true</dataloss>\n"
            "</output>\n"
        "</inkscape-extension>",
        std::make_unique<CdrOutput>());
    // clang-format on
}

} // namespace Internal
} // namespace Extension
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
