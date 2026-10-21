// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FlyoutToolbox - Corel-style single-column toolbox with flyout tool groups.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/toolbar/flyout-toolbox.h"

#include <glibmm/variant.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/image.h>
#include <gtkmm/overlay.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>

#include <giomm/simpleaction.h>

#include "actions/actions-tools.h" // tool_preferences()
#include "inkscape-application.h"
#include "inkscape-window.h" // lookup_action()
#include "ui/builder-utils.h"
#include "ui/controller.h"
#include "ui/toolbar/toolbar-constants.h" // tools_icon_size, min/max_pixel_size
#include "ui/toolbar/tool-toolbar.h"      // get_tool_visible_button_path()
#include "ui/util.h"                      // set_icon_sizes, for_each_descendant, get_color

namespace Inkscape::UI::Toolbar {

namespace {

/// Same pref subtree as the classic toolbox visibility toggles.
constexpr char const *buttons_path = "/toolbox/tools/buttons";

/// Side of the square corner indicator, in px. Kept small like CorelDRAW's.
constexpr int indicator_size = 7;

/// Fraction of the face that counts as its bottom-right flyout corner.
constexpr double corner_fraction = 0.6;

/**
 * Collect tool -> icon-name from the upstream toolbar-tool.ui builder. No
 * icon is duplicated here: the button's own "icon-name" (Text) wins, else
 * the first descendant Gtk::Image's icon name. The builder is discarded.
 */
std::map<Glib::ustring, Glib::ustring> harvest_tool_icons()
{
    std::map<Glib::ustring, Glib::ustring> icons;

    auto builder = create_builder("toolbar-tool.ui");
    for (auto &object : builder->get_objects()) {
        auto const button = dynamic_cast<Gtk::ToggleButton *>(object.get());
        if (!button) {
            continue;
        }

        Glib::VariantBase target;
        button->get_property("action-target", target);
        if (!target.is_of_type(Glib::VARIANT_TYPE_STRING)) {
            continue;
        }
        auto const tool = Glib::ustring(static_cast<gchar const *>(target.get_data()));

        Glib::ustring icon;
        button->get_property("icon-name", icon);
        if (icon.empty()) {
            for_each_descendant(*button, [&](Gtk::Widget &widget) {
                if (auto const image = dynamic_cast<Gtk::Image *>(&widget)) {
                    Glib::ustring name;
                    image->get_property("icon-name", name);
                    if (!name.empty()) {
                        icon = name;
                        return ForEachResult::_break;
                    }
                }
                return ForEachResult::_continue;
            });
        }
        if (icon.empty()) {
            g_warning("FlyoutToolbox: no icon found for tool '%s'; using 'image-missing'", tool.c_str());
            icon = "image-missing";
        }
        icons.emplace(tool, icon);
    }
    return icons;
}

/// Same tooltip call as the tool preferences page.
Glib::ustring tooltip_for_tool(Glib::ustring const &tool)
{
    auto *iapp = InkscapeApplication::instance();
    if (!iapp) {
        return {};
    }
    return iapp->get_action_extra_data().get_tooltip_for_action("win.tool-switch('" + tool + "')", true, true);
}

/// Whether (x, y) lies in the widget's bottom-right corner.
bool in_corner(double x, double y, Gtk::Widget const &widget)
{
    return x >= widget.get_width() * corner_fraction && y >= widget.get_height() * corner_fraction;
}

} // namespace

FlyoutToolbox::FlyoutToolbox(InkscapeWindow *window,
                             std::vector<ToolGroup> const &groups,
                             std::function<void(Gtk::Button &, Glib::ustring const &)> on_context_menu)
    : Gtk::Box(Gtk::Orientation::VERTICAL)
    , _window(window)
    , _groups(groups)
    , _on_context_menu(std::move(on_context_menu))
{
    set_name("FlyoutToolbox");

    _icons = harvest_tool_icons();

    auto const scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::EXTERNAL);
    scrolled->set_halign(Gtk::Align::START);
    scrolled->set_vexpand(true);

    auto const list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    scrolled->set_child(*list);

    _rows.resize(_groups.size());
    for (std::size_t index = 0; index < _groups.size(); ++index) {
        build_row(index);
        list->append(*_rows[index].overlay);
    }

    append(*scrolled);

    // The face follows tool switches from shortcuts, menus and the canvas.
    if (auto const action = std::dynamic_pointer_cast<Gio::SimpleAction>(window->lookup_action("tool-switch"))) {
        _tool_switch_conn = action->property_state().signal_changed().connect([this, action] {
            Glib::ustring tool;
            action->get_state(tool);
            on_tool_switched(tool);
        });

        // Start in sync with the window's current tool.
        Glib::ustring current;
        action->get_state(current);
        on_tool_switched(current);
    }

