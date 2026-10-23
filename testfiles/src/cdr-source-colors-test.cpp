// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the source-colour mapping of imported drawings: CMYK values written
 * verbatim as an icc-color of the output profile, spot inks turned into named
 * swatches, tints flattened, malformed values ignored, and no undo history.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <src/extension/internal/cdr-import-mapping.h>
#include <src/extension/internal/cdr-source-colors.h>

#include <gtest/gtest.h>

#include <charconv>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include <glib.h>

#include "colors/cms/system.h"
#include "colors/document-cms.h"
#include "colors/spaces/enum.h"
#include "colors/spot-swatch.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/sp-gradient.h"
#include "object/sp-stop.h"
#include "object/sp-object.h"
#include "xml/node.h"
#include "xml/repr.h"

using Inkscape::Colors::RenderingIntent;
using Inkscape::Colors::Spot::get_spot_name;
using Inkscape::Extension::Internal::CdrImport::apply_source_colors;
using Inkscape::Extension::Internal::CdrImport::attach_output_profile;
using Inkscape::Extension::Internal::CdrImport::convert_flat_paints;

namespace {

std::string const icc_dir = INKSCAPE_TESTS_DIR "/data/colors/";
std::string const kProfile = icc_dir + "default_cmyk.icc";

/// A CMYK fill, a CMYK outline, a spot at full strength, a spot tint, a
/// malformed value, a plain sRGB element, one inside a group and one in defs.
char const *const DOCUMENT = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="cmyk" fill="#e0c4a8" data-rp-fill="cmyk:0.1,0.2,0.3,0.4"/>
  <rect id="stroke" width="10" height="10" style="fill:none;stroke:#000000" data-rp-stroke="cmyk:0,0,0,1"/>
  <ellipse id="spot" cx="20" cy="20" rx="5" ry="5" fill="#ff8000" data-rp-fill="spot:1,0,0.5,1,0,Test%20Ink"/>
  <rect id="tint" width="10" height="10" fill="#ffbf80" data-rp-fill="spot:0.5,0,0.5,1,0,Test%20Ink"/>
  <rect id="bad" width="10" height="10" fill="#123456" data-rp-fill="cmyk:1.5,0,0,0"/>
  <rect id="plain" width="10" height="10" fill="#3366cc"/>
  <g id="group"><rect id="nested" width="10" height="10" fill="#e0c4a8" data-rp-fill="cmyk:0.1,0.2,0.3,0.4"/></g>
  <defs><rect id="indefs" width="10" height="10" fill="#e0c4a8" data-rp-fill="cmyk:0.1,0.2,0.3,0.4"/></defs>
</svg>)SVG";

std::string escape_regex(std::string const &text)
{
    std::string result;
    for (char c : text) {
        if (std::strchr(".^$|()[]{}*+?\\", c)) {
            result += '\\';
        }
        result += c;
    }
    return result;
}

std::string serialized(SPDocument &doc)
{
    doc.ensureUpToDate();
    return sp_repr_save_buf(doc.getReprDoc()).raw();
}

std::string style_property(SPDocument &doc, char const *id, char const *property)
{
    auto *object = doc.getObjectById(id);
    if (!object) {
        return {};
    }
    SPCSSAttr *css = sp_repr_css_attr(object->getRepr(), "style");
    std::string value = sp_repr_css_property(css, property, "");
    sp_repr_css_attr_unref(css);
    if (value.empty()) {
        // Paints may still be a plain presentation attribute.
        if (char const *attribute = object->getRepr()->attribute(property)) {
            value = attribute;
        }
    }
    return value;
}

std::string attribute(SPDocument &doc, char const *id, char const *name)
{
    auto *object = doc.getObjectById(id);
    if (!object) {
        return {};
    }
    char const *value = object->getRepr()->attribute(name);
    return value ? value : "";
}

/// A decimal number; from_chars, so the test's locale cannot matter.
double number(std::string const &text)
{
    double value = 0.0;
    auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() ? value : 0.0;
}

