// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the pure PDF/X helpers: mode selection, page boxes, crop-mark
 * geometry, the XMP packet, and the document-level and per-item preflight
 * (including the X-1a transparency refusal by item id).
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

#include <2geom/point.h>
#include <2geom/rect.h>

#include <src/colors/document-cms.h>
#include <src/colors/spaces/cms.h>
#include <src/document.h>
#include <src/extension/internal/pdfoutput/pdfx.h>
#include <src/inkscape.h>
#include <src/object/color-profile.h>
#include <src/object/sp-item.h>
#include <src/object/sp-object.h>
#include <src/object/sp-root.h>

using namespace Inkscape::Extension::Internal::PdfX;

namespace {

constexpr double px_per_mm = 96.0 / 25.4;

bool tags_balanced(std::string const &xml)
{
    std::vector<std::string> stack;
    for (std::size_t i = 0; i < xml.size();) {
        if (xml[i] != '<') {
            ++i;
            continue;
        }
        auto const end = xml.find('>', i);
        if (end == std::string::npos) {
            return false;
        }
        auto const tag = xml.substr(i + 1, end - i - 1);
        i = end + 1;
        if (tag.empty() || tag.front() == '?' || tag.front() == '!') {
            continue;
        }
        if (tag.front() == '/') {
            auto const name = tag.substr(1);
            if (stack.empty() || stack.back() != name) {
                return false;
            }
            stack.pop_back();
            continue;
        }
        if (tag.back() == '/') {
            continue;
        }
        auto const name_end = tag.find_first_of(" \t\r\n");
        stack.push_back(tag.substr(0, name_end));
    }
    return stack.empty();
}

std::string profile_doc(char const *profile_name, char const *href)
{
    return std::string(R"A(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="80mm" viewBox="0 0 100 80">
  <sodipodi:namedview id="nv" inkscape:output-profile=")A") +
           profile_name + R"A(" />
  <defs>
    <color-profile id="profile1" name=")A" +
           profile_name + R"A(" xlink:href=")A" + href + R"A(" />
    <view id="p1" viewBox="0 0 100 80" inkscape:bleed="3" />
  </defs>
</svg>)A";
}

/**
 * Hide every leaf item except `keep`, so one transparency construct can be
 * preflighted on its own. Containers stay visible (they carry no rule).
 */
void hide_other_items(SPObject *object, std::string const &keep)
{
    for (auto &child : object->children) {
        auto *item = cast<SPItem>(&child);
        if (!item) {
            continue;
        }
        hide_other_items(item, keep);
        bool has_item_child = false;
        for (auto const &grandchild : item->children) {
            if (cast<SPItem>(&grandchild)) {
                has_item_child = true;
                break;
            }
        }
        auto const *id = item->getId();
        bool const is_kept = id && keep == id;
        if (!has_item_child && !is_kept) {
            item->setHidden(true);
        }
    }
}

} // namespace

class PdfXTest : public ::testing::Test
{
public:
    static void SetUpTestCase() { Inkscape::Application::create(false); }

    std::unique_ptr<SPDocument> document(std::string const &source)
    {
        auto doc = SPDocument::createNewDocFromMem(source);
        EXPECT_TRUE(doc);
        if (doc) {
            doc->ensureUpToDate();
        }
        return doc;
    }

    std::unique_ptr<SPDocument> transparency_document()
    {
        auto doc = std::unique_ptr<SPDocument>(
            SPDocument::createNewDoc(INKSCAPE_TESTS_DIR "/cli_tests/testcases/pdfx/pdfx-transparency.svg"));
        EXPECT_TRUE(doc);
        if (doc) {
            doc->ensureUpToDate();
        }
        return doc;
    }
};

TEST_F(PdfXTest, modeFromParam)
{
    EXPECT_EQ(mode_from_param("PDF-X-1a"), Mode::X1a);
    EXPECT_EQ(mode_from_param("PDF-X-4"), Mode::X4);
    EXPECT_EQ(mode_from_param("PDF-2.0"), Mode::None);
    EXPECT_EQ(mode_from_param("PDF-1.4"), Mode::None);
    EXPECT_EQ(mode_from_param(""), Mode::None);
    EXPECT_EQ(mode_from_param("garbage"), Mode::None);
}