    // Same visibility prefs as the classic toolbox.
    _buttons_observer = Inkscape::Preferences::get()->createObserver(buttons_path, [this] { update_visible(); });
    update_visible();
}

FlyoutToolbox::~FlyoutToolbox()
{
    for (auto &row : _rows) {
        if (row.popover) {
            row.popover->popdown();
            row.popover->unparent();
        }
    }
}

void FlyoutToolbox::build_row(std::size_t index)
{
    auto &group = _groups.at(index);
    auto &row = _rows.at(index);

    row.overlay = Gtk::make_managed<Gtk::Overlay>();

    // Face: shows the group's active tool; a plain click switches to it.
    auto const face = Gtk::make_managed<Gtk::ToggleButton>();
    face->set_has_frame(false);
    face->set_focus_on_click(false);
    face->add_css_class("square-button");
    face->set_action_name("win.tool-switch");
    row.face = face;

    row.face_icon = Gtk::make_managed<Gtk::Image>();
    face->set_child(*row.face_icon);
    row.overlay->set_child(*face);

    // Multi-member groups get a corner indicator and a flyout popover.
    if (group.tools.size() > 1) {
        // Code-drawn triangle: a Gtk::Image here would be resized by
        // set_icon_sizes() along with the tool icons.
        auto const indicator = Gtk::make_managed<Gtk::DrawingArea>();
        indicator->set_content_width(indicator_size);
        indicator->set_content_height(indicator_size);
        indicator->set_halign(Gtk::Align::END);
        indicator->set_valign(Gtk::Align::END);
        indicator->set_margin_end(2);
        indicator->set_margin_bottom(2);
        indicator->set_can_target(false); // clicks pass through to the face
        indicator->add_css_class("flyout-indicator");
        indicator->set_draw_func([indicator](Cairo::RefPtr<Cairo::Context> const &cr, int width, int height) {
            auto const fg = ::get_color(*indicator); // CSS foreground, follows the theme
            cr->set_source_rgba(fg.red, fg.green, fg.blue, fg.alpha);
            cr->move_to(0, 0);
            cr->line_to(width, height / 2.0);
            cr->line_to(0, height);
            cr->close_path();
            cr->fill();
        });
        row.indicator = indicator;
        row.overlay->add_overlay(*indicator);

        auto const popover = Gtk::make_managed<Gtk::Popover>();
        popover->set_position(Gtk::PositionType::RIGHT);
        popover->set_has_arrow(false);
        row.flyout_box = Gtk::make_managed<Gtk::Box>();
        popover->set_child(*row.flyout_box);
        popover->set_parent(*row.overlay);
        row.popover = popover;
    }

    // Flyout members. Single-member groups have no popover; their only
    // member is the face.
    for (auto const &tool : group.tools) {
        if (!row.popover) {
            break;
        }

        auto const button = Gtk::make_managed<Gtk::ToggleButton>();
        button->set_has_frame(false);
        button->set_focus_on_click(false);
        button->add_css_class("square-button");
        button->set_action_name("win.tool-switch");
        button->set_action_target_value(Glib::Variant<Glib::ustring>::create(tool));
        button->set_tooltip_markup(tooltip_for_tool(tool));

        auto const icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name(icon_for_tool(tool));
        button->set_child(*icon);

        button->signal_clicked().connect([this, index, tool] {
            set_active_tool(_groups[index], tool);
            set_face(index, tool);
            if (auto const popover = _rows[index].popover) {
                popover->popdown();
            }
        });

        row.flyout_box->append(*button);
        row.members.push_back({tool, button});
    }

    if (auto const active = active_tool(group); !active.empty()) {
        set_face(index, active); // visibility correction happens in update_visible()
    }

    attach_face_gestures(index);
}

/**
 * Face gestures, using the CAPTURE idiom of ToolToolbar::attachHandlers:
 *  - long-press button 1 opens the flyout and claims the sequence, so the
 *    release does not switch tools (Corel click-and-hold);
 *  - a single press in the bottom-right corner opens the flyout and claims;
 *  - double-click opens the tool preferences;
 *  - button 3 opens the context menu via the caller's callback;
 *  - a plain click elsewhere falls through to the win.tool-switch action.
 */
