// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Pages docker: a page list plus per-page size and master-layer
 * controls, driving the headless app actions.
 */
/*
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INK_PAGE_MANAGER_PANEL_H
#define INK_PAGE_MANAGER_PANEL_H

#include <gtkmm/box.h>

#include "ui/dialog/dialog-base.h"

namespace Gtk {
class Button;
class CheckButton;
class ComboBoxText;
class EditableLabel;
class ListBox;
class ListBoxRow;
class MenuButton;
class ScrolledWindow;
class SpinButton;
class ToggleButton;
} // namespace Gtk

namespace Inkscape::UI::Dialog {

class PageRow;

/**
 * A dockable panel that lists every page of the document and drives page
 * operations (insert, duplicate, remove, reorder, rename, navigate) and
 * per-page sizes (preset, custom, orientation, apply to all).
 *
 * The panel holds no page logic of its own: every operation activates a
 * machine-tested `app.page-*` action, exactly like the command line does.
 *
 * A "Master layers" section lists the top-level layers with an "All pages"
 * check that flags a layer as master via `app.layer-master-set`; the copies
 * on the other pages are drawn by the canvas, not managed here.
 */
class PageManagerPanel final : public DialogBase
{
public:
    PageManagerPanel();
    ~PageManagerPanel() final;

private:
    void documentReplaced() final;
    void desktopReplaced() final;

    void rebuild();
    void schedule_rebuild();
    bool rebuild_soon();
    bool is_editing() const;
    void update_sensitive();
    void update_size_section();
    void update_master_section();
    void update_order_buttons();

    void on_row_selected(Gtk::ListBoxRow *row);
    void connect_row_editing(PageRow &row, int position, Glib::ustring const &initial);
    void on_label_committed(int position, Gtk::EditableLabel &label, Glib::ustring const &initial);
    void on_move_relative(int delta);
    void on_preset_changed();
    void on_unit_changed();
    void on_apply_size();

    // Header actions
    Gtk::Box *_header = nullptr;
    Gtk::Button *_add_btn = nullptr;
    Gtk::MenuButton *_add_menu_btn = nullptr;
    Gtk::Button *_duplicate_btn = nullptr;
    Gtk::Button *_remove_btn = nullptr;
    Gtk::MenuButton *_remove_menu_btn = nullptr;
    Gtk::Button *_order_backwards_btn = nullptr;
    Gtk::Button *_order_forwards_btn = nullptr;

    // Page list
    Gtk::ScrolledWindow *_list_scroll = nullptr;
    Gtk::ListBox *_list_box = nullptr;
    bool _updating_list = false;
    bool _rebuild_pending = false;
    sigc::connection _rebuild_connection;

    // Per-page size section
    Gtk::Box *_size_box = nullptr;
    Gtk::ComboBoxText *_preset_combo = nullptr;
    Gtk::SpinButton *_width_spin = nullptr;
    Gtk::SpinButton *_height_spin = nullptr;
    Gtk::ComboBoxText *_unit_combo = nullptr;
    Gtk::ToggleButton *_portrait_btn = nullptr;
    Gtk::ToggleButton *_landscape_btn = nullptr;
    Gtk::CheckButton *_apply_all_btn = nullptr;
    Gtk::Button *_apply_btn = nullptr;
    Glib::ustring _size_unit = "px"; // unit the W/H values are currently in
    bool _updating_size_ui = false;

    // Master layers section
    Gtk::Box *_master_section = nullptr;
    Gtk::Box *_master_box = nullptr;
    bool _updating_master_ui = false;

    // Live update connections, rebound per document.
    sigc::scoped_connection _pages_changed;
    sigc::scoped_connection _page_selected;
    sigc::scoped_connection _page_modified;
    sigc::scoped_connection _doc_modified;
};

} // namespace Inkscape::UI::Dialog

#endif // INK_PAGE_MANAGER_PANEL_H

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
