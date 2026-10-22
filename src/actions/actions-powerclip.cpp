// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for PowerClip, application level and headless.
 *
 * The actions are thin over the Gtk-free model in object/powerclip.{h,cpp}:
 * they fetch the active selection or desktop, run one model call, and record
 * exactly one undo step per successful document change.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "actions-powerclip.h"

#include <iostream>
#include <string>
#include <vector>

#include <glibmm/i18n.h>
#include <giomm.h>

#include "actions-helper.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "layer-manager.h"
#include "message-stack.h"
#include "object/powerclip.h"
#include "object/sp-clippath.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-use.h"
#include "selection.h"
#include "util/cast.h"

using Inkscape::DocumentUndo;
using Inkscape::PowerClip::Refusal;

namespace {

SPDocument *active_document(InkscapeApplication *app, char const *action)
{
    auto *doc = app->get_active_document();
    if (!doc) {
        g_warning("%s: no active document", action);
    }
    return doc;
}

/**
 * The refusal as a user-visible (translatable) message.
 * The texts mirror Inkscape::PowerClip::refusal_message(); keep them in sync.
 */
Glib::ustring refusal_flash(Refusal refusal)
{
    switch (refusal) {
        case Refusal::None:
            return _("PowerClip: ok");
        case Refusal::TooFewItems:
            return _("PowerClip: select at least two objects, the frame last");
        case Refusal::NoContainer:
            return _("PowerClip: the frame has no container");
        case Refusal::ContainerIsLayer:
            return _("PowerClip: a layer cannot be the frame");
        case Refusal::ContentIsLayer:
            return _("PowerClip: a layer cannot be placed inside a PowerClip");
        case Refusal::ContainerInsideContent:
            return _("PowerClip: the frame is inside the content");
        case Refusal::ContentContainsContainer:
            return _("PowerClip: the content contains the frame");
        case Refusal::NoContents:
            return _("PowerClip: nothing to place inside the frame");
        case Refusal::NotInFrame:
            return _("PowerClip: not inside a PowerClip");
    }
    return _("PowerClip: unknown refusal");
}

/**
 * Show a refusal to the user: a status-bar flash when there is a desktop,
 * the stable untranslated text on stderr otherwise (for scripts and tests).
 * Refusals change nothing, so the caller must not record an undo step.
 */
void report_refusal(InkscapeApplication *app, Refusal refusal)
{
    if (auto *desktop = app->get_active_desktop()) {
        desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE, refusal_flash(refusal));
    } else {
        // Headless: the stable, untranslated refusal text for scripts/tests.
        show_output(Glib::ustring(Inkscape::PowerClip::refusal_message(refusal)));
    }
}

std::string id_or_dash(SPObject const *object)
{
    auto const *id = object ? object->getId() : nullptr;
    return id ? id : "-";
}

/** The id in the clip's <use> href (the live clip source), or "-". */
std::string use_id(SPClipPath *clip)
{
    if (!clip) {
        return "-";
    }
    for (auto &child : clip->children) {
        if (auto *use = cast<SPUse>(&child)) {
            auto const *href = use->getAttribute("xlink:href");
            return (href && href[0] == '#') ? std::string(href + 1) : std::string("-");
        }
    }
    return "-";
}

void object_powerclip_place(InkscapeApplication *app)
{
    auto *selection = app->get_active_selection();
    if (!selection) {
        g_warning("object-powerclip-place: no active selection");
        return;
    }

    auto result = Inkscape::PowerClip::place_inside(*selection);
    if (result.refusal != Refusal::None) {
        report_refusal(app, result.refusal);
        return;
    }

    DocumentUndo::done(selection->document(), RC_("Undo", "Place inside PowerClip"), "");
}

void object_powerclip_extract(InkscapeApplication *app)
{
    auto *selection = app->get_active_selection();
    if (!selection) {
        g_warning("object-powerclip-extract: no active selection");
        return;
    }

    auto extracted = Inkscape::PowerClip::extract(*selection);
    if (extracted.empty()) {
        report_refusal(app, Refusal::NotInFrame);
        return;
    }

    DocumentUndo::done(selection->document(), RC_("Undo", "Extract PowerClip contents"), "");
}