void FlyoutToolbox::attach_face_gestures(std::size_t index)
{
    auto const face = _rows.at(index).face;

    auto const click = Gtk::GestureClick::create();
    click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    click->set_button(0); // any
    click->signal_pressed().connect(Controller::use_state(
        [this, index](Gtk::GestureClick &click, int n_press, double x, double y) {
            auto &row = _rows[index];
            switch (click.get_current_button()) {
                case 1:
                    if (n_press == 2) {
                        tool_preferences(row.face_tool, _window);
                        return Gtk::EventSequenceState::CLAIMED;
                    }
                    // Claim only when a flyout opened; with a single visible
                    // member the corner click falls through to the tool switch.
                    if (n_press == 1 && row.popover && in_corner(x, y, *row.face) && open_flyout(index)) {
                        return Gtk::EventSequenceState::CLAIMED;
                    }
                    break;
                case 3:
                    _on_context_menu(*row.face, row.face_tool);
                    return Gtk::EventSequenceState::CLAIMED;
                default:
                    break;
            }
            return Gtk::EventSequenceState::NONE; // plain click falls through to the action
        },
        *click),
        true);
    face->add_controller(click);

    if (_rows.at(index).popover) {
        auto const press = Gtk::GestureLongPress::create();
        press->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        press->set_button(1);
        press->signal_pressed().connect(Controller::use_state(
            [this, index](Gtk::GestureLongPress &, double, double) {
                // Claiming keeps the release from triggering the action; with
                // no flyout to show, the release still switches to the face.
                return open_flyout(index) ? Gtk::EventSequenceState::CLAIMED
                                          : Gtk::EventSequenceState::NONE;
            },
            *press),
            true);
        face->add_controller(press);
    }
}

/// Show @a tool on the group's face, reusing the same icon image and tooltip.
void FlyoutToolbox::set_face(std::size_t index, Glib::ustring const &tool)
{
    auto &row = _rows.at(index);
    if (row.face_tool == tool) {
        return;
    }
    row.face_tool = tool;
    // Changing the target re-syncs the active state of every tool button.
    row.face->set_action_target_value(Glib::Variant<Glib::ustring>::create(tool));
    // Same image object, so the icon size set by desktop-widget survives.
    row.face_icon->set_from_icon_name(icon_for_tool(tool));
    row.face->set_tooltip_markup(tooltip_for_tool(tool));
}

/**
 * Rebuild member visibility from /toolbox/tools/buttons. Groups without a
 * visible member are hidden; a group reduced to one visible member loses
 * its indicator and flyout; a hidden face falls back to active_tool().
 */
void FlyoutToolbox::update_visible()
{
    for (std::size_t index = 0; index < _rows.size(); ++index) {
        auto &row = _rows[index];
        auto &group = _groups[index];

        int visible_members = 0;
        for (auto &member : row.members) {
            bool const show = is_tool_visible(member.tool);
            member.button->set_visible(show);
            visible_members += show;
        }
        if (row.members.empty()) {
            visible_members = is_tool_visible(group.tools.front()) ? 1 : 0;
        }

        row.overlay->set_visible(visible_members > 0);
        if (row.indicator) {
            row.indicator->set_visible(visible_members > 1);
        }

        if (visible_members > 0 && !is_tool_visible(row.face_tool)) {
            auto visible = [this](Glib::ustring const &tool) { return is_tool_visible(tool); };
            set_face(index, active_tool(group, visible));
        }
    }
}

bool FlyoutToolbox::open_flyout(std::size_t index)
{
    auto &row = _rows.at(index);
    if (!row.popover) {
        return false;
    }

    // No flyout behind a single visible member.
    int visible_members = 0;
    for (auto const &member : row.members) {
        visible_members += is_tool_visible(member.tool);
    }
    if (visible_members <= 1) {
        return false;
    }

    // Popover contents do not inherit the toolbox icon size; apply it per popup,
    // clamped like desktop-widget.
    auto const prefs = Inkscape::Preferences::get();
    auto const size = prefs->getIntLimited(tools_icon_size, min_pixel_size, min_pixel_size, max_pixel_size);
    set_icon_sizes(row.flyout_box, size);

    row.popover->popup();
    return true;
}

void FlyoutToolbox::on_tool_switched(Glib::ustring const &tool)
{
    auto const index = group_index_for_tool(_groups, tool);
    if (!index) {
        return; // not a grouped tool (e.g. Marker/Picker): nothing to do
    }
    if (_rows[*index].face_tool == tool) {
        return;
    }
    set_active_tool(_groups[*index], tool); // remember the last-used member
    set_face(*index, tool);
}

Glib::ustring FlyoutToolbox::icon_for_tool(Glib::ustring const &tool) const
{
    auto const it = _icons.find(tool);
    return it != _icons.end() ? it->second : Glib::ustring("image-missing");
}

bool FlyoutToolbox::is_tool_visible(Glib::ustring const &tool) const
{
    return Inkscape::Preferences::get()->getBool(ToolToolbar::get_tool_visible_button_path(tool), true);
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
