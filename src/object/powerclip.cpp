// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PowerClip: a frame object that owns and clips its contents.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "powerclip.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>

#include <2geom/affine.h>

#include "document.h"
#include "gc-anchored.h"
#include "object-set.h"
#include "sp-clippath.h"
#include "sp-item-group.h"
#include "sp-item.h"
#include "sp-object.h"
#include "sp-root.h"
#include "sp-use.h"
#include "util/cast.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::PowerClip {

namespace {

bool has_role(SPObject const *object, char const *role)
{
    return object && g_strcmp0(object->getAttribute(attr), role) == 0;
}

/**
 * Move a live repr to another parent, keeping the item where it is on the page.
 *
 * The repr is anchored across the removal so the XML node survives while its
 * old SPObject is torn down; the new SPObject is looked up from the document
 * afterwards. `old_i2doc` must be the item's document transform before the move.
 */
void reparent_keep_visual(SPDocument *doc, Inkscape::XML::Node *repr, Geom::Affine const &old_i2doc,
                          Inkscape::XML::Node *new_parent, Inkscape::XML::Node *after)
{
    if (!doc || !repr || !new_parent) {
        return;
    }
    auto *old_parent = repr->parent();
    if (!old_parent) {
        return;
    }

    Inkscape::GC::anchor(repr);
    old_parent->removeChild(repr);
    if (after) {
        new_parent->addChild(repr, after);
    } else {
        new_parent->appendChild(repr);
    }

    // No compensation: re-parenting is not a user transform, so the
    // stroke/pattern/gradient scaling preferences must not apply.
    auto *moved = cast<SPItem>(doc->getObjectByRepr(repr));
    auto *parent_item = cast<SPItem>(doc->getObjectByRepr(new_parent));
    if (moved && parent_item) {
        moved->doWriteTransform(old_i2doc * parent_item->i2doc_affine().inverse(), nullptr, false);
    }
    Inkscape::GC::release(repr);
}

/** Create a clipPath holding a single <use> of `container_id`; returns its id. */
std::string create_live_clip(SPDocument *doc, std::string const &container_id)
{
    auto *xml_doc = doc->getReprDoc();
    auto *use = xml_doc->createElement("svg:use");
    use->setAttribute("x", "0");
    use->setAttribute("y", "0");
    std::string const href = "#" + container_id;
    use->setAttribute("xlink:href", href.c_str());

    std::vector<Inkscape::XML::Node *> reprs{use};
    std::string clip_id = SPClipPath::create(reprs, doc);
    Inkscape::GC::release(use);
    return clip_id;
}

// A valid contents clip is private to this frame and is an untransformed
// <use> of its own container. Anything else is stale, shared with a
// duplicate, or edited. (Upstream clone compensation, SPUse::move_compensate,
// counter-moves the <use> when the container alone is translated.)
bool clip_matches(SPClipPath *clip, std::string const &container_id)
{
    if (!clip || clip->hrefcount != 1) {
        return false;
    }

    SPObject *only = nullptr;
    for (auto &child : clip->children) {
        if (!is<SPItem>(&child) || only) {
            return false;
        }
        only = &child;
    }
    auto *use = cast<SPUse>(only);
    if (!use || !use->transform.isIdentity(1e-9) || use->x.computed != 0 || use->y.computed != 0) {
        return false;
    }

    auto const *href = use->getAttribute("xlink:href");
    return href && href[0] == '#' && container_id == (href + 1);
}

SPGroup *build_frame(SPDocument *doc, SPItem *container, std::string const &container_id)
{
    auto *xml_doc = doc->getReprDoc();
    auto *container_repr = container->getRepr();
    auto *parent_repr = container_repr->parent();
    if (!parent_repr) {
        return nullptr;
    }

    auto *frame_repr = xml_doc->createElement("svg:g");
    frame_repr->setAttribute(attr, role_frame);
    parent_repr->addChild(frame_repr, container_repr);
    auto *frame = cast<SPGroup>(doc->getObjectByRepr(frame_repr));
    Inkscape::GC::release(frame_repr);
    if (!frame) {
        return nullptr;
    }

    // Move the container into the frame. The frame sits where the container
    // was, and the container keeps its own transform and its id, so every
    // existing reference (clones, textPath, the clip use) stays valid.
    Inkscape::GC::anchor(container_repr);
    parent_repr->removeChild(container_repr);
    frame_repr->appendChild(container_repr);
    Inkscape::GC::release(container_repr);

    auto *moved_container = cast<SPItem>(doc->getObjectByRepr(container_repr));
    if (!moved_container) {
        return nullptr;
    }
    moved_container->setAttribute(attr, role_container);

    auto *contents_repr = xml_doc->createElement("svg:g");
    contents_repr->setAttribute(attr, role_contents);
    frame_repr->appendChild(contents_repr);
    auto *contents = cast<SPGroup>(doc->getObjectByRepr(contents_repr));
    Inkscape::GC::release(contents_repr);
    if (!contents) {
        return nullptr;
    }

    std::string const clip_id = create_live_clip(doc, container_id);
    std::string const value = "url(#" + clip_id + ")";
    contents->setAttribute("clip-path", value.c_str());

    return frame;
}

// Returns the reprs of the extracted contents: their SPItems are rebuilt if a
// later (outer) extraction moves them again.
std::vector<Inkscape::XML::Node *> extract_frame(SPGroup *frame)
{
    auto *doc = frame->document;
    auto *container = container_of(frame);
    auto *contents = contents_of(frame);
    if (!doc || !container || !contents) {
        g_warning("%s: frame %s is missing its container or contents", attr,
                  frame->getId() ? frame->getId() : "?");
        return {};
    }

    auto *parent_obj = frame->parent;
    if (!parent_obj) {
        return {};
    }
    doc->ensureUpToDate();

    auto *frame_repr = frame->getRepr();
    auto *parent_repr = parent_obj->getRepr();

    // Record everything before touching the tree: moving an ancestor rebuilds
    // the SPObjects of its descendants, so only the reprs stay stable. Items
    // that ended up directly in the frame group (e.g. drawn after entering it
    // with a double-click) come out too, in the frame's stacking order;
    // deleting the frame must never delete user objects.
    auto *container_repr = container->getRepr();
    std::vector<std::pair<Inkscape::XML::Node *, Geom::Affine>> order;
    for (auto &frame_child : frame->children) {
        if (&frame_child == contents) {
            for (auto &child : contents->children) {
                if (auto *item = cast<SPItem>(&child)) {
                    order.emplace_back(item->getRepr(), item->i2doc_affine());
                }
            }
        } else if (auto *item = cast<SPItem>(&frame_child)) {
            order.emplace_back(item->getRepr(), item->i2doc_affine());
        }
    }

    std::vector<Inkscape::XML::Node *> extracted;
    Inkscape::XML::Node *after = frame_repr;
    for (auto const &[repr, old_i2doc] : order) {
        reparent_keep_visual(doc, repr, old_i2doc, parent_repr, after);
        after = repr;
        if (repr != container_repr) {
            extracted.push_back(repr);
        }
    }

    if (auto *moved_container = cast<SPItem>(doc->getObjectByRepr(container_repr)); moved_container) {
        moved_container->removeAttribute(attr);
    }

    SPClipPath *clip = contents->getClipObject();
    contents->removeAttribute("clip-path");
    if (clip && !clip->isReferenced()) {
        clip->deleteObject(false);
    }

    contents->deleteObject(false);
    frame->deleteObject(false);

    return extracted;
}

} // namespace

