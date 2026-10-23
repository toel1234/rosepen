// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the output profile mapping of imported drawings: attaching the
 * preferred profile, converting the flat paints of the document into it,
 * idempotence, opacity, paint servers and defs staying untouched, and the
 * absence of undo history.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <src/extension/internal/cdr-import-mapping.h>

#include <gtest/gtest.h>

#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include <glib.h>

#include "colors/cms/system.h"
#include "colors/document-cms.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/enum.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/color-profile.h"
#include "object/sp-defs.h"
#include "object/sp-namedview.h"
#include "util/cast.h"
#include "xml/href-attribute-helper.h"
#include "xml/node.h"
#include "xml/repr.h"

using Inkscape::Colors::RenderingIntent;
using Inkscape::Extension::Internal::CdrImport::Mapping;
using Inkscape::Extension::Internal::CdrImport::attach_output_profile;
using Inkscape::Extension::Internal::CdrImport::convert_flat_paints;
using Inkscape::Extension::Internal::CdrImport::map_to_output_profile;

namespace {

std::string const icc_dir = INKSCAPE_TESTS_DIR "/data/colors/";
std::string const kProfile = icc_dir + "default_cmyk.icc";
std::string const kNotAProfile = icc_dir + "cms-output-profile.svg";

// Flat paints on objects, one inherited paint, one paint server, currentColor,
// and a gradient stop plus a marker path inside defs.
char const *const DOCUMENT = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="a" fill="#ff0000" style="stroke:#0000ff;fill-opacity:0.5"/>
  <g id="g" style="fill:#00ff00"><rect id="gc" width="10" height="10"/></g>
  <text id="t"><tspan id="ts" style="fill:#123456">x</tspan></text>
  <rect id="n" width="10" height="10" style="fill:none;stroke:url(#lg)"/>
  <rect id="cc" width="10" height="10" style="fill:currentColor;color:#ff00ff"/>
  <defs>
    <linearGradient id="lg"><stop id="s" offset="0" stop-color="#ff0000"/></linearGradient>
    <marker id="mk"><path id="mp" style="fill:#ff0000"/></marker>
  </defs>
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
    return value;
}

/// The four comma separated numbers of the icc-color() value, or an empty list.
std::vector<double> icc_values(std::string const &value)
{
    static std::regex const re(R"(icc-color\([^,]+, ([0-9.]+), ([0-9.]+), ([0-9.]+), ([0-9.]+)\))");
    std::smatch match;
    if (!std::regex_search(value, match, re)) {
        return {};
    }
    return {std::stod(match[1].str()), std::stod(match[2].str()), std::stod(match[3].str()),
            std::stod(match[4].str())};
}

} // namespace

class CdrImportMappingTest : public ::testing::Test
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

    void TearDown() override
    {
        doc.reset();
    }

    std::size_t defs_children()
    {
        return doc->getDefs()->children.size();
    }

    char const *output_attribute()
    {
        return doc->getNamedView()->getRepr()->attribute("inkscape:output-profile");
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(CdrImportMappingTest, AttachProfileBecomesOutputSpace)
{
    auto name = attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL);
    ASSERT_TRUE(name);
    EXPECT_FALSE(name->empty());

    auto &cms = doc->getDocumentCMS();
    auto space = cms.getOutputSpace();
    ASSERT_TRUE(space);
    EXPECT_EQ(space->getName(), *name);

    ASSERT_NE(output_attribute(), nullptr);
    EXPECT_EQ(std::string(output_attribute()), *name);

    int profiles = 0;
    for (auto &child : doc->getDefs()->children) {
        if (!is<Inkscape::ColorProfile>(&child)) {
            continue;
        }
        ++profiles;
        auto href = Inkscape::getHrefAttribute(*child.getRepr()).second;
        ASSERT_NE(href, nullptr);
        std::string const value = href;
        ASSERT_GE(value.size(), std::strlen("default_cmyk.icc"));
        EXPECT_EQ(value.rfind("default_cmyk.icc"), value.size() - std::strlen("default_cmyk.icc"));
    }
    EXPECT_EQ(profiles, 1);
}

TEST_F(CdrImportMappingTest, MissingProfileFileIsIgnored)
{
    std::size_t const before = defs_children();
    EXPECT_FALSE(attach_output_profile(doc.get(), "/nonexistent/x.icc", RenderingIntent::PERCEPTUAL));
    EXPECT_EQ(defs_children(), before);
    EXPECT_FALSE(doc->getDocumentCMS().getOutputSpace());
}

TEST_F(CdrImportMappingTest, InvalidProfileFileIsIgnored)
{
    std::size_t const before = defs_children();
    // The expected "is not a valid ICC profile" warning goes to stderr.
    auto profile = attach_output_profile(doc.get(), kNotAProfile, RenderingIntent::PERCEPTUAL);

    EXPECT_FALSE(profile);
    EXPECT_EQ(defs_children(), before);
    EXPECT_FALSE(doc->getDocumentCMS().getOutputSpace());
}

