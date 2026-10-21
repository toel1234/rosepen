// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Display copies of master layers on pages 2..n for one Drawing.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef INK_OBJECT_MASTER_LAYER_DISPLAY_H
#define INK_OBJECT_MASTER_LAYER_DISPLAY_H

#include <utility>
#include <vector>

#include <2geom/affine.h>
#include <2geom/point.h>
#include <sigc++/scoped_connection.h>

class SPDocument;
class SPGroup;
class SPItem;
class SPObject;
class SPPage;

namespace Inkscape {

class DrawingGroup;

/**
 * Per-Drawing display of master layers on every page after the first.
 *
 * Copies are display-only: one insensitive DrawingGroup per master layer and
 * page, appended at the end of that layer's own DrawingGroup. The wrapper
 * transform translates the layer content by the page offset, expressed in the
 * layer's own coordinate system.
 */
class MasterLayerDisplay
{
public:
    struct Copy
    {
        SPGroup *layer = nullptr;
        DrawingGroup *wrapper = nullptr;
        unsigned key = 0;
        std::vector<SPItem *> items;
    };

    MasterLayerDisplay(SPDocument &document, unsigned dkey);
    ~MasterLayerDisplay();

    MasterLayerDisplay(MasterLayerDisplay const &) = delete;
    MasterLayerDisplay &operator=(MasterLayerDisplay const &) = delete;

    void refresh();
    void clear();

    std::vector<Copy> const &copies() const { return _copies; }

private:
    void rebuild();
    void update_transforms(std::vector<std::pair<SPPage *, Geom::Point>> const &offsets);
    void on_release();

    SPDocument &_doc;
    unsigned _dkey;

    std::vector<Copy> _copies;

    sigc::scoped_connection _document_modified;
    sigc::scoped_connection _pages_changed;
    std::vector<sigc::scoped_connection> _release_connections;

    // Fingerprint of the state the copies were built from.
    std::vector<SPGroup *> _layers;
    std::vector<unsigned> _positions;
    std::vector<Geom::Affine> _transforms;
    std::vector<std::vector<SPItem *>> _children;
    std::vector<SPPage *> _pages;
    std::vector<Geom::Point> _offsets;
};

} // namespace Inkscape

#endif // INK_OBJECT_MASTER_LAYER_DISPLAY_H

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