void object_powerclip_edit(InkscapeApplication *app)
{
    auto *desktop = app->get_active_desktop();
    if (!desktop) {
        show_output("object-powerclip-edit: needs a window");
        return;
    }

    auto *selection = desktop->getSelection();
    auto *contents = Inkscape::PowerClip::edit_target(selection->single());
    if (!contents) {
        report_refusal(app, Refusal::NotInFrame);
        return;
    }

    // Repair stale invariants (e.g. after duplicating the frame) as one
    // undoable step before entering the contents.
    auto *frame = Inkscape::PowerClip::frame_of(contents);
    if (Inkscape::PowerClip::normalize(frame)) {
        DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Repair PowerClip"), "");
    }

    desktop->layerManager().setCurrentLayer(contents);
    selection->setList(Inkscape::PowerClip::content_items(frame));
}

void object_powerclip_finish(InkscapeApplication *app)
{
    auto *desktop = app->get_active_desktop();
    if (!desktop) {
        show_output("object-powerclip-finish: needs a window");
        return;
    }

    auto *frame = Inkscape::PowerClip::finish_target(desktop->layerManager().currentLayer());
    if (!frame) {
        report_refusal(app, Refusal::NotInFrame);
        return;
    }

    if (Inkscape::PowerClip::normalize(frame)) {
        DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Repair PowerClip"), "");
    }

    desktop->layerManager().setCurrentLayer(frame->parent);
    desktop->getSelection()->set(frame);
}

void object_powerclip_list(InkscapeApplication *app)
{
    auto *doc = active_document(app, "object-powerclip-list");
    if (!doc) {
        return;
    }

    std::string out;
    auto const frame_list = Inkscape::PowerClip::frames(doc);
    for (auto *frame : frame_list) {
        auto *container = Inkscape::PowerClip::container_of(frame);
        auto *contents = Inkscape::PowerClip::contents_of(frame);
        auto *clip = contents ? contents->getClipObject() : nullptr;

        out += "powerclip ";
        out += id_or_dash(frame);
        out += " container=";
        out += id_or_dash(container);
        out += " contents=";
        if (contents) {
            std::string ids;
            for (auto *item : Inkscape::PowerClip::content_items(frame)) {
                ids += (ids.empty() ? "" : ",");
                ids += id_or_dash(item);
            }
            out += ids.empty() ? "-" : ids;
        } else {
            out += "-";
        }
        out += " clip=";
        out += id_or_dash(clip);
        out += " use=";
        out += use_id(clip);
        out += '\n';
    }
    out += "powerclip-count " + std::to_string(frame_list.size()) + "\n";

    std::cout << out;
    std::cout.flush();
}

const Glib::ustring SECTION = NC_("Action Section", "Object");

std::vector<std::vector<Glib::ustring>> raw_data_powerclip = {
    // clang-format off
    {"app.object-powerclip-place",   N_("Place Inside Frame"), SECTION, N_("Place the selected objects inside the last selected object, the PowerClip frame")},
    {"app.object-powerclip-extract", N_("Extract Contents"),   SECTION, N_("Take the contents out of the PowerClip at their current position and dissolve the frame")},
    {"app.object-powerclip-edit",    N_("Edit Contents"),      SECTION, N_("Enter the PowerClip contents and edit them in place")},
    {"app.object-powerclip-finish",  N_("Finish Editing"),     SECTION, N_("Leave the PowerClip contents and select the frame")},
    {"app.object-powerclip-list",    N_("List PowerClips"),    SECTION, N_("Print one line per PowerClip in the document and the count")},
    // clang-format on
};

} // namespace

void add_actions_powerclip(InkscapeApplication *app)
{
    if (!app) {
        show_output("add_actions_powerclip: no app!");
        return;
    }

    auto *gapp = app->gio_app();

    // clang-format off
    gapp->add_action("object-powerclip-place",   sigc::bind(sigc::ptr_fun(&object_powerclip_place),   app));
    gapp->add_action("object-powerclip-extract", sigc::bind(sigc::ptr_fun(&object_powerclip_extract), app));
    gapp->add_action("object-powerclip-edit",    sigc::bind(sigc::ptr_fun(&object_powerclip_edit),    app));
    gapp->add_action("object-powerclip-finish",  sigc::bind(sigc::ptr_fun(&object_powerclip_finish),  app));
    gapp->add_action("object-powerclip-list",    sigc::bind(sigc::ptr_fun(&object_powerclip_list),    app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_powerclip);
}

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
