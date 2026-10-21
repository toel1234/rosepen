// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the document output (CMYK) profile API in DocumentCMS.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/document-cms.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <regex>
#include <string>

#include "colors/cms/system.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "inkscape.h"
#include "object/color-profile.h"
#include "object/sp-namedview.h"
#include "object/sp-rect.h"
#include "style.h"
#include "xml/repr.h"

static std::string icc_dir = INKSCAPE_TESTS_DIR "/data/colors/";
static std::string svg_file = INKSCAPE_TESTS_DIR "/data/colors/cms-output-profile.svg";
static std::string svg_out_file = INKSCAPE_TESTS_DIR "/data/colors/cms-output-profile-roundtrip.svg";

using namespace Inkscape;
using namespace Inkscape::Colors;

namespace {

std::regex const &outputValueRegex()
{
    static std::regex const re("^#[0-9a-f]{6} icc-color\\(cmyk-out, [0-9.]+, [0-9.]+, [0-9.]+, [0-9.]+\\)$");
    return re;
}

class ColorDocumentOutputProfileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Setup inkscape dependency
        Inkscape::Application::create(false);

        // Allow lookup by ID and name with test icc profiles
        auto &cms = Inkscape::Colors::CMS::System::get();
        cms.clearDirectoryPaths();
        cms.addDirectoryPath(icc_dir, false);
        cms.refreshProfiles();

        // Load the test svg file with an attached cmyk output profile
        doc = SPDocument::createNewDoc(svg_file.c_str());
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
    }

    void TearDown() override
    {
        doc.reset();
        std::remove(svg_out_file.c_str());
    }

    char const *outputAttribute() const
    {
        return doc->getNamedView()->getRepr()->attribute("inkscape:output-profile");
    }

    std::unique_ptr<SPDocument> doc;
};

/**
 * AC-1: Without the attribute the first attached CMYK space is the output space,
 * an unresolvable attribute falls back the same way and an empty document
 * profile list gives nullptr.
 */
TEST_F(ColorDocumentOutputProfileTest, fallbackOutputSpace)
{
    auto &tr = doc->getDocumentCMS();

    ASSERT_EQ(outputAttribute(), nullptr);
    auto space = tr.getOutputSpace();
    ASSERT_TRUE(space);
    EXPECT_EQ(space->getName(), "cmyk-out");
    EXPECT_EQ(space->getComponentType(), Space::Type::CMYK);

    // An attribute that doesn't resolve falls back too.
    doc->getNamedView()->getRepr()->setAttribute("inkscape:output-profile", "nonsense");
    space = tr.getOutputSpace();
    ASSERT_TRUE(space);
    EXPECT_EQ(space->getName(), "cmyk-out");
    doc->getNamedView()->getRepr()->removeAttribute("inkscape:output-profile");
}

/**
 * AC-1: Only CMYK profiles can become the output profile, the attribute is
 * written to the namedview and an empty name removes it again.
 */
TEST_F(ColorDocumentOutputProfileTest, setOutputProfile)
{
    auto &tr = doc->getDocumentCMS();

    EXPECT_FALSE(tr.setOutputProfile("nonsense"));
    EXPECT_EQ(outputAttribute(), nullptr);

    EXPECT_TRUE(tr.setOutputProfile("cmyk-out"));
    ASSERT_NE(outputAttribute(), nullptr);
    EXPECT_EQ(std::string(outputAttribute()), "cmyk-out");
    ASSERT_TRUE(tr.getOutputSpace());
    EXPECT_EQ(tr.getOutputSpace()->getName(), "cmyk-out");

    // An empty name removes the attribute, the CMYK fallback remains.
    EXPECT_TRUE(tr.setOutputProfile(""));
    EXPECT_EQ(outputAttribute(), nullptr);
    ASSERT_TRUE(tr.getOutputSpace());
    EXPECT_EQ(tr.getOutputSpace()->getName(), "cmyk-out");
}

/**
 * AC-2: Flat colors convert into `#hex icc-color(cmyk-out, c, m, y, k)` values.
 */
TEST_F(ColorDocumentOutputProfileTest, convertToOutputSpaceRegex)
{
    auto &tr = doc->getDocumentCMS();

    auto fill = tr.parse("#ff0000");
    ASSERT_TRUE(fill);
    auto fill_out = tr.convertToOutputSpace(*fill);
    ASSERT_TRUE(fill_out);
    EXPECT_TRUE(std::regex_match(fill_out->toString(), outputValueRegex())) << fill_out->toString();

    auto stroke = tr.parse("device-cmyk(0 0 0 1)");
    ASSERT_TRUE(stroke);
    auto stroke_out = tr.convertToOutputSpace(*stroke);
    ASSERT_TRUE(stroke_out);
    EXPECT_TRUE(std::regex_match(stroke_out->toString(), outputValueRegex())) << stroke_out->toString();
}

/**
 * AC-2: The fill and stroke paints of a rect convert and write back into the
 * style, the same way the object-convert-to-output-profile action does.
 */
TEST_F(ColorDocumentOutputProfileTest, convertRectFillAndStroke)
{
    auto &tr = doc->getDocumentCMS();
    auto rect = cast<SPRect>(doc->getObjectById("r1"));
    ASSERT_TRUE(rect);

    ASSERT_TRUE(rect->style->fill.isColor());
    auto fill = tr.convertToOutputSpace(rect->style->fill.getColor());
    ASSERT_TRUE(fill);
    EXPECT_TRUE(std::regex_match(fill->toString(), outputValueRegex())) << fill->toString();

    ASSERT_TRUE(rect->style->stroke.isColor());
    auto stroke = tr.convertToOutputSpace(rect->style->stroke.getColor());
    ASSERT_TRUE(stroke);
    EXPECT_TRUE(std::regex_match(stroke->toString(), outputValueRegex())) << stroke->toString();

    SPCSSAttr *css = sp_repr_css_attr(rect->getRepr(), "style");
    sp_repr_css_set_property(css, "fill", fill->toString().c_str());
    sp_repr_css_set_property(css, "stroke", stroke->toString().c_str());
    sp_repr_css_change(rect->getRepr(), css, "style");
    sp_repr_css_attr_unref(css);

    std::string const style = rect->getRepr()->attribute("style");
    EXPECT_NE(style.find("icc-color(cmyk-out,"), std::string::npos);
}