TEST_F(PdfXTest, pageBoxesSymmetricBleed)
{
    Geom::Rect const trim(0, 0, 378, 302);
    double const bleed_side = 3 * px_per_mm;
    Geom::Rect const bleed(Geom::Point(-bleed_side, -bleed_side),
                           Geom::Point(378 + bleed_side, 302 + bleed_side));

    auto const no_marks = page_boxes(trim, bleed, false);
    EXPECT_EQ(no_marks.media, bleed);
    EXPECT_EQ(no_marks.bleed, bleed);
    EXPECT_EQ(no_marks.trim, trim);

    auto const marks = page_boxes(trim, bleed, true);
    double const extent = 8 * px_per_mm;
    EXPECT_NEAR(marks.media.left(), -extent, 1e-6);
    EXPECT_NEAR(marks.media.top(), -extent, 1e-6);
    EXPECT_NEAR(marks.media.right(), 378 + extent, 1e-6);
    EXPECT_NEAR(marks.media.bottom(), 302 + extent, 1e-6);
    EXPECT_EQ(marks.bleed, bleed);
    EXPECT_EQ(marks.trim, trim);
}

TEST_F(PdfXTest, pageBoxesAsymmetricBleed)
{
    Geom::Rect const trim(0, 0, 378, 302);
    double const top = 1 * px_per_mm;
    double const right = 2 * px_per_mm;
    double const bottom = 4 * px_per_mm;
    double const left = 6 * px_per_mm;
    Geom::Rect const bleed(Geom::Point(-left, -top), Geom::Point(378 + right, 302 + bottom));

    auto const boxes = page_boxes(trim, bleed, true);
    EXPECT_NEAR(boxes.media.left(), -11 * px_per_mm, 1e-6);
    EXPECT_NEAR(boxes.media.top(), -8 * px_per_mm, 1e-6);
    EXPECT_NEAR(boxes.media.right(), 378 + 8 * px_per_mm, 1e-6);
    EXPECT_NEAR(boxes.media.bottom(), 302 + 9 * px_per_mm, 1e-6);
}

TEST_F(PdfXTest, pageBoxesZeroBleed)
{
    Geom::Rect const trim(10, 20, 110, 100);
    auto const boxes = page_boxes(trim, trim, false);
    EXPECT_EQ(boxes.bleed, trim);
    EXPECT_EQ(boxes.media, trim);
}

TEST_F(PdfXTest, cropMarkSegments)
{
    Geom::Rect const trim(0, 0, 378, 302);
    double const bleed_side = 3 * px_per_mm;
    Geom::Rect const bleed(Geom::Point(-bleed_side, -bleed_side),
                           Geom::Point(378 + bleed_side, 302 + bleed_side));

    auto const segments = crop_mark_segments(trim, bleed);
    ASSERT_EQ(segments.size(), 8u);

    double const offset = 3 * px_per_mm;
    double const length = 5 * px_per_mm;
    double const eps = 1e-6;

    for (auto const &[from, to] : segments) {
        bool const horizontal = std::abs(from.y() - to.y()) < eps;
        bool const vertical = std::abs(from.x() - to.x()) < eps;
        ASSERT_TRUE(horizontal || vertical) << "segment is neither horizontal nor vertical";

        if (horizontal) {
            EXPECT_NEAR(std::abs(from.x() - to.x()), length, eps);
            bool const at_top = std::abs(from.y() - trim.top()) < eps;
            bool const at_bottom = std::abs(from.y() - trim.bottom()) < eps;
            ASSERT_TRUE(at_top || at_bottom) << "horizontal mark is not collinear with a trim edge";
            double const low = std::min(from.x(), to.x());
            double const high = std::max(from.x(), to.x());
            EXPECT_TRUE(high <= bleed.left() + eps || low >= bleed.right() - eps)
                << "horizontal mark intersects the bleed rect";
            if (high <= bleed.left() + eps) {
                EXPECT_NEAR(high, trim.left() - offset, eps);
            } else {
                EXPECT_NEAR(low, trim.right() + offset, eps);
            }
        } else {
            EXPECT_NEAR(std::abs(from.y() - to.y()), length, eps);
            bool const at_left = std::abs(from.x() - trim.left()) < eps;
            bool const at_right = std::abs(from.x() - trim.right()) < eps;
            ASSERT_TRUE(at_left || at_right) << "vertical mark is not collinear with a trim edge";
            double const low = std::min(from.y(), to.y());
            double const high = std::max(from.y(), to.y());
            EXPECT_TRUE(high <= bleed.top() + eps || low >= bleed.bottom() - eps)
                << "vertical mark intersects the bleed rect";
            if (high <= bleed.top() + eps) {
                EXPECT_NEAR(high, trim.top() - offset, eps);
            } else {
                EXPECT_NEAR(low, trim.bottom() + offset, eps);
            }
        }
    }
}

