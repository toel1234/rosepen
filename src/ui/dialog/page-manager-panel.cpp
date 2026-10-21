// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Pages docker: a page list plus per-page size controls, driving the
 * headless app.page-* actions.
 */
/*
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "page-manager-panel.h"

#include <utility>

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/variant.h>
#include <giomm/application.h>
#include <giomm/menu.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/editablelabel.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>

#include "desktop.h"
#include "document.h"
#include "object/sp-page.h"
#include "page-manager.h"
#include "util/paper.h"
#include "util/units.h"

namespace Inkscape::UI::Dialog {

/**
 * One row of the page list: position number, editable label, size text.
 */
class PageRow final : public Gtk::Box
{
public:
    PageRow(int position, Glib::ustring const &label, Glib::ustring const &size_text)
        : Gtk::Box(Gtk::Orientation::HORIZONTAL, 6)
        , pos(position)
    {
        set_margin_start(4);
        set_margin_end(4);
        set_margin_top(2);
        set_margin_bottom(2);

        auto *number = Gtk::make_managed<Gtk::Label>(Glib::ustring::format(position));
        number->set_halign(Gtk::Align::END);
        number->set_valign(Gtk::Align::CENTER);
        append(*number);

        editable = Gtk::make_managed<Gtk::EditableLabel>(label);
        editable->set_hexpand();
        editable->set_valign(Gtk::Align::CENTER);
        editable->set_halign(Gtk::Align::START);
        append(*editable);

        auto *size = Gtk::make_managed<Gtk::Label>(size_text);
        size->set_halign(Gtk::Align::END);
        size->set_valign(Gtk::Align::CENTER);
        size->add_css_class("dim-label");
        append(*size);
    }

    int pos;
    Gtk::EditableLabel *editable = nullptr;
};

namespace {

// Same definition as in 'document.cpp'
#ifndef SP_DOCUMENT_UPDATE_PRIORITY
#define SP_DOCUMENT_UPDATE_PRIORITY (G_PRIORITY_HIGH_IDLE - 2)
#endif

// Units accepted by the shared page-resize spec parser (actions-page-manager.cpp).
constexpr char const *SPEC_UNITS[] = {"px", "mm", "cm", "in", "pt", "pc"};
constexpr int NUM_SPEC_UNITS = static_cast<int>(sizeof(SPEC_UNITS) / sizeof(SPEC_UNITS[0]));

/**
 * A plain icon button for the header action bar.
 */
Gtk::Button *make_icon_button(char const *icon_name, Glib::ustring const &tooltip)
{
    auto *button = Gtk::make_managed<Gtk::Button>();
    button->set_icon_name(icon_name);
    button->set_tooltip_text(tooltip);
    return button;
}

/**
 * A small drop-down menu button offering extra variants of a header action.
 */
Gtk::MenuButton *make_menu_button(Glib::ustring const &tooltip)
{
    auto *button = Gtk::make_managed<Gtk::MenuButton>();
    button->set_icon_name("pan-down-symbolic");
    button->set_tooltip_text(tooltip);
    return button;
}

void activate_app_action(Glib::ustring const &name, Glib::VariantBase const &value = {})
{
    if (auto app = Gio::Application::get_default()) {
        app->activate_action(name, value);
    }
}

} // namespace

/*
 * Lifecycle
 */

