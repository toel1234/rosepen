// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Dimension tool controls bar.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "dimension-toolbar.h"

#include <cstdlib>
#include <string>

#include <gtkmm/dropdown.h>
#include <gtkmm/togglebutton.h>

#include "actions/actions-dimension.h"
#include "desktop.h"
#include "document-undo.h"
#include "object/sp-item-group.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "preferences.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-tracker.h"
#include "util/cast.h"
#include "xml/repr.h"

using Inkscape::UI::Widget::SpinButton;
using Inkscape::UI::Widget::UnitTracker;
using Inkscape::Util::Unit;

namespace Inkscape::UI::Toolbar {

namespace {

char const *mode_pref = "/tools/dimension/mode";
char const *precision_pref = "/tools/dimension/precision";
char const *offset_pref = "/tools/dimension/offset";
char const *unit_pref = "/tools/dimension/unit";
char const *text_pref = "/tools/dimension/text";
char const *units_pref = "/tools/dimension/units";

char const *mode_keys[] = {"auto", "parallel", "angular", "segment"};
char const *text_keys[] = {"above", "inline", "below"};

/// The nearest ancestor that is a Rosepen dimension group, or null.
SPGroup *dimension_group(SPItem *item)
{
    for (SPObject *object = item; object; object = object->parent) {
        auto const group = cast<SPGroup>(object);
        if (!group) {
            continue;
        }
        auto const repr = group->getRepr();
        auto const klass = repr ? repr->attribute("class") : nullptr;
        if (klass && std::string(klass).find("rosepen-dimension") != std::string::npos) {
            return group;
        }
    }
    return nullptr;
}

std::string number(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(buffer, sizeof buffer, value);
    return buffer;
}

} // namespace

DimensionToolbar::DimensionToolbar()
    : DimensionToolbar{create_builder("toolbar-dimension.ui")}
{
}

DimensionToolbar::DimensionToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "dimension-toolbar")}
    , _tracker{std::make_unique<UnitTracker>(Util::UNIT_TYPE_LINEAR)}
    , _mode_auto{get_widget<Gtk::ToggleButton>(builder, "mode_auto")}
    , _mode_parallel{get_widget<Gtk::ToggleButton>(builder, "mode_parallel")}
    , _mode_angular{get_widget<Gtk::ToggleButton>(builder, "mode_angular")}
    , _mode_segment{get_widget<Gtk::ToggleButton>(builder, "mode_segment")}
    , _precision_item{get_derived_widget<SpinButton>(builder, "_precision_item")}
    , _offset_item{get_derived_widget<SpinButton>(builder, "_offset_item")}
    , _text_combo{get_widget<Gtk::DropDown>(builder, "text_combo")}
    , _units_btn{get_widget<Gtk::ToggleButton>(builder, "show_units_btn")}
{
    auto const prefs = Preferences::get();

    auto unit_menu = _tracker->create_unit_dropdown();
    unit_menu->signal_changed().connect(sigc::mem_fun(*this, &DimensionToolbar::unit_changed));
    get_widget<Gtk::Box>(builder, "unit_menu_box").append(*unit_menu);

    _precision_item.get_adjustment()->set_value(prefs->getInt(precision_pref, 2));
    _precision_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &DimensionToolbar::precision_changed));
    _precision_item.setDefocusTarget(this);

    _offset_item.get_adjustment()->set_value(prefs->getDouble(offset_pref, 5.0));
    _offset_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &DimensionToolbar::offset_changed));
    _offset_item.setDefocusTarget(this);

    _text_combo.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &DimensionToolbar::text_changed));

    _units_btn.set_active(prefs->getBool(units_pref, true));
    _units_btn.signal_toggled().connect(sigc::mem_fun(*this, &DimensionToolbar::units_changed));

    for (auto button : {&_mode_auto, &_mode_parallel, &_mode_angular, &_mode_segment}) {
        button->signal_toggled().connect(sigc::mem_fun(*this, &DimensionToolbar::mode_changed));
    }

    sync_mode_toggles();

    auto const text = prefs->getString(text_pref, "above");
    for (int i = 0; i < 3; ++i) {
        if (text == text_keys[i]) {
            _text_combo.set_selected(i);
            break;
        }
    }
}

