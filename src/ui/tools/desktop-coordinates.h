// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Window → document coordinate conversion for canvas tools.
 *
 * Event positions arrive in window (canvas) coordinates. Converting them
 * straight to document coordinates with dt2doc() ignores zoom and scroll;
 * they must pass through the view affine first (w2d), then the document
 * scale (dt2doc). The Artistic Media tool regressed exactly this way
 * (07-01): strokes landed offset whenever the view was zoomed or scrolled.
 *
 * Depends only on lib2geom and a duck-typed desktop, so it is fully
 * unit-testable without a display.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_TOOLS_DESKTOP_COORDINATES_H
#define INKSCAPE_UI_TOOLS_DESKTOP_COORDINATES_H

#include <2geom/point.h>

namespace Inkscape::UI::Tools {

/**
 * Maps window (event) coordinates to document coordinates through the
 * desktop: view transform first (w2d), then document scale (dt2doc).
 *
 * saveDragOrigin()/checkDragMoved() style comparisons intentionally keep
 * raw window coordinates — they compare screen distances.
 */
template <typename Desktop>
Geom::Point doc_point_from_window(Desktop const &desktop, Geom::Point const &pos)
{
    return desktop.dt2doc(desktop.w2d(pos));
}

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_DESKTOP_COORDINATES_H