PageManagerPanel::PageManagerPanel()
    : DialogBase("/dialogs/pages", "Pages")
{
    set_orientation(Gtk::Orientation::VERTICAL);
    set_spacing(6);

    // -- Header action bar -----------------------------------------------
    _header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    _header->set_margin_start(4);
    _header->set_margin_end(4);
    _header->set_margin_top(4);
    _header->set_valign(Gtk::Align::CENTER);

    _add_btn = make_icon_button("pages-add", _("Add a page after the current page"));
    _add_btn->signal_clicked().connect([] {
        activate_app_action("page-insert", Glib::Variant<Glib::ustring>::create("after"));
    });
    _header->append(*_add_btn);

    _add_menu_btn = make_menu_button(_("Insert a page before the current one, or at the end"));
    auto add_menu = Gio::Menu::create();
    add_menu->append(_("Insert Page Before"), "app.page-insert('before')");
    add_menu->append(_("Insert Page at End"), "app.page-insert('end')");
    _add_menu_btn->set_menu_model(add_menu);
    _header->append(*_add_menu_btn);

    _duplicate_btn = make_icon_button("pages-duplicate", _("Duplicate the current page"));
    _duplicate_btn->signal_clicked().connect([] { activate_app_action("page-duplicate-after"); });
    _header->append(*_duplicate_btn);

    _remove_btn = make_icon_button("pages-remove", _("Delete the current page, with its contents"));
    _remove_btn->signal_clicked().connect([] {
        activate_app_action("page-remove", Glib::Variant<Glib::ustring>::create("contents"));
    });
    _header->append(*_remove_btn);

    _remove_menu_btn = make_menu_button(_("Delete variants"));
    auto remove_menu = Gio::Menu::create();
    remove_menu->append(_("Delete Page, Keep Objects"), "app.page-remove('keep')");
    _remove_menu_btn->set_menu_model(remove_menu);
    _header->append(*_remove_menu_btn);

    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep->set_margin_start(6);
    sep->set_margin_end(6);
    _header->append(*sep);

    _order_backwards_btn = make_icon_button("pages-order-backwards", _("Move the page one position back"));
    _order_backwards_btn->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &PageManagerPanel::on_move_relative), -1));
    _header->append(*_order_backwards_btn);

    _order_forwards_btn = make_icon_button("pages-order-forwards", _("Move the page one position forward"));
    _order_forwards_btn->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &PageManagerPanel::on_move_relative), +1));
    _header->append(*_order_forwards_btn);

    append(*_header);

    // -- Page list ---------------------------------------------------------
    _list_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    _list_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _list_scroll->set_vexpand();
    _list_scroll->set_hexpand();

    _list_box = Gtk::make_managed<Gtk::ListBox>();
    _list_box->set_selection_mode(Gtk::SelectionMode::SINGLE);
    _list_box->signal_row_selected().connect(sigc::mem_fun(*this, &PageManagerPanel::on_row_selected));
    _list_scroll->set_child(*_list_box);

    append(*_list_scroll);

    auto *bottom_sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    append(*bottom_sep);

    // -- Per-page size section ---------------------------------------------
    _size_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    _size_box->set_margin_start(6);
    _size_box->set_margin_end(6);
    _size_box->set_margin_bottom(6);

    _size_box->append(*Gtk::make_managed<Gtk::Label>(_("Page Size"), false));

    auto *grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(6);

    _preset_combo = Gtk::make_managed<Gtk::ComboBoxText>();
    _preset_combo->set_hexpand();
    _preset_combo->append(_("Custom"));
    for (auto const &paper : Inkscape::PaperSize::getPageSizes()) {
        _preset_combo->append(paper.name);
    }
    _preset_combo->set_active(0);
    grid->attach(*_preset_combo, 1, 0);

    auto *sizes_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto width_adjustment = Gtk::Adjustment::create(0.0, 0.01, 100000.0, 1.0, 10.0);
    _width_spin = Gtk::make_managed<Gtk::SpinButton>(width_adjustment, 1.0, 2);
    _width_spin->set_hexpand();
    sizes_box->append(*_width_spin);

    auto height_adjustment = Gtk::Adjustment::create(0.0, 0.01, 100000.0, 1.0, 10.0);
    _height_spin = Gtk::make_managed<Gtk::SpinButton>(height_adjustment, 1.0, 2);
    _height_spin->set_hexpand();
    sizes_box->append(*_height_spin);

    _unit_combo = Gtk::make_managed<Gtk::ComboBoxText>();
    for (int i = 0; i < NUM_SPEC_UNITS; ++i) {
        _unit_combo->append(SPEC_UNITS[i]);
    }
    _unit_combo->set_active(0);
    sizes_box->append(*_unit_combo);
    grid->attach(*sizes_box, 1, 1);

    auto *orientation_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    _portrait_btn = Gtk::make_managed<Gtk::ToggleButton>();
    _portrait_btn->set_image_from_icon_name("page-portrait");
    _portrait_btn->set_tooltip_text(_("Portrait orientation"));
    _portrait_btn->signal_toggled().connect([this] {
        if (!_updating_size_ui && _portrait_btn->get_active()) {
            _landscape_btn->set_active(false);
        }
    });
    orientation_box->append(*_portrait_btn);

    _landscape_btn = Gtk::make_managed<Gtk::ToggleButton>();
    _landscape_btn->set_image_from_icon_name("page-landscape");
    _landscape_btn->set_tooltip_text(_("Landscape orientation"));
    _landscape_btn->signal_toggled().connect([this] {
        if (!_updating_size_ui && _landscape_btn->get_active()) {
            _portrait_btn->set_active(false);
        }
    });
    orientation_box->append(*_landscape_btn);
    grid->attach(*orientation_box, 0, 2);

    _apply_all_btn = Gtk::make_managed<Gtk::CheckButton>(_("_Apply to all pages"), true);
    _apply_all_btn->set_hexpand();
    grid->attach(*_apply_all_btn, 1, 2);

    _size_box->append(*grid);

    auto *apply_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    _apply_btn = Gtk::make_managed<Gtk::Button>(_("_Apply"), true);
    _apply_btn->set_halign(Gtk::Align::END);
    _apply_btn->signal_clicked().connect(sigc::mem_fun(*this, &PageManagerPanel::on_apply_size));
    apply_box->append(*_apply_btn);
    _size_box->append(*apply_box);

    append(*_size_box);

    // A typed size is a custom size: drop the preset so Apply uses W/H.
    auto to_custom = [this] {
        if (!_updating_size_ui) {
            _preset_combo->set_active(0);
        }
    };
    _width_spin->signal_value_changed().connect(to_custom);
    _height_spin->signal_value_changed().connect(to_custom);
    _preset_combo->signal_changed().connect(sigc::mem_fun(*this, &PageManagerPanel::on_preset_changed));
    _unit_combo->signal_changed().connect(sigc::mem_fun(*this, &PageManagerPanel::on_unit_changed));

    // Mnemonic labels, created after their widgets.
    auto *preset_label = Gtk::make_managed<Gtk::Label>(_("_Preset:"), true);
    preset_label->set_halign(Gtk::Align::START);
    preset_label->set_mnemonic_widget(*_preset_combo);
    grid->attach(*preset_label, 0, 0);

    auto *width_label = Gtk::make_managed<Gtk::Label>(_("_Size:"), true);
    width_label->set_halign(Gtk::Align::START);
    width_label->set_mnemonic_widget(*_width_spin);
    grid->attach(*width_label, 0, 1);

    // -- Initial state -------------------------------------------------------
    update_sensitive();
}

