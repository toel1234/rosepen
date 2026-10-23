// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the C ABI of the TraceDraw bridge: the version handshake, argument
 * checks, error mapping, and a full round trip through the linked static
 * library (export model JSON -> a version 14 file -> open -> annotated page
 * SVG with the source CMYK numbers and the layer names).
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "tracecdr-ffi.h"

namespace {

/** A buffer from the library, freed on scope exit with the length it wrote. */
class Buffer
{
public:
    Buffer() = default;
    Buffer(Buffer const &) = delete;
    Buffer &operator=(Buffer const &) = delete;
    ~Buffer() { release(); }

    void release()
    {
        if (p) {
            rp_free(p, n);
        }
        p = nullptr;
        n = 0;
    }

    uint8_t *p = nullptr;
    size_t n = 0;
};

std::string text(Buffer const &b)
{
    return std::string(reinterpret_cast<char const *>(b.p), b.n);
}

/** The model of a two page drawing: a CMYK rect, a spot ellipse, an RGB path
 *  and a text on layer "Ink". */
std::string const MODEL = R"({
  "schema": "rosepen-cdr-export/1",
  "title": "Bridge",
  "inks": [{"name": "Test Ink", "cmyk": [0.0, 0.5, 1.0, 0.0]}],
  "pages": [
    {"name": "Page 1", "width_mm": 210.0, "height_mm": 297.0,
     "layers": [{"name": "Ink", "visible": true, "shapes": [
       {"kind": "rect", "x": 10.0, "y": 20.0, "w": 50.0, "h": 30.0,
        "fill": {"model": "cmyk", "c": 0.1, "m": 0.2, "y": 0.3, "k": 0.4}},
       {"kind": "ellipse", "cx": 120.0, "cy": 60.0, "rx": 20.0, "ry": 10.0,
        "fill": {"model": "spot", "ink": 0, "tint": 1.0}},
       {"kind": "path", "d": "M10 10 L40 10 L40 40 Z",
        "fill": {"model": "rgb", "r": 0.2, "g": 0.4, "b": 0.8}},
       {"kind": "text", "x": 20.0, "y": 280.0,
        "spans": [{"text": "Hello", "family": "DejaVu Sans", "size_pt": 24.0}]}
     ]}]},
    {"name": "Page 2", "width_mm": 210.0, "height_mm": 297.0,
     "layers": [{"name": "Ink", "visible": true, "shapes": [
       {"kind": "rect", "x": 5.0, "y": 10.0, "w": 10.0, "h": 20.0,
        "fill": {"model": "rgb", "r": 0.5, "g": 0.5, "b": 0.5}}
     ]}]}
  ]
})";

/** Write the model at `version`; returns the file bytes. */
std::vector<uint8_t> write_model(uint16_t version)
{
    Buffer out;
    Buffer warnings;
    int const rc = rp_cdr_export(reinterpret_cast<uint8_t const *>(MODEL.data()), MODEL.size(),
                                 version, &out.p, &out.n, &warnings.p, &warnings.n);
    EXPECT_EQ(RP_OK, rc) << rp_strerror(rc);
    EXPECT_TRUE(warnings.p != nullptr);
    std::vector<uint8_t> bytes(out.p, out.p + out.n);
    return bytes;
}

} // namespace

TEST(TraceCdrFfi, abi_version_is_one)
{
    EXPECT_EQ(1u, rp_abi_version());
}

TEST(TraceCdrFfi, versions_are_reported)
{
    uint16_t oldest = 0, newest = 0, checked_since = 0, checked_until = 0;
    rp_cdr_versions(&oldest, &newest, &checked_since, &checked_until);
    EXPECT_EQ(7, oldest);
    EXPECT_EQ(27, newest);
    EXPECT_EQ(14, checked_since);
    EXPECT_EQ(27, checked_until);
}

TEST(TraceCdrFfi, null_arguments_are_refused)
{
    RpCdrDoc *doc = reinterpret_cast<RpCdrDoc *>(0x1);
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_open(nullptr, 0, nullptr, &doc));
    EXPECT_EQ(reinterpret_cast<RpCdrDoc *>(0x1), doc) << "outputs are untouched on failure";

    uint8_t const one = 'x';
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_open(&one, 0, nullptr, &doc));
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_open(&one, 1, nullptr, nullptr));

    RpCdrInfo info;
    std::memset(&info, 0, sizeof info);
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_info(nullptr, &info));
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_info(reinterpret_cast<RpCdrDoc const *>(0x1), nullptr));

    Buffer b;
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_page_svg(nullptr, 0, &b.p, &b.n));
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_warning(nullptr, 0, &b.p, &b.n));
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_page_size_mm(nullptr, 0, nullptr, nullptr));

    Buffer out, warnings;
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_export(nullptr, 0, 14, &out.p, &out.n, &warnings.p, &warnings.n));
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_export(reinterpret_cast<uint8_t const *>("{}"), 2, 14, nullptr,
                                        &out.n, &warnings.p, &warnings.n));

    // Closing nothing is a no-op, and the static strings are always there.
    rp_cdr_close(nullptr);
    rp_free(nullptr, 0);
    EXPECT_STRNE("", rp_strerror(RP_OK));
}

