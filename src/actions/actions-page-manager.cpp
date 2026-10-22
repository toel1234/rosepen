// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for page management, application level and headless.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "actions-page-manager.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>
#include <glibmm/i18n.h>
#include <giomm.h>

#include "document-undo.h"
#include "inkscape-application.h"
#include "page-manager.h"
#include "util/paper.h"
#include "util/units.h"

#include "document.h"
#include "object/sp-page.h"
#include "ui/icon-names.h"

namespace {

using Inkscape::PaperSize;
using Inkscape::Util::Quantity;
using Inkscape::Util::Unit;
using Inkscape::Util::UnitTable;

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

Unit const *px_unit()
{
    return UnitTable::get().getUnit("px");
}

/**
 * Find a paper preset by name, case-insensitively. Presets are matched before
 * the numeric grammar so that names containing an "x" (e.g. "Banner 468x60")
 * are not mistaken for dimensions.
 */
PaperSize const *find_preset(std::string const &name)
{
    for (auto const &paper : PaperSize::getPageSizes()) {
        if (g_ascii_strcasecmp(paper.name.c_str(), name.c_str()) == 0) {
            return &paper;
        }
    }
    // Accept the common short form of US presets ("Letter" for "US Letter").
    for (auto const &paper : PaperSize::getPageSizes()) {
        if (paper.name.rfind("US ", 0) == 0 &&
            g_ascii_strcasecmp(paper.name.c_str() + 3, name.c_str()) == 0) {
            return &paper;
        }
    }
    return nullptr;
}

Unit const *resolve_unit(std::string unit)
{
    std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
    if (unit == "\"") {
        unit = "in";
    }
    static char const *const allowed[] = {"px", "mm", "cm", "in", "pt", "pc"};
    if (std::find_if(std::begin(allowed), std::end(allowed),
                     [&unit](char const *candidate) { return unit == candidate; }) == std::end(allowed)) {
        return nullptr;
    }
    return UnitTable::get().getUnit(unit);
}

bool parse_number(std::string const &text, double &out)
{
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    double value = g_ascii_strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    if (!std::isfinite(value) || value <= 0) {
        return false;
    }
    out = value;
    return true;
}

std::string format_user_unit(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

/**
 * Strip a trailing ",token" from spec. Used for the optional resize modifiers.
 */
bool take_suffix(std::string &spec, char const *token)
{
    auto const pos = spec.rfind(',');
    if (pos == std::string::npos) {
        return false;
    }
    if (!equal_ci(trim(spec.substr(pos + 1)), token)) {
        return false;
    }
    spec = trim(spec.substr(0, pos));
    return true;
}

std::string preset_name_for(std::string const &spec)
{
    if (auto const *preset = find_preset(trim(spec))) {
        return preset->name;
    }
    return {};
}

/**
 * SPPage::setRect always clears the size label and leaves putting it back to
 * the caller (sp-page.cpp, "toolbar is responsible"). Reordering pages swaps
 * their rects and deleting a page refits the document, both through setRect,
 * without resizing anything. Remember the labels before such an op and put
 * them back on every surviving page whose size did not change.
 */
class SizeLabelKeeper
{
public:
    explicit SizeLabelKeeper(SPDocument *doc)
        : _doc(doc)
    {
        for (auto *page : doc->getPageManager().getPages()) {
            if (auto label = page->getSizeLabel(); !label.empty()) {
                _saved.push_back({page, std::move(label), page->getRect().dimensions()});
            }
        }
    }

    void restore()
    {
        if (_saved.empty()) {
            return;
        }
        _doc->ensureUpToDate();
        auto const &pages = _doc->getPageManager().getPages();
        for (auto &entry : _saved) {
            // Pointer identity only: a deleted page is never dereferenced.
            if (std::find(pages.begin(), pages.end(), entry.page) == pages.end()) {
                continue;
            }
            if (entry.page->getSizeLabel().empty() &&
                Geom::are_near(entry.page->getRect().dimensions(), entry.size, 1e-6)) {
                entry.page->setSizeLabel(entry.label);
            }
        }
    }

private:
    struct Entry
    {
        SPPage *page;
        std::string label;
        Geom::Point size;
    };
    SPDocument *_doc;
    std::vector<Entry> _saved;
};

} // namespace

namespace Inkscape::PageOps {

SPPage *current_page(SPDocument *doc)
{
    if (!doc) {
        return nullptr;
    }
    auto &pm = doc->getPageManager();
    if (auto page = pm.getSelected()) {
        return page;
    }
    if (pm.getPageCount() > 0) {
        return pm.getPage(0);
    }
    return nullptr;
}

bool goto_page(SPDocument *doc, int pos)
{
    if (!doc) {
        return false;
    }
    auto &pm = doc->getPageManager();

    // The implicit single page is position 1 and must stay implicit: selecting
    // it would materialize pages as a side effect of a read-only navigation.
    if (pm.getPageCount() == 0) {
        if (pos == 1) {
            return true;
        }
        g_warning("page-goto: no page %d in a single-page document", pos);
        return false;
    }
    if (pos < 1 || pos > pm.getPageCount()) {
        g_warning("page-goto: no page %d (document has %d)", pos, pm.getPageCount());
        return false;
    }
    return pm.selectPage(pos - 1);
}

bool step_page(SPDocument *doc, int delta)
{
    if (!doc || delta == 0) {
        return false;
    }
    auto &pm = doc->getPageManager();
    int const count = pm.getPageCount();

    // The implicit single page is not a stepping target: materializing it as a
    // side effect of a navigation would change the document. There is also
    // nowhere to step to, so the actions stop there.
    if (count == 0) {
        return false;
    }

    // No wrap: CorelDRAW's PgUp/PgDn stop at the first and last page. The
    // bounds check must come before selectPage(), which would deselect the
    // page for an out-of-range index.
    int const target = pm.getSelectedPageIndex() + delta;
    if (target < 0 || target >= count) {
        return false;
    }
    return pm.selectPage(target);
}

bool move_to(SPDocument *doc, SPPage *page, int pos)
{
    if (!doc || !page) {
        return false;
    }
    auto &pm = doc->getPageManager();
    int const count = pm.getPageCount();
    if (count <= 0) {
        return false;
    }
    int const target = std::clamp(pos, 1, count) - 1;
    int current = page->getPageIndex();
    if (current < 0 || current == target) {
        return false;
    }

    // setPageIndex does not shift intermediate pages, so bubble the page with
    // a chain of adjacent swaps. Contents follow because move_objects() is
    // passed through to SPPage::swapPage. Object bounds are updated lazily,
    // so refresh between steps or a later swap will not see items moved by an
    // earlier one.
    bool const swap = PageManager::move_objects();
    bool moved = false;
    int guard = count + 2;
    doc->ensureUpToDate();
    SizeLabelKeeper labels(doc);
    while (current > target && guard-- > 0) {
        if (!page->setPageIndex(current - 1, swap)) {
            break;
        }
        moved = true;
        doc->ensureUpToDate();
        int const now = page->getPageIndex();
        if (now < 0 || now >= current) {
            break;
        }
        current = now;
    }
    while (current < target && guard-- > 0) {
        if (!page->setPageIndex(current + 1, swap)) {
            break;
        }
        moved = true;
        doc->ensureUpToDate();
        int const now = page->getPageIndex();
        if (now < 0 || now <= current) {
            break;
        }
        current = now;
    }
    labels.restore();
    return moved && page->getPageIndex() == target;
}

SPPage *insert_page(SPDocument *doc, Where where)
{
    if (!doc) {
        return nullptr;
    }
    auto &pm = doc->getPageManager();
    pm.enablePages();

    auto const selected = pm.getSelected();
    int const base = selected ? selected->getPagePosition() : 1;

    auto page = pm.newPage();
    if (!page) {
        return nullptr;
    }
    if (where != Where::End) {
        int const target = (where == Where::Before) ? base : base + 1;
        move_to(doc, page, target);
    }
    pm.selectPage(page);
    return page;
}

SPPage *duplicate_after(SPDocument *doc)
{
    if (!doc) {
        return nullptr;
    }
    auto &pm = doc->getPageManager();
    pm.enablePages();

    auto const selected = pm.getSelected();
    int const base = selected ? selected->getPagePosition() : 1;

    auto page = pm.duplicatePage();
    if (!page) {
        return nullptr;
    }
    move_to(doc, page, base + 1);
    pm.selectPage(page);
    return page;
}

bool remove_page(SPDocument *doc, bool contents)
{
    if (!doc) {
        return false;
    }
    auto &pm = doc->getPageManager();
    if (pm.getPageCount() <= 1) {
        g_warning("page-remove: refusing to remove the last page (document has %d)", pm.getPageCount());
        return false;
    }
    auto page = current_page(doc);
    if (!page) {
        return false;
    }
    // Deleting by contents inspects item bounds; make sure pending transforms
    // are visible to the page overlap tests.
    doc->ensureUpToDate();
    SizeLabelKeeper labels(doc);
    pm.deletePage(page, contents);
    labels.restore();
    return true;
}

bool rename_page(SPDocument *doc, std::string_view text)
{
    if (!doc) {
        return false;
    }
    auto &pm = doc->getPageManager();
    std::string const value(text);
    if (!pm.hasPages()) {
        // The implicit page is only materialized when it gets a real label;
        // clearing an empty label never needs pages.
        if (value.empty()) {
            return false;
        }
        pm.enablePages();
    }
    auto page = current_page(doc);
    if (!page) {
        return false;
    }
    page->setLabel(value.empty() ? nullptr : value.c_str());
    return true;
}

std::optional<Geom::Point> parse_size(SPDocument *doc, std::string_view spec)
{
    if (!doc) {
        return std::nullopt;
    }
    std::string const text = trim(std::string(spec));
    if (text.empty()) {
        return std::nullopt;
    }

    auto const *px = px_unit();

    // Presets first: some preset names contain digits and an "x".
    if (auto const *preset = find_preset(text)) {
        double const width = Quantity::convert(preset->width, preset->unit, px);
        double const height = Quantity::convert(preset->height, preset->unit, px);
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            return std::nullopt;
        }
        return Geom::Point(width, height);
    }

