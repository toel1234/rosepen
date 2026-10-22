// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Artistic Media tool controls bar (Corel property-bar semantics).
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "artistic-media-toolbar.h"

#include <string>

#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>

#include "actions/actions-artistic-media.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "live_effects/artistic-media-library.h"
#include "message-stack.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "preferences.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/icon-names.h"
#include "ui/widget/spinbutton.h"
#include "util/cast.h"

using Inkscape::UI::Widget::SpinButton;

namespace AM = Inkscape::LivePathEffect::ArtisticMedia;

namespace Inkscape::UI::Toolbar {

namespace {

char const *mode_pref = "/tools/artistic/mode";
char const *preset_pref = "/tools/artistic/preset";
char const *brush_pref = "/tools/artistic/brush";
char const *width_pref = "/tools/artistic/width";
char const *angle_pref = "/tools/artistic/angle";
char const *smoothing_pref = "/tools/artistic/smoothing";
char const *scale_pref = "/tools/artistic/scale_with_object";

AM::Mode mode_from_prefs()
{
    auto const key = Preferences::get()->getString(mode_pref, "preset");
    if (auto const mode = AM::parse_mode(key.raw())) {
        return *mode;
    }
    return AM::Mode::Preset;
}

/// The stored library shape of the mode, or the mode default when unknown.
std::string stored_shape(AM::Mode mode)
{
    if (mode == AM::Mode::Calligraphic) {
        return {};
    }
    auto const prefs = Preferences::get();
    char const *pref = mode == AM::Mode::Brush ? brush_pref : preset_pref;
    auto const name = prefs->getString(pref, mode == AM::Mode::Brush ? "dry-brush" : "lens");
    if (AM::find(mode, name.raw())) {
        return name.raw();
    }
    char const *fallback = AM::default_shape(mode);
    return fallback ? fallback : std::string();
}

} // namespace

ArtisticMediaToolbar::ArtisticMediaToolbar()
    : ArtisticMediaToolbar{create_builder("toolbar-artistic-media.ui")}
{
}

ArtisticMediaToolbar::ArtisticMediaToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "artistic-media-toolbar")}
    , _mode_preset{get_widget<Gtk::ToggleButton>(builder, "mode_preset")}
    , _mode_brush{get_widget<Gtk::ToggleButton>(builder, "mode_brush")}
    , _mode_calligraphic{get_widget<Gtk::ToggleButton>(builder, "mode_calligraphic")}
    , _shape_box{get_widget<Gtk::Box>(builder, "shape_box")}
    , _angle_box{get_widget<Gtk::Box>(builder, "angle_box")}
    , _shape_combo{get_widget<Gtk::DropDown>(builder, "shape_combo")}
    , _width_item{get_derived_widget<SpinButton>(builder, "width_item")}
    , _angle_item{get_derived_widget<SpinButton>(builder, "angle_item")}
    , _smoothing_item{get_derived_widget<SpinButton>(builder, "smoothing_item")}
    , _scale_toggle{get_widget<Gtk::ToggleButton>(builder, "scale_toggle")}
{
    for (auto button : {&_mode_preset, &_mode_brush, &_mode_calligraphic}) {
        button->signal_toggled().connect(sigc::mem_fun(*this, &ArtisticMediaToolbar::mode_changed));
    }

    _shape_combo.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &ArtisticMediaToolbar::shape_changed));

    _width_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &ArtisticMediaToolbar::width_changed));
    _width_item.setDefocusTarget(this);

    _angle_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &ArtisticMediaToolbar::angle_changed));
    _angle_item.setDefocusTarget(this);

    _smoothing_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &ArtisticMediaToolbar::smoothing_changed));
    _smoothing_item.setDefocusTarget(this);

    _scale_toggle.signal_toggled().connect(sigc::mem_fun(*this, &ArtisticMediaToolbar::scale_changed));

    // First show must not touch the document: everything programmatic below
    // runs guarded.
    sync_from_prefs();
}

ArtisticMediaToolbar::~ArtisticMediaToolbar() = default;

