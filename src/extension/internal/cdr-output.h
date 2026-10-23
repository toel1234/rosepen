// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The native drawing-format output extension: an SPDocument through the
 * bridge's export model and writer.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_INTERNAL_CDR_OUTPUT_H
#define INKSCAPE_EXTENSION_INTERNAL_CDR_OUTPUT_H

#include "extension/implementation/implementation.h"

namespace Inkscape {
namespace Extension {
class Output;
namespace Internal {

/**
 * Write a drawing-format file (versions 7 to 27, default 14).
 *
 * Every feature the format cannot carry is turned into a named warning; a
 * failure to produce or write the file throws save_failed.
 */
class CdrOutput : public Inkscape::Extension::Implementation::Implementation {
public:
    void save(Inkscape::Extension::Output *mod, SPDocument *doc, gchar const *filename) override;

    static void init();
};

} // namespace Internal
} // namespace Extension
} // namespace Inkscape

#endif // INKSCAPE_EXTENSION_INTERNAL_CDR_OUTPUT_H

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