DimensionToolbar::~DimensionToolbar() = default;

void DimensionToolbar::setDesktop(SPDesktop *desktop)
{
    Toolbar::setDesktop(desktop);

    if (!_desktop || _unit_set) {
        return;
    }
    auto const default_unit = desktop->getNamedView()->getDisplayUnit();
    auto const unit = Preferences::get()->getString(unit_pref, default_unit->abbr);
    // Initialising the dropdown must not restyle the selected dimensions.
    _updating = true;
    _tracker->setActiveUnitByAbbr(unit.c_str());
    _updating = false;
    _unit_set = true;
}

void DimensionToolbar::sync_mode_toggles()
{
    auto const mode = Preferences::get()->getString(mode_pref, "auto");

    _updating = true;
    for (auto [button, key] : {std::pair{&_mode_auto, mode_keys[0]}, std::pair{&_mode_parallel, mode_keys[1]},
                               std::pair{&_mode_angular, mode_keys[2]}, std::pair{&_mode_segment, mode_keys[3]}}) {
        if (mode == key) {
            button->set_active(true);
        }
    }
    _updating = false;
}

void DimensionToolbar::apply_to_selection(char const *key, std::string const &value)
{
    if (!_desktop) {
        return;
    }
    auto const selection = _desktop->getSelection();
    if (!selection || selection->isEmpty()) {
        return;
    }

    bool changed = false;
    for (auto item : selection->items()) {
        if (auto const group = dimension_group(item)) {
            changed = Inkscape::DimensionOps::set(group, key, value) || changed;
        }
    }
    if (changed) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Change dimension"), "");
    }
}

void DimensionToolbar::mode_changed()
{
    if (_updating) {
        return;
    }

    char const *mode = nullptr;
    if (_mode_auto.get_active()) {
        mode = mode_keys[0];
    } else if (_mode_parallel.get_active()) {
        mode = mode_keys[1];
    } else if (_mode_angular.get_active()) {
        mode = mode_keys[2];
    } else if (_mode_segment.get_active()) {
        mode = mode_keys[3];
    }
    if (!mode) {
        return;
    }
    Preferences::get()->setString(mode_pref, mode);

    // Only the stored linear kinds can be changed on an existing dimension;
    // auto resolves at creation time and angular/segment cannot be converted.
    if (std::string(mode) == "parallel") {
        apply_to_selection("kind", "aligned");
    }
}

void DimensionToolbar::precision_changed()
{
    int const value = static_cast<int>(_precision_item.get_adjustment()->get_value());
    Preferences::get()->setInt(precision_pref, value);
    apply_to_selection("precision", std::to_string(value));
}

void DimensionToolbar::offset_changed()
{
    double const value = _offset_item.get_adjustment()->get_value();
    Preferences::get()->setDouble(offset_pref, value);
    apply_to_selection("offset", number(value));
}

void DimensionToolbar::text_changed()
{
    auto const selected = _text_combo.get_selected();
    if (selected >= 3) {
        return;
    }
    auto const key = text_keys[selected];
    Preferences::get()->setString(text_pref, key);
    apply_to_selection("text", key);
}

void DimensionToolbar::units_changed()
{
    bool const active = _units_btn.get_active();
    Preferences::get()->setBool(units_pref, active);
    apply_to_selection("units", active ? "true" : "false");
}

void DimensionToolbar::unit_changed()
{
    if (_updating || !_tracker->getActiveUnit()) {
        return;
    }
    auto const unit = _tracker->getActiveUnit()->abbr;
    Preferences::get()->setString(unit_pref, unit);
    apply_to_selection("unit", unit.raw());
}

} // namespace Inkscape::UI::Toolbar

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