TEST_F(CdrImportMappingTest, ConvertWithoutOutputSpaceChangesNothing)
{
    std::string const before = serialized(*doc);
    EXPECT_EQ(convert_flat_paints(doc.get()), 0);
    EXPECT_EQ(serialized(*doc), before);
}

TEST_F(CdrImportMappingTest, ConvertFlatPaintsAfterAttach)
{
    auto name = attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL);
    ASSERT_TRUE(name);

    EXPECT_EQ(convert_flat_paints(doc.get()), 4);

    std::regex const color_regex("^#[0-9a-f]{6} icc-color\\(" + escape_regex(*name) +
                                 ", [0-9.]+, [0-9.]+, [0-9.]+, [0-9.]+\\)$");

    EXPECT_TRUE(std::regex_match(style_property(*doc, "a", "fill"), color_regex))
        << style_property(*doc, "a", "fill");
    EXPECT_TRUE(std::regex_match(style_property(*doc, "a", "stroke"), color_regex))
        << style_property(*doc, "a", "stroke");
    EXPECT_EQ(style_property(*doc, "a", "fill-opacity"), "0.5");
    EXPECT_TRUE(std::regex_match(style_property(*doc, "g", "fill"), color_regex));
    EXPECT_TRUE(std::regex_match(style_property(*doc, "ts", "fill"), color_regex));

    // Inherited paint is not written onto the child.
    EXPECT_TRUE(style_property(*doc, "gc", "fill").empty());

    // Paint servers, currentColor and the contents of defs stay as they are.
    EXPECT_EQ(style_property(*doc, "n", "fill"), "none");
    EXPECT_EQ(style_property(*doc, "n", "stroke"), "url(#lg)");
    EXPECT_EQ(style_property(*doc, "cc", "fill"), "currentColor");
    EXPECT_EQ(style_property(*doc, "s", "stop-color"), "");
    auto *stop = doc->getObjectById("s");
    ASSERT_TRUE(stop);
    ASSERT_NE(stop->getAttribute("stop-color"), nullptr);
    EXPECT_EQ(std::string(stop->getAttribute("stop-color")), "#ff0000");
    EXPECT_EQ(style_property(*doc, "mp", "fill"), "#ff0000");

    // A red fill may not come out with almost no magenta or yellow.
    auto const values = icc_values(style_property(*doc, "a", "fill"));
    ASSERT_EQ(values.size(), 4u);
    EXPECT_LT(values[0], 0.2);
    EXPECT_GT(values[1], 0.5);
    EXPECT_GT(values[2], 0.5);
}

TEST_F(CdrImportMappingTest, ConversionIsIdempotent)
{
    ASSERT_TRUE(attach_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL));
    ASSERT_EQ(convert_flat_paints(doc.get()), 4);

    std::string const before = serialized(*doc);
    EXPECT_EQ(convert_flat_paints(doc.get()), 0);
    EXPECT_EQ(serialized(*doc), before);
}

TEST_F(CdrImportMappingTest, MapToOutputProfileWithoutUsesNothing)
{
    std::string const before = serialized(*doc);
    Mapping const mapping = map_to_output_profile(doc.get(), "", RenderingIntent::PERCEPTUAL);
    EXPECT_FALSE(mapping.profile);
    EXPECT_EQ(mapping.converted, 0);
    EXPECT_EQ(serialized(*doc), before);
    EXPECT_FALSE(doc->getDocumentCMS().getOutputSpace());
}

TEST_F(CdrImportMappingTest, MapToOutputProfileAttachesAndConverts)
{
    Mapping const mapping = map_to_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL);
    ASSERT_TRUE(mapping.profile);
    EXPECT_EQ(mapping.converted, 4);
    ASSERT_TRUE(doc->getDocumentCMS().getOutputSpace());
    EXPECT_EQ(doc->getDocumentCMS().getOutputSpace()->getName(), *mapping.profile);
}

TEST_F(CdrImportMappingTest, MappingLeavesNoUndoHistory)
{
    ASSERT_TRUE(map_to_output_profile(doc.get(), kProfile, RenderingIntent::PERCEPTUAL).profile);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(CdrImportMappingTest, CloneContentInDefsIsNotReachedThroughUse)
{
    // A <use> shows a copy of its referenced element as its child object; the
    // walk must not follow it into defs (the copy shares the original's repr).
    static char const *const WITH_USE = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100">
  <defs><symbol id="sym"><rect id="sr" width="10" height="10" style="fill:#ff0000"/></symbol></defs>
  <use id="u" xlink:href="#sym" style="stroke:#0000ff"/>
</svg>)SVG";
    auto use_doc = SPDocument::createNewDocFromMem({WITH_USE, std::strlen(WITH_USE)});
    ASSERT_TRUE(use_doc);
    use_doc->ensureUpToDate();

    ASSERT_TRUE(attach_output_profile(use_doc.get(), kProfile, RenderingIntent::PERCEPTUAL));
    EXPECT_EQ(convert_flat_paints(use_doc.get()), 1);
    EXPECT_EQ(style_property(*use_doc, "sr", "fill"), "#ff0000");
    EXPECT_FALSE(icc_values(style_property(*use_doc, "u", "stroke")).empty());
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
