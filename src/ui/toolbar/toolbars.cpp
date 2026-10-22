// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 *
 * A container for toolbars, displaying one toolbar at a time.
 *
 *//*
 * Authors:
 *  Tavmjong Bah
 *  Alex Valavanis
 *  Mike Kowalski
 *  Vaibhav Malik
 *  PBS
 *
 * Copyright (C) 2024 PBS
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "toolbars.h"

#include <gtkmm/separator.h>

#include "desktop.h"
#include "object/sp-namedview.h"
#include "preferences.h"
#include "selection.h"
#include "ui/toolbar/property-bar-context.h"
#include "ui/toolbar/toolbar-constants.h"
#include "ui/shortcuts.h"
#include "ui/util.h"

// For creating toolbars
#include "ui/toolbar/arc-toolbar.h"
#include "ui/toolbar/booleans-toolbar.h"
#include "ui/toolbar/box3d-toolbar.h"
#include "ui/toolbar/calligraphy-toolbar.h"
#include "ui/toolbar/connector-toolbar.h"
#include "ui/toolbar/dropper-toolbar.h"
#include "ui/toolbar/eraser-toolbar.h"
#include "ui/toolbar/gradient-toolbar.h"
#include "ui/toolbar/lpe-toolbar.h"
#include "ui/toolbar/marker-toolbar.h"
#include "ui/toolbar/measure-toolbar.h"
#include "ui/toolbar/dimension-toolbar.h"
#include "ui/toolbar/mesh-toolbar.h"
#include "ui/toolbar/node-toolbar.h"
#include "ui/toolbar/objectpicker-toolbar.h"
#include "ui/toolbar/page-toolbar.h"
#include "ui/toolbar/paintbucket-toolbar.h"
#include "ui/toolbar/pencil-toolbar.h"
#include "ui/toolbar/rect-toolbar.h"
#include "ui/toolbar/select-toolbar.h"
#include "ui/toolbar/spiral-toolbar.h"
#include "ui/toolbar/spray-toolbar.h"
#include "ui/toolbar/star-toolbar.h"
#include "ui/toolbar/text-toolbar.h"
#include "ui/toolbar/tweak-toolbar.h"
#include "ui/toolbar/zoom-toolbar.h"
#include "ui/tools/tool-base.h"