bool is_frame(SPObject const *object)
{
    return is<SPGroup>(object) && has_role(object, role_frame);
}

bool is_live_clip_use(SPUse const *use)
{
    // A live PowerClip clip is a <use> inside a clipPath whose original is the
    // frame's container (duplicated frames share that clip until normalize).
    // This is O(1): no document scan, no href string compare.
    return use && cast<SPClipPath>(use->parent) && has_role(use->get_original(), role_container);
}

SPGroup *frame_of(SPObject *object)
{
    for (auto *current = object; current; current = current->parent) {
        if (is_frame(current)) {
            return cast<SPGroup>(current);
        }
    }
    return nullptr;
}

SPItem *container_of(SPGroup *frame)
{
    if (!frame) {
        return nullptr;
    }
    for (auto &child : frame->children) {
        if (auto *item = cast<SPItem>(&child); item && has_role(item, role_container)) {
            return item;
        }
    }
    return nullptr;
}

SPGroup *contents_of(SPGroup *frame)
{
    if (!frame) {
        return nullptr;
    }
    for (auto &child : frame->children) {
        if (auto *group = cast<SPGroup>(&child); group && has_role(group, role_contents)) {
            return group;
        }
    }
    return nullptr;
}

std::vector<SPItem *> content_items(SPGroup *frame)
{
    auto *contents = contents_of(frame);
    return contents ? contents->item_list() : std::vector<SPItem *>{};
}

std::vector<SPGroup *> frames(SPDocument *doc)
{
    std::vector<SPGroup *> result;
    if (!doc) {
        return result;
    }

    std::vector<SPObject *> pending;
    if (auto *root = doc->getRoot()) {
        pending.push_back(root);
    }
    while (!pending.empty()) {
        auto *object = pending.back();
        pending.pop_back();
        if (is_frame(object)) {
            result.push_back(cast<SPGroup>(object));
        }
        for (auto &child : object->children) {
            pending.push_back(&child);
        }
    }
    std::sort(result.begin(), result.end(), [](SPGroup *a, SPGroup *b) {
        return sp_repr_compare_position_bool(a->getRepr(), b->getRepr());
    });
    return result;
}

