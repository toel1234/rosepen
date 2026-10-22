// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PowerClip: a frame object that owns and clips its contents.
 *
 * A PowerClip is a plain SVG group marked with `inkscape:powerclip="frame"`.
 * Its first item child is the container (the frame shape, marked
 * `inkscape:powerclip="container"`); its second child is the contents group
 * (marked `inkscape:powerclip="contents"`), which carries a clip-path whose
 * clipPath holds a single <use> of the container. The clip is therefore live:
 * reshaping or moving the container reshapes or moves the clip.
 *
 * This is the Gtk-free document model: no ui/ or desktop headers, so it can be
 * driven from tests and headless actions.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef INK_OBJECT_POWERCLIP_H
#define INK_OBJECT_POWERCLIP_H

#include <vector>

class SPDocument;
class SPGroup;
class SPItem;
class SPObject;

namespace Inkscape {
class ObjectSet;
} // namespace Inkscape

namespace Inkscape::PowerClip {

/** Attribute that marks the parts of a PowerClip; values are the roles below. */
inline constexpr char const *attr = "inkscape:powerclip";
inline constexpr char const *role_frame = "frame";
inline constexpr char const *role_container = "container";
inline constexpr char const *role_contents = "contents";

/** True for an SPGroup carrying the frame role. */
bool is_frame(SPObject const *object);

/** Nearest ancestor-or-self frame, or nullptr. */
SPGroup *frame_of(SPObject *object);

/** The frame's direct item child with the container role, or nullptr. */
SPItem *container_of(SPGroup *frame);

/** The frame's direct group child with the contents role, or nullptr. */
SPGroup *contents_of(SPGroup *frame);

/** The contents group's direct item children, in document order. */
std::vector<SPItem *> content_items(SPGroup *frame);

/** All frames in document order, at every depth (master layers included). */
std::vector<SPGroup *> frames(SPDocument *doc);

/** Why place_inside() did nothing. */
enum class Refusal {
    None,
    TooFewItems,
    NoContainer,
    ContainerIsLayer,
    ContentIsLayer,
    ContainerInsideContent,
    ContentContainsContainer,
    NoContents,
    NotInFrame,
};

/** Stable, untranslated text for logs and the CLI; the action shows a translated flash. */
char const *refusal_message(Refusal refusal);

struct PlaceResult {
    SPGroup *frame = nullptr;
    Refusal refusal = Refusal::None;
};

/**
 * Place every selected item except the last one inside the last selected item
 * (the frame). If the target is already a PowerClip, its container (or
 * something inside it) or its contents group, the items are added to that
 * frame. A target inside the contents becomes the container of a new, nested
 * frame. A selected container/contents group of another frame stands for that
 * whole frame. Contents keep their absolute position on the page.
 *
 * The model does not record undo: the caller calls DocumentUndo::done() once.
 */
PlaceResult place_inside(ObjectSet &set);

/**
 * Dissolve every frame that owns a selection item: the contents come out at
 * their current visual position, just above the frame, and the container
 * becomes a plain object again. Returns the extracted items and selects them.
 */
std::vector<SPItem *> extract(ObjectSet &set);

/**
 * Repair a frame's invariants: push a contents-group transform into the
 * children, and give the frame a private, live clip of its own container.
 * Returns true if anything was changed.
 */
bool normalize(SPGroup *frame);

/** The contents group to enter for "Edit contents", or nullptr. */
SPGroup *edit_target(SPObject *selected);

/** The innermost frame whose contents (or contents descendant) is the current layer, or nullptr. */
SPGroup *finish_target(SPObject *current_layer);

} // namespace Inkscape::PowerClip

#endif // INK_OBJECT_POWERCLIP_H

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