PageManagerPanel::~PageManagerPanel()
{
    _rebuild_connection.disconnect();
}

/*
 * Live update
 */

void PageManagerPanel::documentReplaced()
{
    _pages_changed.disconnect();
    _page_selected.disconnect();
    _page_modified.disconnect();
    _doc_modified.disconnect();

    if (auto const document = getDocument()) {
        auto &pm = document->getPageManager();
        _pages_changed = pm.connectPagesChanged([this](SPPage *) { schedule_rebuild(); });
        _page_selected = pm.connectPageSelected([this](SPPage *) { schedule_rebuild(); });
        _page_modified = pm.connectPageModified([this](SPPage *) { schedule_rebuild(); });
        _doc_modified = document->connectModified([this](unsigned) { schedule_rebuild(); });
    }

    schedule_rebuild();
}

void PageManagerPanel::desktopReplaced()
{
    update_sensitive();
}

void PageManagerPanel::schedule_rebuild()
{
    if (_rebuild_pending) {
        return;
    }
    _rebuild_pending = true;
    auto handler = sigc::mem_fun(*this, &PageManagerPanel::rebuild_soon);
    _rebuild_connection = Glib::signal_idle().connect(handler, SP_DOCUMENT_UPDATE_PRIORITY + 1);
}

/**
 * Rebuilding destroys rows, which would cancel an in-progress label edit, so
 * it is dropped while editing; the end of the edit schedules a fresh rebuild
 * (connect_row_editing). Rescheduling from here instead would spin a
 * high-priority idle for the whole edit and starve the redraw.
 */