char const *refusal_message(Refusal refusal)
{
    switch (refusal) {
        case Refusal::None:
            return "PowerClip: ok";
        case Refusal::TooFewItems:
            return "PowerClip: select at least two objects, the frame last";
        case Refusal::NoContainer:
            return "PowerClip: the frame has no container";
        case Refusal::ContainerIsLayer:
            return "PowerClip: a layer cannot be the frame";
        case Refusal::ContentIsLayer:
            return "PowerClip: a layer cannot be placed inside a PowerClip";
        case Refusal::ContainerInsideContent:
            return "PowerClip: the frame is inside the content";
        case Refusal::ContentContainsContainer:
            return "PowerClip: the content contains the frame";
        case Refusal::NoContents:
            return "PowerClip: nothing to place inside the frame";
        case Refusal::NotInFrame:
            return "PowerClip: not inside a PowerClip";
    }
    return "PowerClip: unknown refusal";
}

PlaceResult place_inside(ObjectSet &set)
{
    PlaceResult result;

    // 1. At least the contents and the frame.
    if (set.size() < 2) {
        result.refusal = Refusal::TooFewItems;
        return result;
    }

    SPDocument *doc = set.document();
    SPItem *target = set.lastItem();
    if (!doc || !target) {
        result.refusal = Refusal::TooFewItems;
        return result;
    }

    // 2. The last selected item is the frame. An existing frame (or its
    //    container, contents, or anything inside the container) means
    //    add-to-frame. A target INSIDE the contents is an ordinary item that
    //    becomes the container of a new, nested frame.
    SPGroup *frame = frame_of(target);
    if (frame) {
        auto *target_frame_contents = contents_of(frame);
        if (target_frame_contents && target_frame_contents != target
            && target_frame_contents->isAncestorOf(target)) {
            frame = nullptr;
        }
    }
    if (!frame && SP_IS_LAYER(target)) {
        result.refusal = Refusal::ContainerIsLayer;
        return result;
    }

    SPGroup *contents = frame ? contents_of(frame) : nullptr;
    SPItem *old_container = frame ? container_of(frame) : nullptr;
    if (frame && (!contents || !old_container)) {
        result.refusal = Refusal::NoContainer;
        return result;
    }

    // 3. Everything else selected goes inside, except the frame itself and
    //    items that are already inside its contents. The container or
    //    contents group of ANOTHER frame stands for that whole frame, so
    //    placing never tears a PowerClip apart.
    std::vector<SPItem *> content_list;
    for (auto *item : set.items()) {
        if (!item || item == target) {
            continue;
        }
        if ((has_role(item, role_container) || has_role(item, role_contents)) && is_frame(item->parent)) {
            item = cast<SPGroup>(item->parent);
            if (item == target) {
                continue;
            }
        }
        if (std::find(content_list.begin(), content_list.end(), item) != content_list.end()) {
            continue;
        }
        if (frame && (item == frame || item == old_container)) {
            continue;
        }
        if (contents && (item == contents || contents->isAncestorOf(item))) {
            continue;
        }
        content_list.push_back(item);
    }

    for (auto *item : content_list) {
        if (SP_IS_LAYER(item)) {
            result.refusal = Refusal::ContentIsLayer;
            return result;
        }
    }
    for (auto *item : content_list) {
        if (item->isAncestorOf(target)) {
            result.refusal = Refusal::ContainerInsideContent;
            return result;
        }
        if (target->isAncestorOf(item)) {
            result.refusal = Refusal::ContentContainsContainer;
            return result;
        }
    }
    if (content_list.empty()) {
        result.refusal = Refusal::NoContents;
        return result;
    }

    // 4. Record where every content is before the tree changes, in document
    //    z-order so the contents group keeps the stacking order.
    doc->ensureUpToDate();
    std::vector<std::pair<Inkscape::XML::Node *, Geom::Affine>> moves;
    for (auto *item : content_list) {
        moves.emplace_back(item->getRepr(), item->i2doc_affine());
    }
    std::sort(moves.begin(), moves.end(), [](auto const &a, auto const &b) {
        return sp_repr_compare_position_bool(a.first, b.first);
    });

    // 5. Build the frame when the target is not one yet.
    if (!frame) {
        std::string container_id = target->getId() ? target->getId() : "";
        if (container_id.empty()) {
            container_id = doc->generate_unique_id("powerclip-container");
            target->setAttribute("id", container_id.c_str());
        }
        frame = build_frame(doc, target, container_id);
        if (!frame) {
            result.refusal = Refusal::NoContainer;
            return result;
        }
        contents = contents_of(frame);
        if (!contents) {
            result.refusal = Refusal::NoContainer;
            return result;
        }
    }

    // 6. Move the contents to the end of the contents group, keeping the
    //    visual position. Reprs are stable across the moves.
    for (auto const &[repr, old_i2doc] : moves) {
        reparent_keep_visual(doc, repr, old_i2doc, contents->getRepr(), nullptr);
    }

    // 7. The frame becomes the selection.
    set.set(frame);
    result.frame = frame;
    return result;
}