/// The four comma separated numbers of an icc-color() value.
std::vector<double> icc_values(std::string const &value)
{
    static std::regex const re(R"(icc-color\([^,]+, ([0-9.]+), ([0-9.]+), ([0-9.]+), ([0-9.]+)\))");
    std::smatch match;
    if (!std::regex_search(value, match, re)) {
        return {};
    }
    return {number(match[1].str()), number(match[2].str()), number(match[3].str()),
            number(match[4].str())};
}

} // namespace

class CdrSourceColorsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Inkscape::Application::create(false);

        auto &cms = Inkscape::Colors::CMS::System::get();
        cms.clearDirectoryPaths();
        cms.addDirectoryPath(icc_dir, false);
        cms.refreshProfiles();

        doc = SPDocument::createNewDocFromMem({DOCUMENT, std::strlen(DOCUMENT)});
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    void TearDown() override { doc.reset(); }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(CdrSourceColorsTest, without_an_output_space_the_paints_stay_srgb)
{
    // Six annotations are walked (the one in defs is not).
    auto const stats = apply_source_colors(doc.get());
    EXPECT_EQ(stats.cmyk, 0);
    EXPECT_EQ(stats.spot, 0);
    EXPECT_EQ(stats.flattened, 0);
    EXPECT_EQ(stats.untouched, 6);

    EXPECT_EQ(style_property(*doc, "cmyk", "fill"), "#e0c4a8");
    EXPECT_EQ(style_property(*doc, "stroke", "stroke"), "#000000");
    EXPECT_EQ(style_property(*doc, "spot", "fill"), "#ff8000");
    EXPECT_EQ(style_property(*doc, "plain", "fill"), "#3366cc");

    // Every walked annotation is gone; the one inside defs was never reached.
    for (auto const *id : {"cmyk", "stroke", "spot", "tint", "bad", "nested"}) {
        EXPECT_EQ(attribute(*doc, id, "data-rp-fill"), "") << id;
        EXPECT_EQ(attribute(*doc, id, "data-rp-stroke"), "") << id;
    }
    EXPECT_EQ(attribute(*doc, "indefs", "data-rp-fill"), "cmyk:0.1,0.2,0.3,0.4")
        << "defs are not walked, so their annotation stays";
}

TEST_F(CdrSourceColorsTest, cmyk_values_keep_the_preview_and_the_source_numbers)
{
    auto const profile = attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL);
    ASSERT_TRUE(profile);

    auto const stats = apply_source_colors(doc.get());
    EXPECT_EQ(stats.cmyk, 3) << "the fill, the outline and the nested fill";
    EXPECT_EQ(stats.spot, 1);
    EXPECT_EQ(stats.flattened, 1);
    EXPECT_EQ(stats.untouched, 1) << "the malformed value";

    std::regex const exact("^#e0c4a8 icc-color\\(" + escape_regex(*profile) +
                           ", 0\\.1, 0\\.2, 0\\.3, 0\\.4\\)$");
    EXPECT_TRUE(std::regex_match(style_property(*doc, "cmyk", "fill"), exact))
        << style_property(*doc, "cmyk", "fill");

    // The outline keeps its own preview and its own numbers.
    std::regex const black("^#000000 icc-color\\(" + escape_regex(*profile) +
                           ", 0, 0, 0, 1\\)$");
    EXPECT_TRUE(std::regex_match(style_property(*doc, "stroke", "stroke"), black))
        << style_property(*doc, "stroke", "stroke");
    EXPECT_EQ(style_property(*doc, "stroke", "fill"), "none");

    // The nested element is reached too.
    EXPECT_TRUE(std::regex_match(style_property(*doc, "nested", "fill"), exact))
        << style_property(*doc, "nested", "fill");

    // The malformed value is dropped and the paint is left alone.
    EXPECT_EQ(style_property(*doc, "bad", "fill"), "#123456");
    EXPECT_EQ(attribute(*doc, "bad", "data-rp-fill"), "");

    // Elements without an annotation are untouched.
    EXPECT_EQ(style_property(*doc, "plain", "fill"), "#3366cc");
}