void ArtisticMediaToolbar::setDesktop(SPDesktop *desktop)
{
    Toolbar::setDesktop(desktop);

    if (!_desktop) {
        return;
    }
    // Reflect a selected stroke in the (guarded) widgets, like a property bar.
    sync_from_selection();
}

void ArtisticMediaToolbar::rebuild_shape_combo(Inkscape::LivePathEffect::ArtisticMedia::Mode mode,
                                               std::string const &select_name)
{
    _shown_mode = mode;

    // Setting a model and a selection fires the changed signals; keep them
    // guarded, restoring any outer guard.
    bool const was_updating = _updating;
    _updating = true;

    auto model = Gtk::StringList::create({});
    _shape_names.clear();
    for (auto const &entry : AM::library()) {
        if (entry.mode != mode) {
            continue;
        }
        model->append(_(entry.label));
        _shape_names.push_back(entry.name);
    }
    _shape_combo.set_model(model);

    // Look the name up in the NEW mode's rows.
    unsigned select = GTK_INVALID_LIST_POSITION;
    for (std::size_t i = 0; i < _shape_names.size(); ++i) {
        if (_shape_names[i] == select_name) {
            select = static_cast<unsigned>(i);
            break;
        }
    }
    _shape_combo.set_selected(select);

    _updating = was_updating;
}

void ArtisticMediaToolbar::sync_sensitivity()
{
    bool const calligraphic = _shown_mode == AM::Mode::Calligraphic;
    _shape_box.set_sensitive(!calligraphic);
    _angle_box.set_sensitive(calligraphic);
}

void ArtisticMediaToolbar::sync_from_prefs()
{
    auto const prefs = Preferences::get();
    AM::Mode const mode = mode_from_prefs();
    std::string const shape = stored_shape(mode);

    _updating = true;

    _mode_preset.set_active(mode == AM::Mode::Preset);
    _mode_brush.set_active(mode == AM::Mode::Brush);
    _mode_calligraphic.set_active(mode == AM::Mode::Calligraphic);

    rebuild_shape_combo(mode, shape);

    _width_item.get_adjustment()->set_value(prefs->getDouble(width_pref, 10.0));
    _angle_item.get_adjustment()->set_value(prefs->getDouble(angle_pref, 30.0));
    _smoothing_item.get_adjustment()->set_value(prefs->getDouble(smoothing_pref, 50.0));
    _scale_toggle.set_active(prefs->getBool(scale_pref, true));

    _updating = false;

    sync_sensitivity();
}

void ArtisticMediaToolbar::sync_from_selection()
{
    if (!_desktop) {
        return;
    }
    auto const selection = _desktop->getSelection();
    if (!selection) {
        return;
    }

    // The first selected artistic media stroke wins; a mixed selection shows
    // its values. Purely visual: no preference is written.
    SPPath *first = nullptr;
    for (auto item : selection->items()) {
        if (auto const path = cast<SPPath>(item); path && Inkscape::ArtisticOps::is_artistic(path)) {
            first = path;
            break;
        }
    }
    if (!first || !first->getId()) {
        return;
    }

    for (auto const &row : Inkscape::ArtisticOps::list(_desktop->getDocument())) {
        if (row.id != first->getId()) {
            continue;
        }

        AM::Mode const mode = AM::parse_mode(row.mode).value_or(AM::Mode::Preset);

        _updating = true;
        _mode_preset.set_active(mode == AM::Mode::Preset);
        _mode_brush.set_active(mode == AM::Mode::Brush);
        _mode_calligraphic.set_active(mode == AM::Mode::Calligraphic);

        rebuild_shape_combo(mode, row.shape);

        _width_item.get_adjustment()->set_value(row.width);
        _angle_item.get_adjustment()->set_value(row.angle);
        _updating = false;

        sync_sensitivity();
        break;
    }
}

