// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Provide a capypdf exporter for Inkscape
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "pdf-output.h"

#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

#include "build-document.h"
#include "build-page.h"
#include "colors/cms/profile.h"
#include "colors/document-cms.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "extension/db.h"
#include "extension/internal/latex-text-renderer.h"
#include "extension/output.h"
#include "extension/system.h"
#include "inkscape-version.h"
#include "object/color-profile.h"
#include "object/master-layer.h"
#include "object/sp-item.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "path-chemistry.h"
#include "pdfx.h"
#include "rdf.h"

namespace Inkscape::Extension::Internal {

namespace {

/**
 * capypdf::DocumentProperties does not wrap capy_document_properties_set_metadata_xml.
 * This method-only subclass reaches the C API the same way SeparationColor does
 * for separation colours; the wrapper static_asserts that the size is a pointer,
 * so adding a method cannot change the layout.
 */
class PdfXDocumentProperties : public capypdf::DocumentProperties
{
public:
    void set_metadata_xml(std::string const &xml)
    {
        if (auto const rc = capy_document_properties_set_metadata_xml(
                *this, xml.data(), static_cast<int32_t>(xml.size()))) {
            throw capypdf::PdfException(capy_error_message(rc));
        }
    }
};

/**
 * Pin SOURCE_DATE_EPOCH to a single value for the duration of the export when
 * the caller did not provide one, so capypdf's Info dates and our XMP dates
 * come from the same second. Restores the environment on every exit path.
 */
class SourceDateEpochGuard
{
public:
    explicit SourceDateEpochGuard(std::string const &epoch)
    {
        if (!g_getenv("SOURCE_DATE_EPOCH")) {
            g_setenv("SOURCE_DATE_EPOCH", epoch.c_str(), true);
            _owned = true;
        }
    }
    ~SourceDateEpochGuard()
    {
        if (_owned) {
            g_unsetenv("SOURCE_DATE_EPOCH");
        }
    }
    SourceDateEpochGuard(SourceDateEpochGuard const &) = delete;
    SourceDateEpochGuard &operator=(SourceDateEpochGuard const &) = delete;

private:
    bool _owned = false;
};

/** A profile that only exists as data is materialised for the lifetime of save(). */
class ProfileTempFile
{
public:
    ~ProfileTempFile()
    {
        if (!_path.empty()) {
            g_unlink(_path.c_str());
        }
    }

    bool write(std::vector<unsigned char> const &data)
    {
        gchar *name = nullptr;
        GError *error = nullptr;
        int const fd = g_file_open_tmp("rosepen-pdfx-XXXXXX.icc", &name, &error);
        if (fd < 0) {
            if (error) {
                g_error_free(error);
            }
            return false;
        }
        g_close(fd, nullptr);
        _path = name;
        g_free(name);
        return g_file_set_contents(_path.c_str(), reinterpret_cast<char const *>(data.data()),
                                   static_cast<gssize>(data.size()), nullptr);
    }

