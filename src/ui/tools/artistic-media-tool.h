// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Artistic Media tool: interactive creation of live Rosepen brush strokes.
 *
 * A press-drag records a raw polyline of document points; the preview shows
 * the brush (preset/brush shape mapped along it, or the flat-nib sweep)
 * computed with the same pure model the effect uses. On release the stroke is
 * committed through ArtisticOps::create, the same entry point as
 * app.artistic-add, so tool and headless corpus produce identical objects.
 * A click without drag selects the item under the cursor so the property bar
 * can edit it.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLS_ARTISTIC_MEDIA_TOOL_H
#define INKSCAPE_UI_TOOLS_ARTISTIC_MEDIA_TOOL_H

#include <vector>

#include <2geom/point.h>

#include "display/control/canvas-item-ptr.h"
#include "ui/tools/tool-base.h"

namespace Inkscape {
class CanvasItemBpath;
} // namespace Inkscape

namespace Inkscape::UI::Tools {

class ArtisticMediaTool final : public ToolBase
{
public:
    ArtisticMediaTool(SPDesktop *desktop);
    ~ArtisticMediaTool() override;

    bool root_handler(CanvasEvent const &event) override;

private:
    void capture_point(Geom::Point const &position_doc);
    void clear_preview();
    void update_preview();
    void cancel();
    void commit();
    void click_select(Geom::Point const &window_pos);

    std::vector<Geom::Point> _points;    ///< raw polyline, document user units
    bool _dragging = false;

    CanvasItemPtr<CanvasItemBpath> _preview_path;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_ARTISTIC_MEDIA_TOOL_H

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