void ArtisticMediaToolbar::apply_to_selection(Inkscape::ArtisticOps::Options const &options)
{
    if (!_desktop) {
        return;
    }
    auto const selection = _desktop->getSelection();
    if (!selection || selection->isEmpty()) {
        return;
    }

    std::string errors;
    int const applied = Inkscape::ArtisticOps::apply(*selection, options, errors);
    if (!errors.empty()) {
        // "artistic-apply: <reason>" lines; the action prefix is CLI wording.
        std::string const prefix = "artistic-apply: ";
        std::string message;
        for (std::size_t begin = 0; begin < errors.size();) {
            auto const end = errors.find('\n', begin);
            auto line = errors.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (line.starts_with(prefix)) {
                line.erase(0, prefix.size());
            }
            if (!line.empty()) {
                message = std::move(line);
                break;
            }
            begin = end == std::string::npos ? errors.size() : end + 1;
        }
        if (!message.empty()) {
            _desktop->messageStack()->flash(ERROR_MESSAGE, Glib::ustring{message});
        }
    }
    if (applied > 0) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Change artistic media"),
                           INKSCAPE_ICON("taper-stroke"));
        _desktop->getDocument()->ensureUpToDate();
    }
}

void ArtisticMediaToolbar::mode_changed()
{
    if (_updating) {
        return;
    }

    char const *key = nullptr;
    AM::Mode mode = AM::Mode::Preset;
    if (_mode_preset.get_active()) {
        key = "preset";
        mode = AM::Mode::Preset;
    } else if (_mode_brush.get_active()) {
        key = "brush";
        mode = AM::Mode::Brush;
    } else if (_mode_calligraphic.get_active()) {
        key = "calligraphic";
        mode = AM::Mode::Calligraphic;
    }
    if (!key) {
        return;
    }
    Preferences::get()->setString(mode_pref, key);

    rebuild_shape_combo(mode, stored_shape(mode));
    sync_sensitivity();

    Inkscape::ArtisticOps::Options options;
    options.mode = mode;
    apply_to_selection(options);
}

void ArtisticMediaToolbar::shape_changed()
{
    if (_updating) {
        return;
    }
    auto const selected = _shape_combo.get_selected();
    if (selected == GTK_INVALID_LIST_POSITION || static_cast<std::size_t>(selected) >= _shape_names.size()) {
        return;
    }
    auto const &name = _shape_names[selected];

    // The rows belong to the mode on show (a selected stroke's may differ
    // from the tool's).
    AM::Mode const mode = _shown_mode;
    if (mode == AM::Mode::Calligraphic) {
        return;
    }
    char const *pref = mode == AM::Mode::Brush ? brush_pref : preset_pref;
    Preferences::get()->setString(pref, name);

    Inkscape::ArtisticOps::Options options;
    options.mode = mode; // plain paths must not fall back to the preset table
    options.shape = name;
    apply_to_selection(options);
}

void ArtisticMediaToolbar::width_changed()
{
    if (_updating) {
        return;
    }
    double const value = _width_item.get_adjustment()->get_value();
    Preferences::get()->setDouble(width_pref, value);

    Inkscape::ArtisticOps::Options options;
    options.width = value;
    apply_to_selection(options);
}

void ArtisticMediaToolbar::angle_changed()
{
    if (_updating) {
        return;
    }
    double const value = _angle_item.get_adjustment()->get_value();
    Preferences::get()->setDouble(angle_pref, value);

    Inkscape::ArtisticOps::Options options;
    options.angle = value;
    apply_to_selection(options);
}

void ArtisticMediaToolbar::smoothing_changed()
{
    if (_updating) {
        return;
    }
    // Creation-only: smoothing never changes existing strokes.
    Preferences::get()->setDouble(smoothing_pref, _smoothing_item.get_adjustment()->get_value());
}

void ArtisticMediaToolbar::scale_changed()
{
    if (_updating) {
        return;
    }
    bool const active = _scale_toggle.get_active();
    Preferences::get()->setBool(scale_pref, active);

    Inkscape::ArtisticOps::Options options;
    options.scale_with_object = active;
    apply_to_selection(options);
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