namespace Inkscape::UI::Toolbar {
namespace {

// Data for building and tracking toolbars.
struct ToolbarData
{
    Glib::ustring name;
    std::unique_ptr<Toolbar> (*create)();
};

template <typename T, auto... args>
auto create = [] () -> std::unique_ptr<Toolbar> { return std::make_unique<T>(args...); };

auto const toolbar_data = std::unordered_map<std::string, ToolbarData>{
    // clang-format off
    {"/tools/select",          {"Select",       create<SelectToolbar>}},
    {"/tools/nodes",           {"Node",         create<NodeToolbar>}},
    {"/tools/booleans",        {"Booleans",     create<BooleansToolbar>}},
    {"/tools/marker",          {"Marker",       create<MarkerToolbar>}},
    {"/tools/shapes/rect",     {"Rect",         create<RectToolbar>}},
    {"/tools/shapes/arc",      {"Arc",          create<ArcToolbar>}},
    {"/tools/shapes/star",     {"Star",         create<StarToolbar>}},
    {"/tools/shapes/3dbox",    {"3DBox",        create<Box3DToolbar>}},
    {"/tools/shapes/spiral",   {"Spiral",       create<SpiralToolbar>}},
    {"/tools/freehand/pencil", {"Pencil",       create<PencilToolbar, true>}},
    {"/tools/freehand/pen",    {"Pen",          create<PencilToolbar, false>}},
    {"/tools/calligraphic",    {"Calligraphic", create<CalligraphyToolbar>}},
    {"/tools/text",            {"Text",         create<TextToolbar>}},
    {"/tools/gradient",        {"Gradient",     create<GradientToolbar>}},
    {"/tools/mesh",            {"Mesh",         create<MeshToolbar>}},
    {"/tools/zoom",            {"Zoom",         create<ZoomToolbar>}},
    {"/tools/measure",         {"Measure",      create<MeasureToolbar>}},
    {"/tools/dimension",       {"Dimension",    create<DimensionToolbar>}},
    {"/tools/dropper",         {"Dropper",      create<DropperToolbar>}},
    {"/tools/tweak",           {"Tweak",        create<TweakToolbar>}},
    {"/tools/spray",           {"Spray",        create<SprayToolbar>}},
    {"/tools/connector",       {"Connector",    create<ConnectorToolbar>}},
    {"/tools/pages",           {"Pages",        create<PageToolbar>}},
    {"/tools/paintbucket",     {"Paintbucket",  create<PaintbucketToolbar>}},
    {"/tools/eraser",          {"Eraser",       create<EraserToolbar>}},
    {"/tools/lpetool",         {"LPETool",      create<LPEToolbar>}},
    {"/tools/picker",          {"ObjectPicker", create<ObjectPickerToolbar>}},
    // clang-format on
};

} // namespace

Toolbars::Toolbars()
    : Gtk::Box{Gtk::Orientation::HORIZONTAL}
{
    set_name("Toolbars");

    // Rosepen 04-02: separator between the current toolbar and the object
    // section; visible only while a section is shown. Margins instead of CSS
    // classes, so the classic bar is untouched when no section is visible.
    _section_separator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    _section_separator->set_margin_start(4);
    _section_separator->set_margin_end(4);
    _section_separator->set_visible(false);
    append(*_section_separator);

    _sections_pref_observer = Preferences::get()->createObserver(object_sections_pref_path,
                                                                 [this] { _updateSection(); });
}

Toolbars::~Toolbars()
{
    _selection_changed_conn = sigc::connection{};
    if (_current_section) {
        _current_section->setDesktop(nullptr);
    }
    if (_current_toolbar) {
        _current_toolbar->setDesktop(nullptr);
    }
}

void Toolbars::setTool(Tools::ToolBase *tool)
{
    // Acquire the toolbar to be shown, possibly null
    Toolbar *toolbar = nullptr;

    if (tool) {
        auto &toolbars_entry = _toolbars[tool->getPrefsPath()];

        if (!toolbars_entry) {
            // Lazily create the toolbar.
            auto const &data = toolbar_data.at(tool->getPrefsPath());
            toolbars_entry = data.create();
            toolbars_entry->set_name(data.name + "Toolbar");
            toolbars_entry->set_hexpand();
            Shortcuts::getInstance().update_gui_text_recursive(toolbars_entry.get());
            append(*toolbars_entry);
        }

        toolbar = toolbars_entry.get();
    }

    if (toolbar != _current_toolbar) {
        // Tool has changed.
        if (_current_toolbar) {
            _current_toolbar->set_visible(false);
            _current_toolbar->setDesktop(nullptr);
        }
        _current_toolbar = toolbar;
        if (_current_toolbar) {
            _current_toolbar->setDesktop(tool->getDesktop());
            _current_toolbar->set_visible(true);
        }
    } else if (_current_toolbar && tool->getDesktop() != _current_toolbar->getDesktop()) {
        // Tool has stayed the same but desktop has changed.
        _current_toolbar->setDesktop(tool->getDesktop());
    }

    // Rosepen 04-02: track the active tool and desktop for the object section.
    _tool_path = tool ? tool->getPrefsPath() : "";
    auto *desktop = tool ? tool->getDesktop() : nullptr;
    if (desktop != _desktop) {
        _selection_changed_conn = sigc::connection{}; // disconnects the old slot
        if (desktop) {
            _selection_changed_conn = desktop->getSelection()->connectChanged(
                [this](Selection *) { _updateSection(); });
        }
        _desktop = desktop;
    }
    _updateSection();
}

void Toolbars::setActiveUnit(Util::Unit const *unit)
{
    if (_current_toolbar) {
        _current_toolbar->setActiveUnit(unit);
    }
    if (_current_section) {
        _current_section->setActiveUnit(unit);
    }
}

Toolbar *Toolbars::get_current_toolbar()
{
    return _current_toolbar;
}

void Toolbars::_updateSection()
{
    if (!_desktop) {
        _showSection({});
        return;
    }

    // Classify the selection; a mixed or non-sectionable selection shows no
    // section at all, so bail out at the first mismatch.
    std::vector<ObjectKind> kinds;
    kinds.reserve(8);
    for (auto item : _desktop->getSelection()->items()) {
        auto const kind = classify_object(item);
        if (kind == ObjectKind::Other || (!kinds.empty() && kinds.front() != kind)) {
            kinds.clear();
            break;
        }
        kinds.push_back(kind);
    }

    auto const path = object_section_for(_tool_path, common_kind(kinds),
                                         Preferences::get()->getBool(object_sections_pref_path, true));
    _showSection(std::string{path});
}

void Toolbars::_showSection(std::string const &path)
{
    if (path == _section_path && (!_current_section || _current_section->getDesktop() == _desktop)) {
        return; // nothing to swap; the section updates itself on selection changes
    }

    if (_current_section) {
        _current_section->set_visible(false);
        _current_section->setDesktop(nullptr); // drops its selection connections
        _current_section = nullptr;
    }
    _section_path = path;

    if (path.empty()) {
        _section_separator->set_visible(false);
        return;
    }

    auto &section = _sections[path];
    if (!section) {
        // Lazily create the section from the same factory as the primaries.
        auto const &data = toolbar_data.at(path);
        section = data.create();
        section->set_name("PropertyBar" + data.name + "Section");
        section->set_hexpand(false);
        Shortcuts::getInstance().update_gui_text_recursive(section.get());
        append(*section);

        // apply_ctrlbar_settings() runs only on tool changes, but a section can
        // be created at any time: give it the current control-bar icon size.
        auto *prefs = Preferences::get();
        int const min = Inkscape::UI::Toolbar::min_pixel_size;
        int const max = Inkscape::UI::Toolbar::max_pixel_size;
        int const size = prefs->getIntLimited(Inkscape::UI::Toolbar::ctrlbars_icon_size, min, min, max);
        Inkscape::UI::set_icon_sizes(section.get(), size);
    }

    _current_section = section.get();
    _current_section->setDesktop(_desktop); // synthesizes _selectionChanged inside
    _current_section->setActiveUnit(_desktop->getNamedView()->getDisplayUnit());
    _current_section->set_visible(true);
    _section_separator->set_visible(true);
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
