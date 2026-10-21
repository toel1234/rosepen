// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Display copies of master layers on pages 2..n for one Drawing.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "master-layer-display.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <glib.h>

#include <2geom/transforms.h>

#include "document.h"
#include "display/drawing-group.h"
#include "display/drawing-item.h"
#include "display/drawing.h"
#include "object/master-layer.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-object.h"
#include "object/sp-page.h"
#include "page-manager.h"

namespace Inkscape {

MasterLayerDisplay::MasterLayerDisplay(SPDocument &document, unsigned dkey)
    : _doc(document)
    , _dkey(dkey)
{
    _document_modified = _doc.connectModified([this](unsigned) { refresh(); });
    _pages_changed = _doc.getPageManager().connectPagesChanged([this](SPPage *) { refresh(); });
    refresh();
}

MasterLayerDisplay::~MasterLayerDisplay()
{
    clear();
    _release_connections.clear();
    _document_modified.disconnect();
    _pages_changed.disconnect();
}

/**
 * Reconcile the display copies with the current document state.
 *
 * A structural change (master set, layer order, layer transform, direct child
 * list or page list) rebuilds the copies. A change of just the page offsets
 * updates the wrapper transforms in place.
 */
void MasterLayerDisplay::refresh()
{
    auto const layers = MasterLayer::master_layers(&_doc);
    auto const offsets = MasterLayer::page_offsets(&_doc);

    std::vector<unsigned> positions;
    std::vector<Geom::Affine> transforms;
    std::vector<std::vector<SPItem *>> children;
    positions.reserve(layers.size());
    transforms.reserve(layers.size());
    children.reserve(layers.size());

    for (auto *layer : layers) {
        positions.push_back(layer->pos_in_parent());
        transforms.push_back(layer->transform);

        std::vector<SPItem *> items;
        for (auto &child : layer->children) {
            if (auto *item = cast<SPItem>(&child)) {
                items.push_back(item);
            }
        }
        children.push_back(std::move(items));
    }

    std::vector<SPPage *> pages;
    std::vector<Geom::Point> points;
    pages.reserve(offsets.size());
    points.reserve(offsets.size());
    for (auto const &[page, offset] : offsets) {
        pages.push_back(page);
        points.push_back(offset);
    }

    bool const structure_changed = layers != _layers || positions != _positions
                                   || transforms != _transforms || children != _children
                                   || pages != _pages;
    if (structure_changed) {
        rebuild();
        return;
    }

    if (points != _offsets) {
        update_transforms(offsets);
        _offsets = std::move(points);
    }
}

/**
 * Remove every display copy. Must run before the owner hides the root, while
 * all copied objects and their drawing items are still alive.
 */
void MasterLayerDisplay::clear()
{
    for (auto &copy : _copies) {
        for (auto *item : copy.items) {
            item->invoke_hide(copy.key);
        }
        copy.wrapper->unlink();
    }

    _copies.clear();

    _layers.clear();
    _positions.clear();
    _transforms.clear();
    _children.clear();
    _pages.clear();
    _offsets.clear();
}

void MasterLayerDisplay::rebuild()
{
    clear();
    _release_connections.clear();

    auto const offsets = MasterLayer::page_offsets(&_doc);

    for (auto *layer : MasterLayer::master_layers(&_doc)) {
        auto *layer_group = dynamic_cast<DrawingGroup *>(layer->get_arenaitem(_dkey));
        if (!layer_group) {
            continue;
        }

        _release_connections.emplace_back(layer->connectRelease([this](SPObject *) { on_release(); }));

        std::vector<SPItem *> items;
        for (auto &child : layer->children) {
            if (auto *item = cast<SPItem>(&child)) {
                items.push_back(item);
            }
        }

        auto &drawing = layer_group->drawing();
        auto const layer_transform = layer->transform;

        for (auto const &[page, offset] : offsets) {
            auto *wrapper = new DrawingGroup(drawing);
            wrapper->setTransform(layer_transform * Geom::Translate(offset) * layer_transform.inverse());
            wrapper->setSensitive(false);
            layer_group->appendChild(wrapper);

            Copy copy;
            copy.layer = layer;
            copy.wrapper = wrapper;
            copy.key = SPItem::display_key_new(1);
            copy.items = items;
            for (auto *item : items) {
                if (auto *drawing_item = item->invoke_show(drawing, copy.key, SP_ITEM_SHOW_DISPLAY)) {
                    wrapper->appendChild(drawing_item);
                }
            }
            _copies.push_back(std::move(copy));
        }

        for (auto *item : items) {
            _release_connections.emplace_back(item->connectRelease([this](SPObject *) { on_release(); }));
        }
    }

    auto const layers = MasterLayer::master_layers(&_doc);
    _layers = layers;
    for (auto *layer : layers) {
        _positions.push_back(layer->pos_in_parent());
        _transforms.push_back(layer->transform);
        std::vector<SPItem *> items;
        for (auto &child : layer->children) {
            if (auto *item = cast<SPItem>(&child)) {
                items.push_back(item);
            }
        }
        _children.push_back(std::move(items));
    }
    for (auto const &[page, offset] : offsets) {
        _pages.push_back(page);
        _offsets.push_back(offset);
    }
}

void MasterLayerDisplay::update_transforms(std::vector<std::pair<SPPage *, Geom::Point>> const &offsets)
{
    std::size_t index = 0;
    for (auto *layer : _layers) {
        auto const layer_transform = layer->transform;
        for (auto const &[page, offset] : offsets) {
            if (index >= _copies.size() || _copies[index].layer != layer) {
                break;
            }
            _copies[index].wrapper->setTransform(layer_transform * Geom::Translate(offset)
                                                 * layer_transform.inverse());
            ++index;
        }
    }
}

void MasterLayerDisplay::on_release()
{
    // SPObject emits this before release(), so every copied object is still
    // alive here and clear() can unbind its views safely.
    clear();
}

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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
