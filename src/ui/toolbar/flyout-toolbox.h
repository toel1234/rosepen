// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FlyoutToolbox - Corel-style single-column toolbox with flyout tool groups.
 *
 * One button per tool group. The face shows the group's last-used tool and
 * carries a corner indicator when the group has more members; click-and-hold
 * or a click on the corner opens a popover with the other tools. Picking a
 * member switches to it, makes it the face and remembers it. Tool switches
 * from shortcuts or menus move the face the same way.
 *
 * Everything else (icons, tooltips, tool switching, visibility, icon size,
 * tool preferences) comes from the existing toolbox infrastructure; no tool
 * logic is reimplemented here.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_UI_TOOLBAR_FLYOUT_TOOLBOX_H
#define SEEN_UI_TOOLBAR_FLYOUT_TOOLBOX_H

#include <cstddef>
#include <functional>
#include <map>
#include <vector>

#include <gtkmm/box.h>
#include <sigc++/scoped_connection.h>

#include "preferences.h" // Inkscape::PrefObserver
#include "ui/toolbar/toolbox-groups.h"

namespace Gio {
class SimpleAction;
}

namespace Gtk {
class Button;
class DrawingArea;
class Image;
class Overlay;
class Popover;
class ToggleButton;
} // namespace Gtk

class InkscapeWindow;

namespace Inkscape::UI::Toolbar {

class FlyoutToolbox : public Gtk::Box
{
public:
    FlyoutToolbox(InkscapeWindow *window,
                  std::vector<ToolGroup> const &groups,
                  std::function<void(Gtk::Button &, Glib::ustring const &)> on_context_menu);
    ~FlyoutToolbox() override;

private:
    /// One popover entry: a member tool and its button.
    struct Member
    {
        Glib::ustring tool;
        Gtk::ToggleButton *button = nullptr;
    };

    /// One toolbox row: the overlay with the group face, indicator and flyout.
    struct Row
    {
        Gtk::Overlay *overlay = nullptr;
        Gtk::ToggleButton *face = nullptr;
        Gtk::Image *face_icon = nullptr;
        Gtk::DrawingArea *indicator = nullptr; // multi-member groups only
        Gtk::Popover *popover = nullptr;       // multi-member groups only
        Gtk::Box *flyout_box = nullptr;        // popover content
        std::vector<Member> members;           // empty for single-member groups
        Glib::ustring face_tool;
    };

    void build_row(std::size_t index);
    void attach_face_gestures(std::size_t index);
    void set_face(std::size_t index, Glib::ustring const &tool);
    void update_visible();
    bool open_flyout(std::size_t index);
    void on_tool_switched(Glib::ustring const &tool);
    Glib::ustring icon_for_tool(Glib::ustring const &tool) const;
    bool is_tool_visible(Glib::ustring const &tool) const;

    InkscapeWindow *_window = nullptr;
    std::vector<ToolGroup> _groups;
    std::vector<Row> _rows;
    /// tool -> icon name, harvested from the upstream toolbar-tool.ui.
    std::map<Glib::ustring, Glib::ustring> _icons;
    std::function<void(Gtk::Button &, Glib::ustring const &)> _on_context_menu;

    Inkscape::PrefObserver _buttons_observer;
    sigc::scoped_connection _tool_switch_conn;
};

} // namespace Inkscape::UI::Toolbar

#endif // SEEN_UI_TOOLBAR_FLYOUT_TOOLBOX_H

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