    std::string const &path() const { return _path; }

private:
    std::string _path;
};

gint64 source_date_epoch()
{
    if (char const *epoch = g_getenv("SOURCE_DATE_EPOCH")) {
        return g_ascii_strtoll(epoch, nullptr, 10);
    }
    return static_cast<gint64>(std::time(nullptr));
}

std::string iso_date(gint64 epoch)
{
    std::time_t const time = static_cast<std::time_t>(epoch);
    struct tm utc = {};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

/**
 * Find the document's <color-profile> for the output space. Its URI contents
 * are the original profile bytes; Profile::getPath() is empty because the
 * document CMS builds profiles with create_from_data(), and dumpData() would
 * re-serialize them through lcms and change the bytes.
 */
Inkscape::ColorProfile *document_profile(SPDocument *doc, std::string const &name)
{
    for (auto *object : doc->getResourceList("iccprofile")) {
        if (auto *profile = cast<Inkscape::ColorProfile>(object)) {
            if (profile->getName() == name) {
                return profile;
            }
        }
    }
    return nullptr;
}

/** PDF/X needs a title; fall back to the output filename stem. */
std::string output_title(SPDocument *doc, char const *filename)
{
    if (char const *title = rdf_get_work_entity(doc, rdf_find_entity("title"))) {
        if (*title) {
            return title;
        }
    }
    std::string name = filename ? filename : "";
    if (auto const slash = name.find_last_of('/'); slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    if (auto const dot = name.find_last_of('.'); dot != std::string::npos) {
        name.erase(dot);
    }
    return name.empty() ? "Untitled" : name;
}

/**
 * capypdf validates the PDF/X properties (output profile, intent identifier)
 * when the generator is created and throws on a violation. On the PDF/X path
 * that becomes a failed save instead of an exception escaping save().
 */
PdfBuilder::Document make_pdfx_document(char const *filename, capypdf::DocumentProperties &opt)
{
    try {
        return PdfBuilder::Document(filename, opt);
    } catch (std::exception const &ex) {
        g_warning("Couldn't start the PDF/X document: %s", ex.what());
        throw Inkscape::Extension::Output::save_failed();
    }
}

} // namespace

bool PdfOutput::check(Inkscape::Extension::Extension *)
{
    return Inkscape::Extension::db.get("org.inkscape.output.pdf.capypdf");
}

/**
 * Save the PDF file
 */
void PdfOutput::save(Inkscape::Extension::Output *mod, SPDocument *doc, char const *filename)
{
    auto const mode = PdfX::mode_from_param(mod->get_param_optiongroup("PDFversion", "PDF-2.0"));
    bool const pdfx = mode != PdfX::Mode::None;
    bool const crop_marks = pdfx && mod->get_param_bool("cropMarks", false);

    PdfXDocumentProperties opt;

    auto root = doc->getRoot();
    if (!root) {
        g_error("Couldn't save PDF, no document root");
        return;
    }

    if (char const *title = rdf_get_work_entity(doc, rdf_find_entity("title"))) {
        opt.set_title(title);
    }
    if (char const *author = rdf_get_work_entity(doc, rdf_find_entity("creator"))) {
        opt.set_author(author);
    }
    // Nothing to set yet.
    // if (char const *subject = rdf_get_work_entity(doc, rdf_find_entity("description"))) {}
    // Nothing to set yet.
    // if (char const *keywords = rdf_get_work_entity(doc, rdf_find_entity("subject"))) {}
    // Nothing to set yet.
    // if (char const *copyright = rdf_get_work_entity(doc, rdf_find_entity("rights"))) {}

    auto creator =
        Glib::ustring::compose("Inkscape %1 (https://inkscape.org)", Inkscape::version_string_without_revision);
    opt.set_creator(creator.c_str());

    // TODO: This API currently doesn't work well for us
    // opt.set_colorspace(CAPY_DEVICE_CS_CMYK);
    // opt.set_device_profile(DEVICE_COLORSPACE, icc_profile);

    std::shared_ptr<Colors::Space::CMS> output_space;
    ProfileTempFile temp_profile;
    std::unique_ptr<SourceDateEpochGuard> date_guard;
    if (pdfx) {
        // Preflight runs before the capypdf document exists: a refused export
        // must not leave a partial target behind.
        auto const findings = PdfX::preflight(doc, mode, mod->get_param_bool("blurToBitmap"));
        bool refused = false;
        for (auto const &finding : findings) {
            if (finding.error) {
                refused = true;
                g_warning("PDF/X preflight: %s: %s", finding.id.c_str(), finding.reason.c_str());
            } else {
                g_warning("PDF/X preflight warning: %s: %s", finding.id.c_str(),
                          finding.reason.c_str());
            }
        }
        // Nothing has been written yet. capypdf writes through a temporary
        // file and a rename, so a file already at the target is the user's
        // previous export, not a partial one: leave it alone.
        if (refused) {
            throw Inkscape::Extension::Output::save_failed();
        }

        output_space = doc->getDocumentCMS().getOutputSpace();
        if (!output_space) {
            throw Inkscape::Extension::Output::save_failed();
        }
        auto const profile = output_space->getProfile();
        if (!profile) {
            throw Inkscape::Extension::Output::save_failed();
        }

        std::string profile_path = profile->getPath();
        if (profile_path.empty()) {
            std::vector<unsigned char> bytes;
            if (auto *color_profile = document_profile(doc, output_space->getName())) {
                auto const data = color_profile->getProfileData();
                bytes.assign(data.begin(), data.end());
            }
            if (bytes.empty()) {
                bytes = profile->dumpData();
            }
            if (!temp_profile.write(bytes)) {
                g_warning("PDF/X preflight: document: could not stage the output profile");
                throw Inkscape::Extension::Output::save_failed();
            }
            profile_path = temp_profile.path();
        }

        // capypdf reports bad properties as exceptions; a refused PDF/X setup
        // is a failed save, never an exception escaping into the caller.
        try {
            auto const identifier = profile->getName();
            opt.set_colorspace(CAPY_DEVICE_CS_CMYK);
            opt.set_device_profile(CAPY_DEVICE_CS_CMYK, profile_path.c_str());
            opt.set_output_intent(identifier.c_str());
            opt.set_pdfx(mode == PdfX::Mode::X1a ? CAPY_PDFX_1A_2003 : CAPY_PDFX_4);

            auto const title = output_title(doc, filename);
            opt.set_title(title.c_str());

            auto const epoch = source_date_epoch();
            date_guard = std::make_unique<SourceDateEpochGuard>(std::to_string(epoch));
            if (mode == PdfX::Mode::X4) {
                gchar *uuid = g_uuid_string_random();
                std::string doc_uuid = uuid ? std::string("uuid:") + uuid : std::string();
                g_free(uuid);
                opt.set_metadata_xml(PdfX::xmp_packet(mode, title, iso_date(epoch), doc_uuid));
            }
        } catch (std::exception const &ex) {
            g_warning("Couldn't set up the PDF/X document: %s", ex.what());
            throw Inkscape::Extension::Output::save_failed();
        }
    }

    auto pdf = pdfx ? make_pdfx_document(filename, opt) : PdfBuilder::Document(filename, opt);
    if (pdfx) {
        pdf.set_pdfx_mode(mode, output_space);
    }
    if (mod->get_param_bool("blurToBitmap")) {
        pdf.set_filter_resolution(mod->get_param_int("resolution"));
    }
    if (mod->get_param_optiongroup_is("textToPath", "paths")) {
        Inkscape::convert_text_to_curves(doc);
    } else if (mod->get_param_optiongroup_is("textToPath", "LaTeX")) {
        pdf.set_text_enabled(false);
        if (!latex_render_document_text_to_file(doc, filename, true)) {
            throw Inkscape::Extension::Output::save_failed();
        }
    }

    if (pdfx) {
        // Master layers are repeated on every page, exactly as on the default
        // path (03-02 handoff): keep paint_root_children + master_offsets.
        bool const paint_masters =
            !MasterLayer::master_layers(doc).empty() && doc->getPageManager().getPageCount() >= 2;

        std::map<SPPage *, Geom::Point> master_offsets;
        if (paint_masters) {
            for (auto const &[master_page, offset] : MasterLayer::page_offsets(doc)) {
                master_offsets.emplace(master_page, offset);
            }
        }

        // capypdf reports PDF/X violations it detects itself (and any other
        // failure) as exceptions; turn them into a failed save, not a crash.
        try {
            std::optional<CapyPDF_TransparencyGroupId> drawing_id;
            if (!paint_masters) {
                auto group_ctx = PdfBuilder::ItemContext(pdf, root);
                drawing_id = pdf.add_group(group_ctx);
            }

            auto &pm = doc->getPageManager();
            pm.enablePages();

            uint32_t page = 0;
            for (auto const &svg_page : pm.getPages()) {
                auto const boxes = PdfX::page_boxes(svg_page->getDocumentRect(),
                                                    svg_page->getDocumentBleed(), crop_marks);
                auto pdf_page = PdfBuilder::PageContext(pdf, svg_page, boxes.media);
                pdf_page.set_pagebox(CAPY_BOX_BLEED, boxes.bleed);
                pdf_page.set_pagebox(CAPY_BOX_TRIM, boxes.trim);

                if (auto label = svg_page->label()) {
                    pdf.set_label(page, label);
                }

                // Keep content (including masters) out of the slug/mark area.
                pdf_page.begin_content_clip(boxes.bleed);
                if (paint_masters) {
                    auto const found = master_offsets.find(svg_page);
                    auto const offset = found == master_offsets.end() ? Geom::Point(0, 0) : found->second;
                    pdf_page.paint_root_children(root, [&](SPItem const *item) {
                        return MasterLayer::is_master(item) ? Geom::Affine(Geom::Translate(offset))
                                                            : Geom::Affine(Geom::identity());
                    });
                } else if (drawing_id) {
                    pdf_page.paint_drawing(*drawing_id, doc->getRoot()->c2p);
                }
                pdf_page.end_content_clip();

                if (crop_marks) {
                    auto const transform = PdfBuilder::PageContext::page_transform(boxes.media);
                    std::vector<std::pair<Geom::Point, Geom::Point>> segments;
                    for (auto const &[from, to] : PdfX::crop_mark_segments(boxes.trim, boxes.bleed)) {
                        segments.emplace_back(from * transform, to * transform);
                    }
                    pdf_page.draw_crop_marks(segments);
                }

                // Link annotations are outside the PDF/X subset used here.
                pdf.add_page(pdf_page);
                page++;
            }
            pdf.write();
        } catch (std::exception &ex) {
            g_warning("Couldn't save pdf file: %s", ex.what());
            throw Inkscape::Extension::Output::save_failed();
        }
        return;
    }

    // Master layers are repeated on every page. With two or more pages and at
    // least one master layer, each page paints the root's top-level children
    // one by one so the master layers can be shifted by their page offset.
    // Two or more pages already exist as page objects, so this needs no enablePages().
    bool const paint_masters =
        !MasterLayer::master_layers(doc).empty() && doc->getPageManager().getPageCount() >= 2;

    std::map<SPPage *, Geom::Point> master_offsets;
    if (paint_masters) {
        for (auto const &[master_page, offset] : MasterLayer::page_offsets(doc)) {
            master_offsets.emplace(master_page, offset);
        }
    }

    // Step 1. Render EVERYTHING in the document out to a single PDF TransparencyGroup
    // This allows the page "positions" to be stored by the offset of the group.
    // On the master path the per-item groups are created while painting each page.
    std::optional<CapyPDF_TransparencyGroupId> drawing_id;
    if (!paint_masters) {
        auto group_ctx = PdfBuilder::ItemContext(pdf, root);
        drawing_id = pdf.add_group(group_ctx);
    }

    // FUTURE: If in the future we want PDF files where the items on a page are rendered only
    // in the group for that page, then we have two mechanisms for page separation.
    //   a. Find out if two pages share the same objects, and if they do, use the same root-group
    //   b. Find out if an object is shared by two pages and force it to use a transparency group
    //      at the most useful level. i.e. all children are shared thus share the parent group.

    // Step 2. Enable pages for this document. It SHOULD be a copy by this stage
    auto &pm = doc->getPageManager();
    pm.enablePages();

    uint32_t page = 0;
    // Step 3. Tell the PDF where to draw that whole plate on the PDF pages
    for (auto const &svg_page : pm.getPages()) {
        auto pdf_page = PdfBuilder::PageContext(pdf, svg_page);

        if (!svg_page->isBarePage()) {
            pdf_page.set_pagebox(CAPY_BOX_BLEED, svg_page->getDocumentRect());
            pdf_page.set_pagebox(CAPY_BOX_TRIM, svg_page->getDocumentRect());
            pdf_page.set_pagebox(CAPY_BOX_ART, svg_page->getDocumentMargin());
        }

        if (auto label = svg_page->label()) {
            pdf.set_label(page, label);
        }

        if (paint_masters) {
            auto const found = master_offsets.find(svg_page);
            auto const offset = found == master_offsets.end() ? Geom::Point(0, 0) : found->second;
            pdf_page.paint_root_children(root, [&](SPItem const *item) {
                return MasterLayer::is_master(item) ? Geom::Affine(Geom::Translate(offset))
                                                    : Geom::Affine(Geom::identity());
            });
        } else if (drawing_id) {
            pdf_page.paint_drawing(*drawing_id, doc->getRoot()->c2p);
        }
        // Page links / anchors / annotations are added in post processing.
        pdf_page.add_anchors_for_page(svg_page);
        pdf.add_page(pdf_page);
        page++;
    }
    try {
        pdf.write();
    } catch (std::exception &ex) {
        g_warning("Couldn't save pdf file: %s", ex.what());
    }
}

#include "../clear-n_.h"

void PdfOutput::init()
{
    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>Portable Document Format</name>\n"
            "<id>org.inkscape.output.pdf.capypdf</id>\n"
            "<param name=\"PDFversion\" gui-text=\"" N_("Restrict to PDF version:") "\" type=\"optiongroup\" appearance=\"combo\" >\n"
                "<option value='PDF-2.0'>" N_("PDF 2.0") "</option>\n"
                "<option value='PDF-X-1a'>" N_("PDF/X-1a:2003") "</option>\n"
                "<option value='PDF-X-4'>" N_("PDF/X-4") "</option>\n"
            "</param>\n"
            "<param name=\"cropMarks\" gui-text=\"" N_("Crop marks (PDF/X only)") "\" type=\"bool\">false</param>\n"
            "<param name=\"textToPath\" gui-text=\"" N_("Text output options:") "\" type=\"optiongroup\" appearance=\"radio\">\n"
                "<option value=\"embed\">" N_("Embed fonts") "</option>\n"
                "<option value=\"paths\">" N_("Convert text to paths") "</option>\n"
                "<option value=\"LaTeX\">" N_("Omit text in PDF and create LaTeX file") "</option>\n"
            "</param>\n"
            "<param name=\"blurToBitmap\" gui-text=\"" N_("Rasterize filter effects") "\" type=\"bool\">true</param>\n"
            "<param name=\"resolution\" gui-text=\"" N_("Resolution for rasterization (dpi):") "\" type=\"int\" min=\"1\" max=\"10000\">96</param>\n"
            "<output is_exported='true' priority='4'>\n"
                "<extension>.pdf</extension>\n"
                "<mimetype>application/pdf</mimetype>\n"
                "<filetypename>PDF (*.pdf)</filetypename>\n"
                "<filetypetooltip>PDF File</filetypetooltip>\n"
            "</output>\n"
        "</inkscape-extension>", std::make_unique<PdfOutput>());
    // clang-format on
}

} // namespace Inkscape::Extension::Internal
