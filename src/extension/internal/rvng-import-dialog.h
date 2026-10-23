// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Common import dialog for .cdr and .vss files.
 */
#ifndef INKSCAPE_EXTENSION_INTERNAL_RVNGIMPORTDIALOG_H
#define INKSCAPE_EXTENSION_INTERNAL_RVNGIMPORTDIALOG_H

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <librevenge/librevenge.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>

#include "document.h"
#include "ui/view/svg-view-widget.h"

namespace Inkscape::Extension::Internal {

inline std::span<char const> as_span(librevenge::RVNGString const &str)
{
    return {str.cstr(), str.size()};
}

class RvngImportDialog : public Gtk::Dialog
{
public:
    RvngImportDialog(std::vector<librevenge::RVNGString> const &pages);

    bool showDialog();
    int getSelectedPage() const { return _current_page; }

private:
    void _setPreviewPage();

    // Signal handlers
    void _onPageNumberChanged();
    void _onSpinButtonClickPressed(int n_press, double x, double y);
    void _onSpinButtonClickReleased(int n_press, double x, double y);

    Gtk::Box *vbox1;
    Gtk::Button *cancelbutton;
    Gtk::Button *okbutton;

    Gtk::Box *_page_selector_box;
    Gtk::Label *_labelSelect;
    Gtk::Label *_labelTotalPages;
    Gtk::SpinButton *_pageNumberSpin;

    std::vector<librevenge::RVNGString> const &_pages; // Document to be imported
    int _current_page = 1; // Current selected page
    bool _spinning = false; // Whether SpinButton is pressed (i.e. we're "spinning")

    std::unique_ptr<SPDocument> _doc;
    Inkscape::UI::View::SVGViewWidget _preview;
};

struct RvngOpenOptions
{
    bool all_pages_headless = false; ///< Build one multipage document when there is no GUI
};

/**
 * Read every page of a drawing as a complete SVG document string.
 *
 * @returns The page strings, or nothing when the file is unsupported or cannot be parsed.
 */
std::optional<std::vector<std::string>> rvng_parse_pages(
    char const *uri,
    bool (*is_supported)(librevenge::RVNGInputStream *),
    bool (*parse)(librevenge::RVNGInputStream *, librevenge::RVNGDrawingInterface *));

std::unique_ptr<SPDocument> rvng_open(
    char const *uri,
    bool (*is_supported)(librevenge::RVNGInputStream *),
    bool (*parse)(librevenge::RVNGInputStream *, librevenge::RVNGDrawingInterface *),
    RvngOpenOptions const &options = {});

} // namespace Inkscape::Extension::Internal

#endif // INKSCAPE_EXTENSION_INTERNAL_RVNGIMPORTDIALOG_H