    // Split off the trailing unit first ("50cm", "50 mm", "6in", "6\""): unit
    // names such as "px" contain an "x" and must not be taken for the
    // separator of the "WxH" form.
    auto unit_end = text.size();
    while (unit_end > 0 && (g_ascii_isalpha(text[unit_end - 1]) || text[unit_end - 1] == '"')) {
        --unit_end;
    }
    std::string const unit_text = text.substr(unit_end);
    std::string const dims = trim(text.substr(0, unit_end));

    std::string width_text;
    std::string height_text;

    auto const xpos = dims.find_first_of("xX");
    if (xpos != std::string::npos) {
        width_text = trim(dims.substr(0, xpos));
        height_text = trim(dims.substr(xpos + 1));
        // In the "WxH" form a comma is the decimal separator.
        std::replace(width_text.begin(), width_text.end(), ',', '.');
        std::replace(height_text.begin(), height_text.end(), ',', '.');
    } else {
        auto const cpos = dims.find(',');
        if (cpos == std::string::npos) {
            return std::nullopt;
        }
        width_text = trim(dims.substr(0, cpos));
        height_text = trim(dims.substr(cpos + 1));
    }

    double width = 0;
    double height = 0;
    if (!parse_number(width_text, width) || !parse_number(height_text, height)) {
        return std::nullopt;
    }