bool PageManagerPanel::rebuild_soon()
{
    _rebuild_pending = false;
    if (is_editing()) {
        return false;
    }
    rebuild();
    return false;
}

bool PageManagerPanel::is_editing() const
{
    if (!_list_box) {
        return false;
    }
    for (int i = 0;; ++i) {
        auto const *row = _list_box->get_row_at_index(i);
        if (!row) {
            return false;
        }
        if (auto const *page_row = dynamic_cast<PageRow const *>(row->get_child())) {
            if (page_row->editable && page_row->editable->get_editing()) {
                return true;
            }
        }
    }
}

void PageManagerPanel::rebuild()
{
    _updating_list = true;

    while (auto *row = _list_box->get_row_at_index(0)) {
        _list_box->remove(*row);
    }

    Gtk::ListBoxRow *selected_row = nullptr;

    if (auto const document = getDocument()) {
        auto &pm = document->getPageManager();
        int const count = pm.getPageCount();
        int list_index = 0;

        if (count == 0) {
            // A single-page document has no explicit page elements; the root
            // viewBox is the implicit page 1 and must stay implicit.
            auto label = Glib::ustring::compose(_("Page %1"), 1);
            _list_box->append(*Gtk::make_managed<PageRow>(1, label, pm.getSizeLabel(nullptr)));

            if (auto *row = _list_box->get_row_at_index(0)) {
                if (auto *page_row = dynamic_cast<PageRow *>(row->get_child())) {
                    connect_row_editing(*page_row, 1, label);
                }
            }
        } else {
            for (int i = 0; i < count; ++i) {
                auto *page = pm.getPage(i);
                if (!page) {
                    continue;
                }
                auto const label = Glib::ustring(page->getLabel());
                _list_box->append(*Gtk::make_managed<PageRow>(i + 1, label, pm.getSizeLabel(page)));

                if (auto *row = _list_box->get_row_at_index(list_index)) {
                    if (auto *page_row = dynamic_cast<PageRow *>(row->get_child())) {
                        connect_row_editing(*page_row, i + 1, label);
                    }
                    if (page == pm.getSelected()) {
                        selected_row = row;
                    }
                }
                ++list_index;
            }
        }
    }

    if (selected_row) {
        _list_box->select_row(*selected_row);
    }

    _updating_list = false;

    update_sensitive();
    update_size_section();
}

void PageManagerPanel::update_sensitive()
{
    auto const document = getDocument();
    bool const have_document = document != nullptr;
    bool const have_pages = have_document && document->getPageManager().hasPages();
    // page-remove refuses the last page, so do not offer it.
    bool const can_remove = have_document && document->getPageManager().getPageCount() > 1;

    _add_btn->set_sensitive(have_document);
    _add_menu_btn->set_sensitive(have_document);
    _duplicate_btn->set_sensitive(have_pages);
    _remove_btn->set_sensitive(can_remove);
    _remove_menu_btn->set_sensitive(can_remove);
    _list_scroll->set_sensitive(have_document);

    update_order_buttons();
}

void PageManagerPanel::update_order_buttons()
{
    bool backwards = false;
    bool forwards = false;

    if (auto const document = getDocument()) {
        auto &pm = document->getPageManager();
        int const position = pm.getSelectedPageIndex() + 1;
        if (position > 0 && pm.getPageCount() > 1) {
            backwards = position > 1;
            forwards = position < pm.getPageCount();
        }
    }

    _order_backwards_btn->set_sensitive(backwards);
    _order_forwards_btn->set_sensitive(forwards);
}

/*
 * Interactions
 */

