// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Toolbar for Commands.
 */
/* Authors:
 *   Tavmjong Bah
 *   Vaibhav Malik <vaibhavmalik2018@gmail.com>
 *
 * Copyright (C) 2023 Tavmjong Bah
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "command-toolbar.h"

#include <gtkmm/box.h>

#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/toolbar/toolbox-groups.h"
#include "ui/workspace-defaults.h"

namespace Inkscape::UI::Toolbar {

namespace {

// Rosepen: the Corel standard bar unless the classic toolbox layout is chosen.
std::string commands_ui_file()
{
    auto const layout = Preferences::get()->getString(toolbox_layout_path);
    return Workspace::commandbar_ui_file(layout.raw());
}

} // namespace

CommandToolbar::CommandToolbar()
    : ToolbarWidget{get_widget<Gtk::Box>(create_builder(commands_ui_file().c_str()), "commands-toolbar")}
{
    _initMenuBtns();
}

CommandToolbar::~CommandToolbar() = default;

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