TEST_F(PdfXTest, xmpPacket)
{
    std::string const date = "2023-11-14T22:13:20Z";
    auto const xmp = xmp_packet(Mode::X4, "A <&> \"B\"", date, "uuid:abc");
    EXPECT_FALSE(xmp.empty());
    EXPECT_NE(xmp.find("pdfxid:GTS_PDFXVersion>PDF/X-4<"), std::string::npos);
    EXPECT_NE(xmp.find("xmpMM:DocumentID>uuid:abc<"), std::string::npos);
    EXPECT_NE(xmp.find("xmpMM:VersionID>1<"), std::string::npos);
    EXPECT_NE(xmp.find("xmpMM:RenditionClass>default<"), std::string::npos);
    EXPECT_NE(xmp.find("pdf:Trapped>False<"), std::string::npos);
    // Same value as capypdf's Info /Producer (Info/XMP consistency).
    // "CapyPDF <version>": the bare name would disagree with Info /Producer.
    auto const producer = xmp.find("pdf:Producer>CapyPDF ");
    ASSERT_NE(producer, std::string::npos);
    EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(xmp[producer + 21])));
    EXPECT_NE(xmp.find("xmp:CreateDate>" + date + "<"), std::string::npos);
    EXPECT_NE(xmp.find("xmp:ModifyDate>" + date + "<"), std::string::npos);
    EXPECT_NE(xmp.find("A &lt;&amp;&gt; &quot;B&quot;"), std::string::npos);
    EXPECT_TRUE(tags_balanced(xmp)) << xmp;

    EXPECT_TRUE(xmp_packet(Mode::X1a, "title", date, "uuid:abc").empty());
}

TEST_F(PdfXTest, preflightMissingProfile)
{
    auto doc = document(R"A(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     width="100mm" height="80mm" viewBox="0 0 100 80">
  <sodipodi:namedview id="nv" />
  <defs><view id="p1" viewBox="0 0 100 80" inkscape:bleed="3" /></defs>
</svg>)A");
    ASSERT_TRUE(doc);

    auto const findings = preflight(doc.get(), Mode::X4, false);
    int errors = 0;
    for (auto const &finding : findings) {
        if (finding.error) {
            ++errors;
            EXPECT_EQ(finding.id, "document");
        }
    }
    EXPECT_EQ(errors, 1);
}

TEST_F(PdfXTest, preflightRgbProfileIsAnError)
{
    auto doc = document(profile_doc("rgb-prof", INKSCAPE_TESTS_DIR "/data/colors/display.icc"));
    ASSERT_TRUE(doc);

    auto const findings = preflight(doc.get(), Mode::X4, false);
    int errors = 0;
    for (auto const &finding : findings) {
        if (finding.error) {
            ++errors;
            EXPECT_EQ(finding.id, "document");
        }
    }
    EXPECT_EQ(errors, 1);
}