std::vector<SPItem *> extract(ObjectSet &set)
{
    SPDocument *doc = set.document();
    if (!doc) {
        return {};
    }

    // Innermost first: extracting an outer frame would take a nested frame
    // out of the tree with it.
    std::vector<std::pair<int, SPGroup *>> targets;
    for (auto *item : set.items()) {
        auto *frame = frame_of(item);
        if (!frame) {
            continue;
        }
        if (std::any_of(targets.begin(), targets.end(), [frame](auto const &entry) {
                return entry.second == frame;
            })) {
            continue;
        }
        int depth = 0;
        for (auto *object = frame->parent; object; object = object->parent) {
            ++depth;
        }
        targets.emplace_back(depth, frame);
    }
    std::sort(targets.begin(), targets.end(), [](auto const &a, auto const &b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return sp_repr_compare_position_bool(a.second->getRepr(), b.second->getRepr());
    });

    // Collect reprs, not SPItems: extracting an outer frame re-parents (and so
    // rebuilds) the items an inner extraction just produced.
    std::vector<Inkscape::XML::Node *> reprs;
    for (auto const &[depth, frame] : targets) {
        for (auto *repr : extract_frame(frame)) {
            if (std::find(reprs.begin(), reprs.end(), repr) == reprs.end()) {
                reprs.push_back(repr);
            }
        }
    }

    std::vector<SPItem *> extracted;
    for (auto *repr : reprs) {
        if (auto *item = cast<SPItem>(doc->getObjectByRepr(repr))) {
            extracted.push_back(item);
        }
    }
    if (!extracted.empty()) {
        set.setList(extracted);
    }
    return extracted;
}

bool normalize(SPGroup *frame)
{
    if (!frame) {
        return false;
    }
    auto *doc = frame->document;
    auto *container = container_of(frame);
    auto *contents = contents_of(frame);
    if (!doc || !container || !contents) {
        g_warning("%s: frame is missing its container or contents", attr);
        return false;
    }

    bool changed = false;

    // (a) The contents group must not carry a transform: the live clip is in
    //     the frame's space, so a contents transform would move contents and
    //     clip away from the container.
    if (!contents->transform.isIdentity(1e-9)) {
        std::vector<std::pair<Inkscape::XML::Node *, Geom::Affine>> children;
        for (auto &child : contents->children) {
            if (auto *item = cast<SPItem>(&child)) {
                children.emplace_back(item->getRepr(), item->i2doc_affine());
            }
        }
        contents->doWriteTransform(Geom::Affine(), nullptr, false);
        auto const contents_i2doc = contents->i2doc_affine();
        for (auto const &[repr, old_i2doc] : children) {
            if (auto *item = cast<SPItem>(doc->getObjectByRepr(repr))) {
                item->doWriteTransform(old_i2doc * contents_i2doc.inverse(), nullptr, false);
            }
        }
        changed = true;
    }

    // (b) The contents must be clipped by a private clipPath that <use>s this
    //     frame's own container.
    std::string container_id = container->getId() ? container->getId() : "";
    if (container_id.empty()) {
        container_id = doc->generate_unique_id("powerclip-container");
        container->setAttribute("id", container_id.c_str());
        changed = true;
    }
    SPClipPath *clip = contents->getClipObject();
    if (!clip_matches(clip, container_id)) {
        std::string const clip_id = create_live_clip(doc, container_id);
        std::string const value = "url(#" + clip_id + ")";
        contents->setAttribute("clip-path", value.c_str());
        if (clip && !clip->isReferenced()) {
            clip->deleteObject(false);
        }
        changed = true;
    }

    return changed;
}

SPGroup *edit_target(SPObject *selected)
{
    auto *frame = frame_of(selected);
    return frame ? contents_of(frame) : nullptr;
}

SPGroup *finish_target(SPObject *current_layer)
{
    // Innermost first: inside nested PowerClips, finish the frame being edited.
    for (auto *object = current_layer; object; object = object->parent) {
        if (has_role(object, role_contents) && is_frame(object->parent)
            && contents_of(cast<SPGroup>(object->parent)) == object) {
            return cast<SPGroup>(object->parent);
        }
    }
    return nullptr;
}

} // namespace Inkscape::PowerClip

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