TEST(TraceCdrFfi, garbage_is_an_error_but_not_a_panic)
{
    uint8_t const junk[] = {'R', 'I', 'F', 'F', 0x10, 0, 0, 0, 'C', 'D', 'R', 'x',
                            'j', 'u', 'n', 'k', 0, 0, 0, 0};
    RpCdrDoc *doc = nullptr;
    int const rc = rp_cdr_open(junk, sizeof junk, "junk", &doc);
    EXPECT_NE(RP_OK, rc);
    EXPECT_NE(RP_ERR_PANIC, rc);
    EXPECT_EQ(nullptr, doc);
    EXPECT_STRNE("", rp_strerror(rc));
}

TEST(TraceCdrFfi, export_versions_14_and_13_magic)
{
    auto const v14 = write_model(14);
    ASSERT_GE(v14.size(), 4u);
    EXPECT_EQ('P', v14[0]);
    EXPECT_EQ('K', v14[1]);
    EXPECT_EQ(0x03, v14[2]);
    EXPECT_EQ(0x04, v14[3]);

    auto const v13 = write_model(13);
    ASSERT_GE(v13.size(), 12u);
    std::string const head(reinterpret_cast<char const *>(v13.data()), 12);
    EXPECT_EQ("RIFF", head.substr(0, 4));
    EXPECT_EQ("CDR", head.substr(8, 3));
}

TEST(TraceCdrFfi, round_trip_keeps_pages_colours_and_layers)
{
    auto const file = write_model(14);

    RpCdrDoc *doc = nullptr;
    int rc = rp_cdr_open(file.data(), file.size(), "bridge", &doc);
    ASSERT_EQ(RP_OK, rc) << rp_strerror(rc);
    ASSERT_NE(nullptr, doc);

    RpCdrInfo info;
    std::memset(&info, 0, sizeof info);
    ASSERT_EQ(RP_OK, rp_cdr_info(doc, &info));
    EXPECT_EQ(2u, info.pages);
    EXPECT_EQ(14u, info.version);
    EXPECT_EQ(0u, info.skipped_objects);

    double w = 0.0, h = 0.0;
    for (size_t page = 0; page < 2; ++page) {
        ASSERT_EQ(RP_OK, rp_cdr_page_size_mm(doc, page, &w, &h));
        EXPECT_NEAR(210.0, w, 0.01);
        EXPECT_NEAR(297.0, h, 0.01);
    }
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_page_size_mm(doc, 2, &w, &h));

    Buffer svg;
    ASSERT_EQ(RP_OK, rp_cdr_page_svg(doc, 0, &svg.p, &svg.n));
    ASSERT_NE(nullptr, svg.p);
    ASSERT_GT(svg.n, 0u);
    EXPECT_EQ('\0', svg.p[svg.n]) << "buffers are NUL-terminated past their length";

    std::string const page = text(svg);
    EXPECT_NE(std::string::npos, page.find("inkscape:label=\"Ink\"")) << page;
    EXPECT_NE(std::string::npos, page.find("inkscape:groupmode=\"layer\"")) << page;
    EXPECT_NE(std::string::npos, page.find("xmlns:inkscape=")) << page;
    EXPECT_NE(std::string::npos, page.find("data-rp-fill=\"cmyk:0.1,0.2,0.3,0.4\"")) << page;

    // The format's writer stores a spot colour as its process stand-in and
    // keeps no ink table, so a spot does not survive export -> import as a
    // spot (FINDING, recorded in SCHEMA.md). Its numbers are still exact.
    EXPECT_NE(std::string::npos, page.find("data-rp-fill=\"cmyk:0,0.5,1,0\"")) << page;
    EXPECT_EQ(std::string::npos, page.find("spot:")) << "no spot annotation after a round trip";

    // No bare integer id remains.
    EXPECT_EQ(std::string::npos, page.find("id=\"1\"")) << page;
    EXPECT_NE(std::string::npos, page.find("id=\"td")) << page;

    // Page 2 reads too; a page past the end is an argument error.
    Buffer second;
    EXPECT_EQ(RP_OK, rp_cdr_page_svg(doc, 1, &second.p, &second.n));
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_page_svg(doc, 2, &second.p, &second.n));

    // Warnings, if the reader produced any, are readable and bounded.
    for (size_t i = 0; i < info.warnings; ++i) {
        Buffer warning;
        ASSERT_EQ(RP_OK, rp_cdr_warning(doc, i, &warning.p, &warning.n));
        EXPECT_GT(warning.n, 0u);
    }
    Buffer none;
    EXPECT_EQ(RP_ERR_ARG, rp_cdr_warning(doc, info.warnings, &none.p, &none.n));

    rp_cdr_close(doc);

    // Invalid models are reported as JSON errors, not crashes.
    char const bad[] = "{\"schema\": \"other\", \"pages\": []}";
    Buffer out, warnings;
    EXPECT_EQ(RP_ERR_JSON, rp_cdr_export(reinterpret_cast<uint8_t const *>(bad), sizeof bad - 1,
                                         14, &out.p, &out.n, &warnings.p, &warnings.n));
}

TEST(TraceCdrFfi, version_zero_means_fourteen)
{
    Buffer out, warnings;
    std::string const model = MODEL;
    ASSERT_EQ(RP_OK, rp_cdr_export(reinterpret_cast<uint8_t const *>(model.data()), model.size(), 0,
                                   &out.p, &out.n, &warnings.p, &warnings.n));

    RpCdrDoc *doc = nullptr;
    ASSERT_EQ(RP_OK, rp_cdr_open(out.p, out.n, "zero", &doc));
    RpCdrInfo info;
    std::memset(&info, 0, sizeof info);
    ASSERT_EQ(RP_OK, rp_cdr_info(doc, &info));
    EXPECT_EQ(14u, info.version);
    rp_cdr_close(doc);
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