    Unit const *unit = doc->getDisplayUnit();
    if (!unit_text.empty()) {
        unit = resolve_unit(unit_text);
        if (!unit) {
            return std::nullopt;
        }
    }
    if (!unit || !px) {
        return std::nullopt;
    }

    double const width_px = Quantity::convert(width, unit, px);
    double const height_px = Quantity::convert(height, unit, px);
    if (!std::isfinite(width_px) || !std::isfinite(height_px) || width_px <= 0 || height_px <= 0) {
        return std::nullopt;
    }
    return Geom::Point(width_px, height_px);
}

bool resize_page(SPDocument *doc, SPPage *page, Geom::Point px, std::optional<Orientation> orientation,
                 std::string preset_label)
{
    if (!doc || !page) {
        return false;
    }
    double width = px.x();
    double height = px.y();
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
        return false;
    }
    if (orientation) {
        bool const landscape = width > height;
        if (*orientation == Orientation::Landscape && !landscape) {
            std::swap(width, height);
        } else if (*orientation == Orientation::Portrait && landscape) {
            std::swap(width, height);
        }
    }
    doc->getPageManager().resizePage(page, width, height);
    // Resizing the viewport page syncs its rect back from the root on the
    // next document update, and SPPage::setRect clears the size label. Flush
    // that update first so the label set below survives.
    doc->ensureUpToDate();
    page->setSizeLabel(std::move(preset_label));
    return true;
}

