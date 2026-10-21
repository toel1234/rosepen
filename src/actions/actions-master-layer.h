// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for master layers, application level and headless.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INK_ACTIONS_MASTER_LAYER_H
#define INK_ACTIONS_MASTER_LAYER_H

#include <string>

class SPDocument;
class InkscapeApplication;

namespace Inkscape {

/**
 * Format the layer-master-list output: one "layer" line per master layer,
 * followed by one "offset" line per page after the first.
 */
std::string master_layer_list(SPDocument *doc);

} // namespace Inkscape

void add_actions_master_layer(InkscapeApplication *app);

#endif // INK_ACTIONS_MASTER_LAYER_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