TEST_F(PdfXTest, preflightCmykProfileIsAccepted)
{
    auto doc = document(profile_doc("cmyk-out", INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc"));
    ASSERT_TRUE(doc);

    auto const findings = preflight(doc.get(), Mode::X4, false);
    for (auto const &finding : findings) {
        EXPECT_FALSE(finding.error) << finding.id << ": " << finding.reason;
    }
}

// An output profile can be embedded as a data URI. Such profiles are built
// from memory, so Profile::getPath() is empty and the exporter stages
// getProfileData() — the original bytes — into a temp file that capypdf
// embeds into the OutputIntent. The staged bytes must match the source data.
TEST_F(PdfXTest, dataUriProfileStagesOriginalBytes)
{
    gchar *icc_data = nullptr;
    gsize icc_len = 0;
    ASSERT_TRUE(g_file_get_contents(INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc", &icc_data,
                                    &icc_len, nullptr));
    ASSERT_NE(icc_data, nullptr);
    ASSERT_GT(icc_len, 0u);
    std::string const source(icc_data, icc_len);
    g_free(icc_data);

    auto *source_sum = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(source.data()), source.size());

    gchar *b64 = g_base64_encode(reinterpret_cast<guchar const *>(source.data()), source.size());
    std::string const href = std::string("data:application/vnd.iccprofile;base64,") + b64;
    g_free(b64);

    auto doc = document(profile_doc("cmyk-out", href.c_str()));
    ASSERT_TRUE(doc);

    auto space = doc->getDocumentCMS().getOutputSpace();
    ASSERT_TRUE(space);
    auto const profile = space->getProfile();
    ASSERT_TRUE(profile);
    ASSERT_TRUE(profile->getPath().empty()) << "a data-URI profile must have no file path";

    Inkscape::ColorProfile *color_profile = nullptr;
    for (auto *object : doc->getResourceList("iccprofile")) {
        if (auto *candidate = cast<Inkscape::ColorProfile>(object)) {
            if (candidate->getName() == "cmyk-out") {
                color_profile = candidate;
            }
        }
    }
    ASSERT_NE(color_profile, nullptr);

    auto const staged = color_profile->getProfileData();
    ASSERT_EQ(staged.size(), source.size());
    auto *staged_sum = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(staged.data()), staged.size());
    EXPECT_STREQ(staged_sum, source_sum) << "staged profile bytes differ from the source data";

    // Materialize the staged bytes exactly like ProfileTempFile does: what
    // capypdf embeds is the temp file's content.
    gchar *name = nullptr;
    GError *error = nullptr;
    int const fd = g_file_open_tmp("rosepen-pdfx-test-XXXXXX.icc", &name, &error);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    ASSERT_TRUE(g_file_set_contents(name, staged.data(), static_cast<gssize>(staged.size()),
                                    nullptr));
    gchar *disk_data = nullptr;
    gsize disk_len = 0;
    ASSERT_TRUE(g_file_get_contents(name, &disk_data, &disk_len, nullptr));
    ASSERT_NE(disk_data, nullptr);
    ASSERT_EQ(disk_len, source.size());
    auto *disk_sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                 reinterpret_cast<guchar const *>(disk_data),
                                                 disk_len);
    EXPECT_STREQ(disk_sum, source_sum) << "temp-file profile bytes differ from the source data";

    g_free(disk_sum);
    g_free(disk_data);
    g_unlink(name);
    g_free(name);
    g_free(staged_sum);
    g_free(source_sum);

    // The data-URI profile satisfies the document-level preflight.
    for (auto const &finding : preflight(doc.get(), Mode::X4, false)) {
        EXPECT_FALSE(finding.error) << finding.id << ": " << finding.reason;
    }
}

TEST_F(PdfXTest, preflightBleedOverlapIsAWarning)
{
    auto const source = std::string(R"A(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="80mm" viewBox="0 0 100 80">
  <sodipodi:namedview id="nv" inkscape:output-profile="cmyk-out" />
  <defs>
    <color-profile id="profile1" name="cmyk-out" xlink:href=")A") +
                             INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc" + R"A(" />
    <view id="p1" viewBox="0 0 100 80" inkscape:bleed="3" />
    <view id="p2" viewBox="102 0 100 80" inkscape:bleed="3" />
  </defs>
</svg>)A";
    auto doc = document(source);
    ASSERT_TRUE(doc);

    auto const findings = preflight(doc.get(), Mode::X4, false);
    int warnings = 0;
    for (auto const &finding : findings) {
        EXPECT_FALSE(finding.error) << finding.id << ": " << finding.reason;
        if (!finding.error) {
            ++warnings;
        }
    }
    EXPECT_EQ(warnings, 1);
}

TEST_F(PdfXTest, preflightX1aTransparencyItems)
{
    struct Case
    {
        char const *id;
        bool reported;
    };
    Case const cases[] = {
        {"op-half", true},  {"fill-op", true},  {"masked", true},    {"blend", true},
        {"grad-alpha", true}, {"img-alpha", true}, {"blurred", true}, {"opaque", false},
    };

    for (auto const &test_case : cases) {
        auto doc = transparency_document();
        ASSERT_TRUE(doc) << test_case.id;
        hide_other_items(doc->getRoot(), test_case.id);

        auto const findings = preflight(doc.get(), Mode::X1a, true);
        std::vector<std::string> errors;
        for (auto const &finding : findings) {
            if (finding.error) {
                errors.push_back(finding.id);
            }
        }

        if (test_case.reported) {
            ASSERT_EQ(errors.size(), 1u) << test_case.id;
            EXPECT_EQ(errors[0], test_case.id);
        } else if (!errors.empty()) {
            ADD_FAILURE() << test_case.id << " -> " << errors[0];
        }
    }
}