std::string list_pages(SPDocument *doc)
{
    if (!doc) {
        return {};
    }
    auto &pm = doc->getPageManager();
    std::ostringstream output;

    if (!pm.hasPages()) {
        auto const box = doc->getViewBox();
        output << "1\t-\t" << _("Page 1") << '\t' << format_user_unit(box.width()) << 'x'
               << format_user_unit(box.height()) << "\t-\n";
        return output.str();
    }

    for (int i = 0; i < pm.getPageCount(); ++i) {
        auto *page = pm.getPage(i);
        if (!page) {
            continue;
        }
        auto const rect = page->getRect();
        auto const id = page->getId();
        auto const preset = page->getSizeLabel();
        output << (i + 1) << '\t' << (id ? id : "-") << '\t' << page->getLabel() << '\t'
               << format_user_unit(rect.width()) << 'x' << format_user_unit(rect.height()) << '\t'
               << (preset.empty() ? "-" : preset) << '\n';
    }
    return output.str();
}

} // namespace Inkscape::PageOps

namespace {

using Inkscape::DocumentUndo;
using Inkscape::PageOps::Orientation;
using Inkscape::PageOps::Where;

SPDocument *active_document(InkscapeApplication *app, char const *action)
{
    auto doc = app->get_active_document();
    if (!doc) {
        g_warning("%s: no active document", action);
    }
    return doc;
}

std::string string_param(Glib::VariantBase const &value)
{
    return Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();
}

int int_param(Glib::VariantBase const &value)
{
    return Glib::VariantBase::cast_dynamic<Glib::Variant<int>>(value).get();
}

bool has_value(std::string const &value)
{
    return !value.empty();
}

void page_goto(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto doc = active_document(app, "page-goto");
    if (!doc) {
        return;
    }
    int const pos = int_param(value);
    if (!Inkscape::PageOps::goto_page(doc, pos)) {
        return;
    }
    g_message("page-goto: selected page %d", pos);
}

void page_step(InkscapeApplication *app, int delta, char const *action)
{
    auto doc = active_document(app, action);
    if (!doc) {
        return;
    }
    if (!Inkscape::PageOps::step_page(doc, delta)) {
        return;
    }
    auto &pm = doc->getPageManager();
    if (auto *desktop = app->get_active_desktop()) {
        pm.centerToSelectedPage(desktop);
    }
    g_message("%s: selected page %d", action, pm.getSelectedPageIndex() + 1);
}

void page_next(InkscapeApplication *app)
{
    page_step(app, 1, "page-next");
}

void page_previous(InkscapeApplication *app)
{
    page_step(app, -1, "page-previous");
}

void page_insert(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto doc = active_document(app, "page-insert");
    if (!doc) {
        return;
    }
    auto const spec = string_param(value);
    Where where = Where::After;
    char const *where_name = "after";
    if (has_value(spec)) {
        if (spec == "before") {
            where = Where::Before;
            where_name = "before";
        } else if (spec == "end") {
            where = Where::End;
            where_name = "end";
        } else if (spec != "after") {
            g_warning("page-insert: unknown position '%s' (expected before, after or end)", spec.c_str());
            return;
        }
    }
    auto *page = Inkscape::PageOps::insert_page(doc, where);
    if (!page) {
        g_warning("page-insert: could not create a page");
        return;
    }
    int const position = page->getPagePosition();
    int const count = doc->getPageManager().getPageCount();
    DocumentUndo::done(doc, RC_("Undo", "Insert Page"), INKSCAPE_ICON("tool-pages"));
    g_message("page-insert: inserted page %d of %d (%s)", position, count, where_name);
}

void page_duplicate_after(InkscapeApplication *app)
{
    auto doc = active_document(app, "page-duplicate-after");
    if (!doc) {
        return;
    }
    auto *page = Inkscape::PageOps::duplicate_after(doc);
    if (!page) {
        g_warning("page-duplicate-after: could not duplicate the page");
        return;
    }
    int const position = page->getPagePosition();
    int const count = doc->getPageManager().getPageCount();
    DocumentUndo::done(doc, RC_("Undo", "Duplicate Page"), INKSCAPE_ICON("tool-pages"));
    g_message("page-duplicate-after: duplicated page, copy is page %d of %d", position, count);
}

void page_remove(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto doc = active_document(app, "page-remove");
    if (!doc) {
        return;
    }
    auto const spec = string_param(value);
    bool contents = true;
    if (has_value(spec)) {
        if (spec == "keep") {
            contents = false;
        } else if (spec != "contents") {
            g_warning("page-remove: unknown mode '%s' (expected contents or keep)", spec.c_str());
            return;
        }
    }
    if (!Inkscape::PageOps::remove_page(doc, contents)) {
        return;
    }
    int const count = doc->getPageManager().getPageCount();
    DocumentUndo::done(doc, RC_("Undo", "Remove Page"), INKSCAPE_ICON("tool-pages"));
    g_message("page-remove: removed page (%s), %d page(s) left", contents ? "with contents" : "keeping objects",
              count);
}

void page_move_to(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto doc = active_document(app, "page-move-to");
    if (!doc) {
        return;
    }
    int const pos = int_param(value);
    auto *page = Inkscape::PageOps::current_page(doc);
    if (!page) {
        g_warning("page-move-to: no page selected");
        return;
    }
    if (!Inkscape::PageOps::move_to(doc, page, pos)) {
        g_message("page-move-to: page already at position %d", std::clamp(pos, 1, doc->getPageManager().getPageCount()));
        return;
    }
    int const position = page->getPagePosition();
    DocumentUndo::done(doc, RC_("Undo", "Move Page"), INKSCAPE_ICON("tool-pages"));
    g_message("page-move-to: moved page to position %d", position);
}

void page_rename(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto doc = active_document(app, "page-rename");
    if (!doc) {
        return;
    }
    auto const label = string_param(value);
    if (!Inkscape::PageOps::rename_page(doc, label)) {
        g_warning("page-rename: could not rename the page");
        return;
    }
    auto *page = Inkscape::PageOps::current_page(doc);
    int const position = page ? page->getPagePosition() : 1;
    DocumentUndo::done(doc, RC_("Undo", "Rename Page"), INKSCAPE_ICON("tool-pages"));
    g_message("page-rename: page %d is now '%s'", position, label.empty() ? "" : label.c_str());
}

void page_resize(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto doc = active_document(app, "page-resize");
    if (!doc) {
        return;
    }
    std::string spec = trim(string_param(value));
    if (spec.empty()) {
        g_warning("page-resize: missing size specification");
        return;
    }

    bool apply_all = take_suffix(spec, "all");
    std::optional<Orientation> orientation;
    if (auto const pos = spec.rfind(','); pos != std::string::npos) {
        auto const tail = trim(spec.substr(pos + 1));
        if (equal_ci(tail, "landscape")) {
            orientation = Orientation::Landscape;
            spec = trim(spec.substr(0, pos));
        } else if (equal_ci(tail, "portrait")) {
            orientation = Orientation::Portrait;
            spec = trim(spec.substr(0, pos));
        }
    }
    if (spec.empty()) {
        g_warning("page-resize: missing size specification");
        return;
    }

    auto const size = Inkscape::PageOps::parse_size(doc, spec);
    if (!size) {
        g_warning("page-resize: could not parse size '%s'", spec.c_str());
        return;
    }
    auto const preset = preset_name_for(spec);

    auto &pm = doc->getPageManager();
    pm.enablePages();

    bool changed = false;
    if (apply_all) {
        auto const pages = pm.getPages();
        for (auto *page : pages) {
            changed |= Inkscape::PageOps::resize_page(doc, page, *size, orientation, preset);
        }
    } else if (auto *page = Inkscape::PageOps::current_page(doc)) {
        changed = Inkscape::PageOps::resize_page(doc, page, *size, orientation, preset);
    }
    if (!changed) {
        g_warning("page-resize: no page was resized");
        return;
    }
    DocumentUndo::done(doc, RC_("Undo", "Resize Page"), INKSCAPE_ICON("tool-pages"));
    g_message("page-resize: resized %s page(s) to %s%s%s", apply_all ? "all" : "the current",
              format_user_unit(size->x()).c_str(), orientation ? (orientation == Orientation::Landscape ? " landscape" : " portrait") : "",
              apply_all ? " (all)" : "");
}

void page_orientation(Glib::VariantBase const &value, InkscapeApplication *app)
{
    auto doc = active_document(app, "page-orientation");
    if (!doc) {
        return;
    }
    auto const spec = string_param(value);
    std::optional<Orientation> orientation;
    if (spec == "portrait") {
        orientation = Orientation::Portrait;
    } else if (spec == "landscape") {
        orientation = Orientation::Landscape;
    } else {
        g_warning("page-orientation: unknown orientation '%s' (expected portrait or landscape)", spec.c_str());
        return;
    }

    auto &pm = doc->getPageManager();
    pm.enablePages();
    auto *page = Inkscape::PageOps::current_page(doc);
    if (!page) {
        g_warning("page-orientation: no page selected");
        return;
    }
    auto const rect = page->getDocumentRect();
    auto const size = Geom::Point(rect.width(), rect.height());
    auto const preset = page->getSizeLabel();
    if (!Inkscape::PageOps::resize_page(doc, page, size, orientation, preset)) {
        g_warning("page-orientation: could not resize the page");
        return;
    }
    int const position = page->getPagePosition();
    DocumentUndo::done(doc, RC_("Undo", "Change Page Orientation"), INKSCAPE_ICON("tool-pages"));
    g_message("page-orientation: page %d is now %s", position, spec.c_str());
}

void page_list(InkscapeApplication *app)
{
    auto doc = active_document(app, "page-list");
    if (!doc) {
        return;
    }
    std::cout << Inkscape::PageOps::list_pages(doc);
    std::cout.flush();
}

const Glib::ustring SECTION = NC_("Action Section", "Page");

std::vector<std::vector<Glib::ustring>> raw_data_page_manager = {
    // clang-format off
    {"app.page-goto",            N_("Go to Page"),           SECTION, N_("Select a page by position (1-based)")},
    {"app.page-next",            N_("Next Page"),             SECTION, N_("Select and show the next page")},
    {"app.page-previous",        N_("Previous Page"),         SECTION, N_("Select and show the previous page")},
    {"app.page-insert",          N_("Insert Page"),          SECTION, N_("Insert a page before, after or at the end")},
    {"app.page-duplicate-after", N_("Duplicate Page After"), SECTION, N_("Duplicate the current page and place the copy after it")},
    {"app.page-remove",          N_("Remove Page"),          SECTION, N_("Remove the current page, with or without its contents")},
    {"app.page-move-to",         N_("Move Page To"),         SECTION, N_("Move the current page to a position (1-based)")},
    {"app.page-rename",          N_("Rename Page"),          SECTION, N_("Set or clear the current page label")},
    {"app.page-resize",          N_("Resize Page"),          SECTION, N_("Resize a page by preset or dimensions, optionally landscape/portrait and all pages")},
    {"app.page-orientation",     N_("Set Page Orientation"), SECTION, N_("Keep the current page size and set portrait or landscape")},
    {"app.page-list",            N_("List Pages"),           SECTION, N_("Print one line per page: position, id, label, size and preset")},
    // clang-format on
};

} // namespace