void PageManagerPanel::on_row_selected(Gtk::ListBoxRow *row)
{
    if (_updating_list || !row) {
        return;
    }
    auto const *page_row = dynamic_cast<PageRow const *>(row->get_child());
    if (!page_row) {
        return;
    }

    activate_app_action("page-goto", Glib::Variant<int>::create(page_row->pos));

    // GUI-only convenience: bring the navigated page to the viewport center.
    if (auto const desktop = getDesktop()) {
        if (auto const document = getDocument()) {
            auto &pm = document->getPageManager();
            if (auto *page = pm.getPage(page_row->pos - 1)) {
                pm.centerToPage(desktop, page);
            }
        }
    }
}

/**
 * Connects the commit handler of a row's editable label. The label text at
 * build time is captured so that an unchanged commit records no undo step.
 */
void PageManagerPanel::connect_row_editing(PageRow &row, int position, Glib::ustring const &initial)
{
    auto *edit = row.editable;
    edit->property_editing().signal_changed().connect([this, edit, position, initial] {
        if (!edit->get_editing()) {
            on_label_committed(position, *edit, initial);
            // Pick up any rebuild that was dropped while the edit was open.
            schedule_rebuild();
        }
    });
}

void PageManagerPanel::on_label_committed(int position, Gtk::EditableLabel &label, Glib::ustring const &initial)
{
    auto const document = getDocument();
    if (!document) {
        return;
    }
    auto &pm = document->getPageManager();

    if (label.get_text() == initial) {
        return;
    }

    auto *page = pm.getPage(position - 1);
    if (!page && pm.getPageCount() != 0) {
        return; // stale row without a backing page
    }

    // page-rename targets the current page, so navigate first when needed.
    // On the implicit single page (no explicit pages yet) renaming with a
    // non-empty name materializes page 1, handled inside the action.
    if (page && pm.getSelectedPageIndex() + 1 != position) {
        activate_app_action("page-goto", Glib::Variant<int>::create(position));
    }
    activate_app_action("page-rename", Glib::Variant<Glib::ustring>::create(label.get_text()));
}

void PageManagerPanel::on_move_relative(int delta)
{
    auto const document = getDocument();
    if (!document) {
        return;
    }
    auto &pm = document->getPageManager();
    int const position = pm.getSelectedPageIndex() + 1;
    if (position <= 0) {
        return;
    }
    int const target = position + delta;
    if (target < 1 || target > pm.getPageCount()) {
        return;
    }
    activate_app_action("page-move-to", Glib::Variant<int>::create(target));
}

/*
 * Per-page size section
 */

void PageManagerPanel::update_size_section()
{
    _updating_size_ui = true;

    auto const document = getDocument();

    Geom::Rect rect;
    std::string preset_label;
    bool have_page = false;

    if (document) {
        auto &pm = document->getPageManager();
        if (auto *page = pm.getSelected()) {
            rect = page->getDocumentRect();
            preset_label = page->getSizeLabel();
            have_page = true;
        } else if (pm.getPageCount() == 0) {
            // The implicit single page: the document bounds, in px like
            // getDocumentRect (the root viewBox is in user units).
            if (auto const bounds = document->preferredBounds()) {
                rect = *bounds;
                have_page = true;
            }
        }
    }

    _size_box->set_sensitive(have_page);

    if (have_page) {
        // Unit menu: the document display unit, unless the shared spec parser
        // does not accept it (then px).
        Glib::ustring abbr = "px";
        if (auto const *display = document->getDisplayUnit()) {
            for (int i = 0; i < NUM_SPEC_UNITS; ++i) {
                if (display->abbr == SPEC_UNITS[i]) {
                    abbr = SPEC_UNITS[i];
                    break;
                }
            }
        }
        auto const *unit = Inkscape::Util::UnitTable::get().getUnit(abbr.raw());
        auto const *px = Inkscape::Util::UnitTable::get().getUnit("px");

        if (unit && px) {
            _width_spin->set_value(Inkscape::Util::Quantity::convert(rect.width(), px, unit));
            _height_spin->set_value(Inkscape::Util::Quantity::convert(rect.height(), px, unit));
        }
        _unit_combo->set_active_text(abbr);
        _size_unit = abbr;

        int const preset_index = [&] {
            if (preset_label.empty()) {
                return 0;
            }
            auto const &sizes = Inkscape::PaperSize::getPageSizes();
            for (std::size_t i = 0; i < sizes.size(); ++i) {
                if (g_ascii_strcasecmp(sizes[i].name.c_str(), preset_label.c_str()) == 0) {
                    return static_cast<int>(i) + 1;
                }
            }
            return 0;
        }();
        _preset_combo->set_active(preset_index);

        bool const landscape = rect.width() > rect.height();
        _portrait_btn->set_active(have_page && !landscape);
        _landscape_btn->set_active(landscape);

        _apply_all_btn->set_active(false);
    }

    _updating_size_ui = false;
}