/**
 * AC-2: Colors already in the output space are left unchanged, converting
 * twice gives the same string.
 */
TEST_F(ColorDocumentOutputProfileTest, idempotentConversion)
{
    auto &tr = doc->getDocumentCMS();
    auto rect = cast<SPRect>(doc->getObjectById("r2"));
    ASSERT_TRUE(rect);
    ASSERT_TRUE(rect->style->fill.isColor());

    auto color = rect->style->fill.getColor();
    std::string const before = color.toString();
    ASSERT_NE(before.find("icc-color(cmyk-out,"), std::string::npos);

    auto once = tr.convertToOutputSpace(color);
    ASSERT_TRUE(once);
    EXPECT_EQ(once->toString(), before);

    auto twice = tr.convertToOutputSpace(*once);
    ASSERT_TRUE(twice);
    EXPECT_EQ(twice->toString(), before);
}

/**
 * AC-2: The converted colors and the output profile name survive a save and
 * reload byte-for-byte. The file is written next to the fixture so the
 * relative `xlink:href="default_cmyk.icc"` keeps resolving.
 */
TEST_F(ColorDocumentOutputProfileTest, saveRoundTrip)
{
    auto &tr = doc->getDocumentCMS();
    ASSERT_TRUE(tr.setOutputProfile("cmyk-out"));

    auto rect = cast<SPRect>(doc->getObjectById("r1"));
    ASSERT_TRUE(rect);
    ASSERT_TRUE(rect->style->fill.isColor());
    ASSERT_TRUE(rect->style->stroke.isColor());

    auto fill = tr.convertToOutputSpace(rect->style->fill.getColor());
    auto stroke = tr.convertToOutputSpace(rect->style->stroke.getColor());
    ASSERT_TRUE(fill);
    ASSERT_TRUE(stroke);

    SPCSSAttr *css = sp_repr_css_attr(rect->getRepr(), "style");
    sp_repr_css_set_property(css, "fill", fill->toString().c_str());
    sp_repr_css_set_property(css, "stroke", stroke->toString().c_str());
    sp_repr_css_change(rect->getRepr(), css, "style");
    sp_repr_css_attr_unref(css);

    std::string const style = rect->getRepr()->attribute("style");
    auto const pos = style.find("icc-color(");
    ASSERT_NE(pos, std::string::npos);
    std::string const icc_before = style.substr(pos);

    // Save like production code does: hrefs are rebased from the document's
    // base to the saved file's location. Both are the fixture directory here,
    // so the relative xlink:href="default_cmyk.icc" must survive unchanged.
    // (Plain sp_repr_save_file would resolve relative hrefs against the
    // process CWD, breaking the profile on reload.)
    ASSERT_TRUE(sp_repr_save_rebased_file(doc->getReprDoc(), svg_out_file.c_str(), nullptr,
                                          doc->getDocumentBase(), svg_out_file.c_str()));

    auto reloaded = SPDocument::createNewDoc(svg_out_file.c_str());
    ASSERT_TRUE(reloaded);
    reloaded->ensureUpToDate();

    // The output profile name survives the round trip.
    ASSERT_NE(reloaded->getNamedView()->getRepr()->attribute("inkscape:output-profile"), nullptr);
    EXPECT_EQ(std::string(reloaded->getNamedView()->getRepr()->attribute("inkscape:output-profile")), "cmyk-out");
    auto space = reloaded->getDocumentCMS().getOutputSpace();
    ASSERT_TRUE(space);
    EXPECT_EQ(space->getName(), "cmyk-out");

    // The converted fill is byte-identical after the round trip.
    auto reloaded_rect = cast<SPRect>(reloaded->getObjectById("r1"));
    ASSERT_TRUE(reloaded_rect);
    std::string const style_out = reloaded_rect->getRepr()->attribute("style");
    auto const pos_out = style_out.find("icc-color(");
    ASSERT_NE(pos_out, std::string::npos);
    EXPECT_EQ(style_out.substr(pos_out), icc_before);
}

/**
 * Removing the profile which is the output profile clears the attribute and
 * deleting the color-profile object leaves the document without any output
 * space.
 */
TEST_F(ColorDocumentOutputProfileTest, removeProfileClearsAttribute)
{
    auto &tr = doc->getDocumentCMS();
    auto space = tr.getSpace("cmyk-out");
    ASSERT_TRUE(space);
    ASSERT_TRUE(tr.setOutputProfile("cmyk-out"));
    ASSERT_EQ(std::string(outputAttribute()), "cmyk-out");

    tr.removeProfile(space);

    EXPECT_EQ(outputAttribute(), nullptr);
    EXPECT_FALSE(tr.getSpace("cmyk-out"));

    // Deleting the color-profile object removes the link and with it the
    // last CMYK space, so there is no output space at all.
    auto cp = cast<ColorProfile>(doc->getObjectById("profile1"));
    ASSERT_TRUE(cp);
    cp->deleteObject();
    EXPECT_FALSE(tr.getOutputSpace());
}

} // namespace

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
