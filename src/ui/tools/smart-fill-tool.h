// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Smart Fill tool: create a new object covering the area under the cursor.
 *
 * The tool only picks the point; the document object is created through
 * SmartFillOps::create, the same entry point as app.smart-fill, so the tool
 * and the headless corpus produce identical objects.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLS_SMART_FILL_TOOL_H
#define INKSCAPE_UI_TOOLS_SMART_FILL_TOOL_H

#include <2geom/point.h>

#include "ui/tools/tool-base.h"

namespace Inkscape::UI::Tools {

class SmartFillTool final : public ToolBase
{
public:
    SmartFillTool(SPDesktop *desktop);
    ~SmartFillTool() override;

    bool root_handler(CanvasEvent const &event) override;

private:
    void click(Geom::Point const &position_window);

    bool _pressed = false;
    bool _dragged = false;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_SMART_FILL_TOOL_H

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
