// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Live Sketch tool: freehand strokes smoothed live into plain editable paths.
 *
 * While the user drags, every captured position feeds a windowed fitter whose
 * smooth curve (not the raw mouse trail) follows the cursor. On release the
 * whole captured polyline is committed through LiveSketchOps::create, the
 * same entry point as app.live-sketch, so the tool and the headless corpus
 * produce identical objects. A click without drag creates nothing.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLS_LIVE_SKETCH_TOOL_H
#define INKSCAPE_UI_TOOLS_LIVE_SKETCH_TOOL_H

#include <optional>
#include <vector>

#include <2geom/point.h>

#include "display/control/canvas-item-ptr.h"
#include "path/live-sketch-geometry.h"
#include "ui/tools/tool-base.h"

namespace Inkscape {
class CanvasItemBpath;
} // namespace Inkscape

namespace Inkscape::UI::Tools {

class LiveSketchTool final : public ToolBase
{
public:
    LiveSketchTool(SPDesktop *desktop);
    ~LiveSketchTool() override;

    bool root_handler(CanvasEvent const &event) override;

private:
    void reset();
    bool capture(Geom::Point const &pos_window);
    void clear_preview();
    void update_preview();
    void cancel();
    void commit();

    std::optional<LiveSketch::LiveFitter> _fitter; ///< live preview fitter, document px
    std::vector<Geom::Point> _points_uu;           ///< captured polyline, document user units
    Geom::Point _last_capture_window_pos;          ///< last accepted capture, window px
    bool _dragging = false;

    CanvasItemPtr<CanvasItemBpath> _preview_path;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_LIVE_SKETCH_TOOL_H

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