TEST_F(CdrSourceColorsTest, a_spot_becomes_a_named_swatch)
{
    ASSERT_TRUE(attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL));
    auto const stats = apply_source_colors(doc.get());
    ASSERT_EQ(stats.spot, 1);

    auto const value = style_property(*doc, "spot", "fill");
    ASSERT_EQ(0u, value.rfind("url(#", 0)) << value;
    ASSERT_EQ(')', value.back());

    auto const id = value.substr(5, value.size() - 6);
    auto *swatch = dynamic_cast<SPGradient *>(doc->getObjectById(id.c_str()));
    ASSERT_TRUE(swatch) << id;

    auto const name = get_spot_name(swatch);
    ASSERT_TRUE(name);
    EXPECT_EQ("Test Ink", *name);

    // Its alternate is the ink's process colour at full strength.
    EXPECT_GE(swatch->getStopCount(), 1);
    auto const *stop = swatch->getFirstStop();
    ASSERT_TRUE(stop);
    // Four CMYK components, then the colour's alpha channel.
    auto const values = stop->getColor().getValues();
    ASSERT_GE(values.size(), 4u);
    EXPECT_NEAR(values[0], 0.0, 1e-6);
    EXPECT_NEAR(values[1], 0.5, 1e-6);
    EXPECT_NEAR(values[2], 1.0, 1e-6);
    EXPECT_NEAR(values[3], 0.0, 1e-6);
}

TEST_F(CdrSourceColorsTest, a_spot_tint_is_flattened)
{
    auto const profile = attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL);
    ASSERT_TRUE(profile);
    auto const stats = apply_source_colors(doc.get());
    ASSERT_EQ(stats.flattened, 1);

    auto const value = style_property(*doc, "tint", "fill");
    EXPECT_EQ(0u, value.find("#ffbf80 icc-color(")) << value;
    auto const values = icc_values(value);
    ASSERT_EQ(values.size(), 4u) << value;
    EXPECT_NEAR(values[0], 0.0, 1e-6);
    EXPECT_NEAR(values[1], 0.25, 1e-6);
    EXPECT_NEAR(values[2], 0.5, 1e-6);
    EXPECT_NEAR(values[3], 0.0, 1e-6);
}

TEST_F(CdrSourceColorsTest, converting_the_rest_leaves_the_source_numbers_alone)
{
    ASSERT_TRUE(attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL));
    EXPECT_EQ(apply_source_colors(doc.get()).cmyk, 3);

    auto const before = style_property(*doc, "cmyk", "fill");
    auto const before_nested = style_property(*doc, "nested", "fill");

    // Only the paints that were not given source numbers are converted: the
    // plain element and the element whose annotation did not parse.
    EXPECT_EQ(convert_flat_paints(doc.get()), 2);
    EXPECT_EQ(style_property(*doc, "cmyk", "fill"), before)
        << "verbatim CMYK wins over the conversion";
    EXPECT_EQ(style_property(*doc, "nested", "fill"), before_nested);
}

TEST_F(CdrSourceColorsTest, applying_twice_changes_nothing)
{
    ASSERT_TRUE(attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL));
    ASSERT_EQ(apply_source_colors(doc.get()).cmyk, 3);

    auto const before = serialized(*doc);
    auto const stats = apply_source_colors(doc.get());
    EXPECT_EQ(stats.cmyk, 0);
    EXPECT_EQ(stats.spot, 0);
    EXPECT_EQ(stats.flattened, 0);
    EXPECT_EQ(stats.untouched, 0);
    EXPECT_EQ(serialized(*doc), before);
}

TEST_F(CdrSourceColorsTest, no_undo_history)
{
    ASSERT_TRUE(attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL));
    apply_source_colors(doc.get());
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
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