void add_actions_page_manager(InkscapeApplication *app)
{
    Glib::VariantType Int(Glib::VARIANT_TYPE_INT32);
    Glib::VariantType String(Glib::VARIANT_TYPE_STRING);

    auto *gapp = app->gio_app();

    // clang-format off
    gapp->add_action_with_parameter("page-goto",            Int,    sigc::bind(sigc::ptr_fun(&page_goto),            app));
    gapp->add_action(                "page-next",                    sigc::bind(sigc::ptr_fun(&page_next),            app));
    gapp->add_action(                "page-previous",                sigc::bind(sigc::ptr_fun(&page_previous),        app));
    gapp->add_action_with_parameter("page-insert",          String, sigc::bind(sigc::ptr_fun(&page_insert),          app));
    gapp->add_action(                "page-duplicate-after",         sigc::bind(sigc::ptr_fun(&page_duplicate_after), app));
    gapp->add_action_with_parameter("page-remove",          String, sigc::bind(sigc::ptr_fun(&page_remove),          app));
    gapp->add_action_with_parameter("page-move-to",         Int,    sigc::bind(sigc::ptr_fun(&page_move_to),         app));
    gapp->add_action_with_parameter("page-rename",          String, sigc::bind(sigc::ptr_fun(&page_rename),          app));
    gapp->add_action_with_parameter("page-resize",          String, sigc::bind(sigc::ptr_fun(&page_resize),          app));
    gapp->add_action_with_parameter("page-orientation",     String, sigc::bind(sigc::ptr_fun(&page_orientation),     app));
    gapp->add_action(                "page-list",                    sigc::bind(sigc::ptr_fun(&page_list),            app));
    // clang-format on

    app->get_action_extra_data().add_data(raw_data_page_manager);
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