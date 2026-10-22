// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dimension tool: interactive creation of live Rosepen dimension objects.
 *
 * The tool only picks points and an offset; the document objects are created
 * through DimensionOps::create, the same entry point as app.dimension-add, so
 * the tool and the headless corpus produce identical objects.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLS_DIMENSION_TOOL_H
#define INKSCAPE_UI_TOOLS_DIMENSION_TOOL_H

#include <optional>
#include <string>

#include <2geom/point.h>

#include "display/control/canvas-item-ptr.h"
#include "ui/tools/tool-base.h"

namespace Inkscape {
class CanvasItemBpath;
class CanvasItemText;
} // namespace Inkscape

class SPItem;

namespace Inkscape::UI::Tools {

class DimensionTool final : public ToolBase
{
public:
    DimensionTool(SPDesktop *desktop);
    ~DimensionTool() override;

    bool root_handler(CanvasEvent const &event) override;

private:
    enum class Mode
    {
        Auto,
        Parallel,
        Angular,
        Segment
    };

    enum class State
    {
        Idle,         ///< waiting for the first point
        First,        ///< first point picked, waiting for the second
        Second,       ///< two points picked, waiting for the offset
        Vertex,       ///< angular: vertex picked, waiting for arm A
        ArmA,         ///< angular: arm A picked, waiting for arm B
        ArmB          ///< angular: all points picked, waiting for the radius
    };

    struct Captured
    {
        std::string id;      ///< measured object id, empty for a free point
        std::string anchor;  ///< anchor spec, empty for a free point
        Geom::Point doc;     ///< document coordinates (px)
        Geom::Point uu;      ///< document user units (for free points)
    };

    Captured capture(Geom::Point const &window_pos, Geom::Point const &snapped_desktop) const;
    std::optional<Geom::Point> snap(Geom::Point const &window_pos, unsigned modifiers) const;

    void reset_state();
    void cancel();
    void clear_preview();
    void update_preview(Geom::Point const &window_pos, unsigned modifiers);
    void commit(Mode mode);

    bool commit_segment(Geom::Point const &window_pos);
    std::optional<std::pair<int, int>> segment_nodes(SPItem *item, Geom::Point const &window_pos) const;

    Mode mode_from_prefs() const;
    double offset_mm_from_cursor(Mode mode, Geom::Point const &cursor_doc) const;

    static double to_mm(double px);

    Mode _mode = Mode::Auto;
    State _state = State::Idle;
    bool _dragging = false;

    Captured _p1;
    Captured _p2;
    Captured _vertex;
    Captured _arm_a;
    Captured _arm_b;
    double _offset_mm = 5.0;

    CanvasItemPtr<CanvasItemBpath> _preview_path;
    CanvasItemPtr<CanvasItemText> _preview_text;
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_DIMENSION_TOOL_H

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
