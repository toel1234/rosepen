// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for master layers, application level and headless.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "actions-master-layer.h"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>
#include <giomm.h>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "layer-manager.h"
#include "object/master-layer.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-page.h"
#include "ui/icon-names.h"

namespace {

using Inkscape::DocumentUndo;

std::string trim(std::string const &input)
{
    auto const first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    auto const last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

bool equal_ci(std::string const &value, char const *expected)
{
    return g_ascii_strcasecmp(value.c_str(), expected) == 0;
}

std::string format_user_unit(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

std::string string_param(Glib::VariantBase const &value)
{
    return Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();
}

SPDocument *active_document(InkscapeApplication *app, char const *action)
{
    auto *doc = app->get_active_document();
    if (!doc) {
        g_warning("%s: no active document", action);
    }
    return doc;
}

SPGroup *current_layer(InkscapeApplication *app, char const *action)
{
    auto *desktop = app->get_active_desktop();
    if (!desktop) {
        g_warning("%s: no active desktop", action);
        return nullptr;
    }
    return desktop->layerManager().currentLayer();
}

void apply_master_state(SPDocument *doc, SPGroup *layer, bool on)
{
    if (!MasterLayer::set_master(layer, on)) {
        return;
    }
    DocumentUndo::done(doc, RC_("Undo", "Set master layer"), INKSCAPE_ICON("layer-new"));
}

void layer_master_set(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto *doc = active_document(app, "layer-master-set");
    if (!doc) {
        return;
    }

    std::string const spec = string_param(value);
    auto const comma = spec.rfind(',');
    if (comma == std::string::npos) {
        g_warning("layer-master-set: expected '<layer-id>,<on|off|toggle>'");
        return;
    }
    std::string const id = trim(spec.substr(0, comma));
    std::string const mode = trim(spec.substr(comma + 1));

    bool const toggle = equal_ci(mode, "toggle");
    bool on = false;
    if (!toggle) {
        if (equal_ci(mode, "on")) {
            on = true;
        } else if (!equal_ci(mode, "off")) {
            g_warning("layer-master-set: unknown mode '%s' (expected on, off or toggle)", mode.c_str());
            return;
        }
    }

    SPGroup *layer = nullptr;
    if (id.empty()) {
        layer = current_layer(app, "layer-master-set");
    } else if (auto *object = doc->getObjectById(id)) {
        layer = cast<SPGroup>(object);
        if (!layer) {
            g_warning("layer-master-set: object '%s' is not a layer", id.c_str());
            return;
        }
    } else {
        g_warning("layer-master-set: no object with id '%s'", id.c_str());
        return;
    }
    if (!layer) {
        return;
    }

    if (toggle) {
        on = !MasterLayer::is_master(layer);
    }
    apply_master_state(doc, layer, on);
}

void layer_master_toggle(InkscapeApplication *app)
{
    auto *layer = current_layer(app, "layer-master-toggle");
    if (!layer || !layer->document) {
        return;
    }
    apply_master_state(layer->document, layer, !MasterLayer::is_master(layer));
}

void layer_master_list(InkscapeApplication *app)
{
    auto *doc = active_document(app, "layer-master-list");
    if (!doc) {
        return;
    }
    std::cout << Inkscape::master_layer_list(doc);
    std::cout.flush();
}

const Glib::ustring SECTION = NC_("Action Section", "Layer");

std::vector<std::vector<Glib::ustring>> raw_data_master_layer = {
    // clang-format off
    {"app.layer-master-set",    N_("Set Master Layer"),    SECTION, N_("Flag a top-level layer as a master layer shown on every page")},
    {"app.layer-master-toggle", N_("Toggle Master Layer"), SECTION, N_("Toggle the current layer as a master layer")},
    {"app.layer-master-list",   N_("List Master Layers"),  SECTION, N_("Print one line per master layer and one per page offset")},
    // clang-format on
};

} // namespace

namespace Inkscape {

std::string master_layer_list(SPDocument *doc)
{
    if (!doc) {
        return {};
    }

    std::string output;
    for (auto *layer : MasterLayer::master_layers(doc)) {
        auto const *id = layer->getId();
        auto const *label = layer->label();
        output += "layer\t";
        output += id ? id : "-";
        output += '\t';
        output += (label && label[0]) ? label : "-";
        output += '\t';
        output += std::to_string(layer->pos_in_parent() + 1);
        output += '\t';
        output += layer->isHidden() ? "hidden" : "visible";
        output += '\n';
    }

    for (auto const &[page, offset] : MasterLayer::page_offsets(doc)) {
        output += "offset\t";
        output += std::to_string(page->getPagePosition());
        output += '\t';
        output += format_user_unit(offset.x());
        output += '\t';
        output += format_user_unit(offset.y());
        output += '\n';
    }
    return output;
}

} // namespace Inkscape

void add_actions_master_layer(InkscapeApplication *app)
{
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    // clang-format off
    gapp->add_action_with_parameter("layer-master-set", String, sigc::bind(sigc::ptr_fun(&layer_master_set), app));
    gapp->add_action(                "layer-master-toggle",        sigc::bind(sigc::ptr_fun(&layer_master_toggle), app));
    gapp->add_action(                "layer-master-list",          sigc::bind(sigc::ptr_fun(&layer_master_list), app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_master_layer);
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