/**
 * Picking a preset shows its size in the W/H fields (current unit, current
 * orientation toggle), so the fields never contradict what Apply will do.
 */
void PageManagerPanel::on_preset_changed()
{
    if (_updating_size_ui) {
        return;
    }
    int const preset_index = _preset_combo->get_active_row_number();
    auto const &sizes = Inkscape::PaperSize::getPageSizes();
    auto const *unit = Inkscape::Util::UnitTable::get().getUnit(_size_unit.raw());
    if (preset_index <= 0 || preset_index > static_cast<int>(sizes.size()) || !unit) {
        return;
    }
    auto const &paper = sizes[preset_index - 1];
    double width = Inkscape::Util::Quantity::convert(paper.width, paper.unit, unit);
    double height = Inkscape::Util::Quantity::convert(paper.height, paper.unit, unit);
    if ((_landscape_btn->get_active() && width < height) || (_portrait_btn->get_active() && width > height)) {
        std::swap(width, height);
    }

    _updating_size_ui = true;
    _width_spin->set_value(width);
    _height_spin->set_value(height);
    _updating_size_ui = false;
}

/**
 * Changing the unit converts the W/H values instead of reinterpreting them.
 */
void PageManagerPanel::on_unit_changed()
{
    if (_updating_size_ui) {
        return;
    }
    auto const abbr = _unit_combo->get_active_text();
    auto &table = Inkscape::Util::UnitTable::get();
    auto const *from = table.getUnit(_size_unit.raw());
    auto const *to = table.getUnit(abbr.raw());
    if (from && to && from != to) {
        _updating_size_ui = true;
        _width_spin->set_value(Inkscape::Util::Quantity::convert(_width_spin->get_value(), from, to));
        _height_spin->set_value(Inkscape::Util::Quantity::convert(_height_spin->get_value(), from, to));
        _updating_size_ui = false;
    }
    _size_unit = abbr;
}

void PageManagerPanel::on_apply_size()
{
    auto const document = getDocument();
    if (!document) {
        return;
    }

    // Build the shared spec string: "<preset>|<w>x<h><unit>[,portrait|landscape][,all]".
    std::string spec;
    if (int const preset_index = _preset_combo->get_active_row_number(); preset_index > 0) {
        auto const &sizes = Inkscape::PaperSize::getPageSizes();
        if (preset_index <= static_cast<int>(sizes.size())) {
            spec = sizes[preset_index - 1].name;
        }
    } else {
        double const width = _width_spin->get_value();
        double const height = _height_spin->get_value();
        if (width <= 0 || height <= 0) {
            return;
        }
        char buffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buffer, sizeof(buffer), "%.3f", width);
        spec = buffer;
        spec += 'x';
        g_ascii_formatd(buffer, sizeof(buffer), "%.3f", height);
        spec += buffer;
        if (auto const unit = _unit_combo->get_active_text(); !unit.empty()) {
            spec += unit.raw();
        }
    }
    if (spec.empty()) {
        return;
    }

    if (_landscape_btn->get_active()) {
        spec += ",landscape";
    } else if (_portrait_btn->get_active()) {
        spec += ",portrait";
    }
    if (_apply_all_btn->get_active()) {
        spec += ",all";
    }

    activate_app_action("page-resize", Glib::Variant<Glib::ustring>::create(spec));
}

} // namespace Inkscape::UI::Dialog

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