// Pattern tiles and markers live in defs but are painted by the exporter, so
// X-1a preflight must inspect them through the item that uses them.
TEST_F(PdfXTest, preflightX1aPatternAndMarkerContent)
{
    auto doc = document(R"A(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="80mm" viewBox="0 0 100 80">
  <sodipodi:namedview id="nv" inkscape:output-profile="cmyk-out" />
  <defs>
    <color-profile id="profile1" name="cmyk-out" xlink:href=")A" INKSCAPE_TESTS_DIR R"A(/data/colors/default_cmyk.icc" />
    <pattern id="pat-base" width="10" height="10" patternUnits="userSpaceOnUse">
      <rect id="tile-op" width="5" height="5" style="fill:#ff0000;opacity:0.5" />
    </pattern>
    <pattern id="pat" xlink:href="#pat-base" />
    <marker id="mk" markerWidth="4" markerHeight="4">
      <circle id="mark-op" r="2" style="fill:#0000ff;fill-opacity:0.5" />
    </marker>
    <pattern id="pat-unused" width="10" height="10" patternUnits="userSpaceOnUse">
      <rect id="unused-op" width="5" height="5" style="opacity:0.5" />
    </pattern>
  </defs>
  <rect id="patterned" x="10" y="10" width="30" height="30" style="fill:url(#pat)" />
  <rect id="patterned2" x="50" y="10" width="30" height="30" style="fill:url(#pat)" />
  <path id="marked" d="M 10,60 L 60,60" style="fill:none;stroke:#000000;marker-end:url(#mk)" />
</svg>)A");
    ASSERT_TRUE(doc);

    std::vector<std::string> errors;
    for (auto const &finding : preflight(doc.get(), Mode::X1a, false)) {
        if (finding.error) {
            errors.push_back(finding.id);
        }
    }
    EXPECT_EQ(errors, (std::vector<std::string>{"tile-op", "mark-op"}));

    for (auto const &finding : preflight(doc.get(), Mode::X4, false)) {
        EXPECT_FALSE(finding.error) << finding.id << ": " << finding.reason;
    }
}

// The exporter paints the root as one group, so <svg> opacity is transparency.
TEST_F(PdfXTest, preflightX1aRootOpacity)
{
    auto doc = document(R"A(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     xmlns:xlink="http://www.w3.org/1999/xlink"
     id="root" style="opacity:0.5"
     width="100mm" height="80mm" viewBox="0 0 100 80">
  <sodipodi:namedview id="nv" inkscape:output-profile="cmyk-out" />
  <defs>
    <color-profile id="profile1" name="cmyk-out" xlink:href=")A" INKSCAPE_TESTS_DIR R"A(/data/colors/default_cmyk.icc" />
  </defs>
  <rect id="plain" x="10" y="10" width="30" height="30" style="fill:#ff0000" />
</svg>)A");
    ASSERT_TRUE(doc);

    std::vector<std::string> errors;
    for (auto const &finding : preflight(doc.get(), Mode::X1a, false)) {
        if (finding.error) {
            errors.push_back(finding.id);
        }
    }
    EXPECT_EQ(errors, (std::vector<std::string>{"root"}));

    for (auto const &finding : preflight(doc.get(), Mode::X4, false)) {
        EXPECT_FALSE(finding.error) << finding.id << ": " << finding.reason;
    }
}

TEST_F(PdfXTest, preflightX4AllowsTransparency)
{
    for (char const *id : {"op-half", "fill-op", "masked", "blend", "grad-alpha", "img-alpha",
                           "blurred", "opaque"}) {
        auto doc = transparency_document();
        ASSERT_TRUE(doc) << id;
        hide_other_items(doc->getRoot(), id);

        auto const findings = preflight(doc.get(), Mode::X4, true);
        for (auto const &finding : findings) {
            EXPECT_FALSE(finding.error) << id << ": " << finding.reason;
        }
    }
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
