// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for color objects.
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>

#include "../test-utils.h"
#include "colors/cms/profile.h"
#include "colors/color.h"
#include "colors/manager.h"
#include "colors/parser.h"
#include "colors/spaces/cms.h"

using namespace Inkscape::Colors;

static std::string cmyk_icc = INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc";

namespace {

// Get access to protected members for testing
class CMS : public Space::CMS
{
public:
    CMS(unsigned size, Space::Type type = Space::Type::CMYK)
        : Space::CMS("test-profile", size, type){};
    CMS(std::shared_ptr<Inkscape::Colors::CMS::Profile> profile, std::string name = {})
        : Space::CMS(profile, name) {};
    std::string toString(std::vector<double> const &values, bool opacity = true) const
    {
        return Space::CMS::toString(values, opacity);
    }
    bool testOverInk(double black) { return overInk({1.0, 1.0, 1.0, black}); }
    bool testOutOfGamut(std::vector<double> values, std::shared_ptr<Space::AnySpace> space)
    {
        return outOfGamut(values, space);
    }
};

TEST(ColorsSpacesCms, getNames)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile);
    ASSERT_EQ(cmyk->getName(), "Artifex-CMYK-SWOP-Profile");
    ASSERT_EQ(cmyk->getShortName(), "Artifex-CMYK-SWOP-Profile");
    ASSERT_EQ(cmyk->getSvgName(), "Artifex-CMYK-SWOP-Profile");
}

TEST(ColorsSpacesCms, parseColor)
{
    auto parser = Space::CMS::CmsParser();
    ASSERT_EQ(parser.getPrefix(), "icc-color");

    bool more = false;
    std::vector<double> output;
    std::istringstream ss("stress-test, 0.2, 90%,2,   .3 5%)");
    auto name = parser.parseColor(ss, output, more);
    ASSERT_EQ(name, "stress-test");
    ASSERT_EQ(output.size(), 5);
    ASSERT_EQ(output[0], 0.2);
    ASSERT_EQ(output[1], 0.9);
    ASSERT_EQ(output[2], 2.0);
    ASSERT_EQ(output[3], 0.3);
    ASSERT_EQ(output[4], 0.05);
}

TEST(ColorsSpaceCms, getType)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile);

    EXPECT_EQ(cmyk->getType(), Space::Type::CMS);
    EXPECT_TRUE(*cmyk == Space::Type::CMS);
    EXPECT_EQ(cmyk->getComponentType(), Space::Type::CMYK);
}

TEST(ColorsSpacesRgb, isDirect)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile);

    ASSERT_TRUE(std::dynamic_pointer_cast<Space::ProfileSpace<true>>(cmyk));
}

TEST(ColorsSpacesCms, realColor)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile);
    auto color = Color(cmyk, {0, 0, 0, 1});

    EXPECT_EQ(color.toString(), "#2c292a icc-color(Artifex-CMYK-SWOP-Profile, 0, 0, 0, 1)");
    EXPECT_EQ(color.toRGBA(), 0x2c292aff);
    EXPECT_EQ(color.toRGBA(0.5), 0x2c292a80);
    EXPECT_EQ(color.converted(Space::Type::RGB)->toString(), "#2c292a");
    EXPECT_FALSE(color.hasOpacity());
    EXPECT_EQ(color.getOpacity(), 1.0);

    color.addOpacity(0.5);
    // Opacity isn't stored in the icc-color string, because it's not supported.
    EXPECT_TRUE(color.hasOpacity());
    EXPECT_EQ(color.getOpacity(), 0.5);
    EXPECT_EQ(color.toString(), "#2c292a icc-color(Artifex-CMYK-SWOP-Profile, 0, 0, 0, 1)");
    EXPECT_EQ(color.toRGBA(), 0x2c292a80);
    EXPECT_EQ(color.toRGBA(0.5), 0x2c292a40);
    EXPECT_EQ(color.converted(Space::Type::RGB)->toString(), "#2c292a80");

    color = Color(0x2c292aff);
    EXPECT_TRUE(color.convert(cmyk));
    EXPECT_EQ(color.toString(), "#1f1b1c icc-color(Artifex-CMYK-SWOP-Profile, 0.688, 0.694, 0.648, 0.866)");
}

TEST(ColorsSpacesCms, renderingIntent)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile, "vals");

    auto color1 = Color(cmyk, {0, 0, 0, 1});
    auto color2 = Color(cmyk, {0.5, 0, 0, 0});
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::UNKNOWN);

    cmyk->setIntent(RenderingIntent::PERCEPTUAL);
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::PERCEPTUAL);
    EXPECT_EQ(color1.toString(), "#2c292a icc-color(vals, 0, 0, 0, 1)");
    EXPECT_EQ(color1.converted(Space::Type::RGB)->toString(), "#2c292a");
    EXPECT_EQ(color2.toString(), "#70d0f6 icc-color(vals, 0.5, 0, 0, 0)");

    cmyk->setIntent(RenderingIntent::RELATIVE_COLORIMETRIC);
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::RELATIVE_COLORIMETRIC);
    EXPECT_EQ(color1.toString(), "#231f20 icc-color(vals, 0, 0, 0, 1)");
    EXPECT_EQ(color1.converted(Space::Type::RGB)->toString(), "#231f20");
    EXPECT_EQ(color2.toString(), "#6dcff6 icc-color(vals, 0.5, 0, 0, 0)");

    cmyk->setIntent(RenderingIntent::SATURATION);
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::SATURATION);
    EXPECT_EQ(color1.toString(), "#2c292a icc-color(vals, 0, 0, 0, 1)");
    EXPECT_EQ(color1.converted(Space::Type::RGB)->toString(), "#2c292a");
    EXPECT_EQ(color2.toString(), "#70d0f6 icc-color(vals, 0.5, 0, 0, 0)");

    cmyk->setIntent(RenderingIntent::ABSOLUTE_COLORIMETRIC);
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::ABSOLUTE_COLORIMETRIC);
    EXPECT_EQ(color1.toString(), "#2f2d2c icc-color(vals, 0, 0, 0, 1)");
    EXPECT_EQ(color1.converted(Space::Type::RGB)->toString(), "#2f2d2c");
    EXPECT_EQ(color2.toString(), "#69b6d1 icc-color(vals, 0.5, 0, 0, 0)");

    cmyk->setIntent(RenderingIntent::RELATIVE_COLORIMETRIC_NOBPC);
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::RELATIVE_COLORIMETRIC_NOBPC);
    EXPECT_EQ(color1.toString(), "#373535 icc-color(vals, 0, 0, 0, 1)");
    EXPECT_EQ(color1.converted(Space::Type::RGB)->toString(), "#373535");
    EXPECT_EQ(color2.toString(), "#73d1f6 icc-color(vals, 0.5, 0, 0, 0)");

    // These should be the same as PERCEPTUAL
    cmyk->setIntent(RenderingIntent::UNKNOWN);
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::UNKNOWN);
    EXPECT_EQ(color1.toString(), "#2c292a icc-color(vals, 0, 0, 0, 1)");
    EXPECT_EQ(color1.converted(Space::Type::RGB)->toString(), "#2c292a");
    EXPECT_EQ(color2.toString(), "#70d0f6 icc-color(vals, 0.5, 0, 0, 0)");

    cmyk->setIntent(RenderingIntent::AUTO);
    EXPECT_EQ(cmyk->getIntent(), RenderingIntent::AUTO);
    EXPECT_EQ(color1.toString(), "#2c292a icc-color(vals, 0, 0, 0, 1)");
    EXPECT_EQ(color1.converted(Space::Type::RGB)->toString(), "#2c292a");
    EXPECT_EQ(color2.toString(), "#70d0f6 icc-color(vals, 0.5, 0, 0, 0)");
}

TEST(ColorsSpacesCms, printColor)
{
    auto space = CMS(4);

    ASSERT_FALSE(space.hasValidCmsProfile());
    EXPECT_EQ(space.toString({}), "");
    EXPECT_EQ(space.toString({1}), "");
    EXPECT_EQ(space.toString({1, 2, 3, 4}), "");
    EXPECT_EQ(space.toString({0, 0.5001, 1, 1, 2, 3, 4}), "#0080ff icc-color(test-profile, 1, 2, 3, 4)");

    space = CMS(2);
    ASSERT_FALSE(space.hasValidCmsProfile());
    EXPECT_EQ(space.toString({1}), "");
    EXPECT_EQ(space.toString({0, 0.5001, 1, 1, 2}), "#0080ff icc-color(test-profile, 1, 2)");
    EXPECT_EQ(space.toString({0, 0, 0, 1, 2, 3}), "#000000 icc-color(test-profile, 1, 2)");

    auto srgb = Inkscape::Colors::CMS::Profile::create_srgb();
    space = CMS(srgb);
    space.setIntent(RenderingIntent::AUTO);
    ASSERT_TRUE(space.hasValidCmsProfile());
    EXPECT_EQ(space.toString({1}), "");
    EXPECT_EQ(space.toString({0, 0.5001, 1}), "#0080ff icc-color(sRGB-built-in, 0, 0.5, 1)");
}

TEST(ColorsSpacesCms, outOfGamut)
{
    auto srgb = Inkscape::Colors::CMS::Profile::create_srgb();
    auto cmyk = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto space = CMS(srgb);
    auto to_space = std::make_shared<CMS>(cmyk);

    EXPECT_FALSE(space.testOutOfGamut({0.83, 0.19, 0.49}, to_space));
    // An RGB color (magenta) which is outside the cmyk color profile
    EXPECT_TRUE(space.testOutOfGamut({1.0, 0.0, 1.0}, to_space));

    auto from_space = std::make_shared<CMS>(srgb);
    auto pink = Inkscape::Colors::Color(from_space, {0.83, 0.19, 0.49});
    EXPECT_FALSE(pink.isOutOfGamut(to_space));

    auto magenta = Inkscape::Colors::Color(from_space, {1.0, 0.0, 1.0});
    EXPECT_TRUE(magenta.isOutOfGamut(to_space));
}

TEST(ColorsSpacesCms, overInk)
{
    auto space = CMS(4, Space::Type::CMYK);
    ASSERT_TRUE(space.testOverInk(0.21));
    ASSERT_FALSE(space.testOverInk(0.19));
    space = CMS(4, Space::Type::RGB);
    ASSERT_FALSE(space.testOverInk(0.21));
    ASSERT_FALSE(space.testOverInk(0.19));
}

/**
 * Policy (1): opacity is never part of the icc-color string or its hex fallback.
 */
TEST(ColorsSpacesCms, opacityPolicy)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile);

    auto color = Color(cmyk, {0.1, 0.2, 0.3, 0.4});
    EXPECT_FALSE(color.hasOpacity());
    color.addOpacity(0.5);
    EXPECT_TRUE(color.hasOpacity());

    auto const with_opacity = color.toString(true);
    auto const without_opacity = color.toString(false);

    // Requesting opacity does not change the string.
    EXPECT_EQ(with_opacity, without_opacity);

    auto const space_at = with_opacity.find(' ');
    ASSERT_NE(space_at, std::string::npos);

    // The fallback hex is #rrggbb, never #rrggbbaa.
    auto const hex = with_opacity.substr(0, space_at);
    EXPECT_EQ(hex.size(), 7u);
    EXPECT_EQ(hex.front(), '#');

    // Profile name plus exactly four icc channels.
    auto const icc = with_opacity.substr(space_at + 1);
    EXPECT_EQ(icc.rfind("icc-color(", 0), 0u);
    EXPECT_EQ(std::count(icc.begin(), icc.end(), ','), 4);
}

/**
 * Round-trip at the Color plus separate-opacity level: the icc channels survive
 * while opacity is carried by the sibling *-opacity property (SPStyle is not
 * part of this unit target).
 */
TEST(ColorsSpacesCms, opacityRoundTrip)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile);

    std::vector<double> const channels{0.1, 0.2, 0.3, 0.4};
    auto color = Color(cmyk, channels);
    color.addOpacity(0.5);

    auto const css = color.toString(false);
    ASSERT_EQ(css.find("0.5"), std::string::npos);

    auto const pos = css.find("icc-color(");
    ASSERT_NE(pos, std::string::npos);

    Space::CMS::CmsParser parser;
    std::istringstream ss(css.substr(pos + std::string("icc-color(").size()));
    std::vector<double> values;
    bool more = false;
    auto const name = parser.parseColor(ss, values, more);

    EXPECT_EQ(name, cmyk->getName());
    ASSERT_EQ(values.size(), channels.size());
    EXPECT_TRUE(VectorIsNear(values, channels, 1e-6));

    auto reborn = Color(cmyk, values);
    EXPECT_FALSE(reborn.hasOpacity());
    reborn.addOpacity(0.5);
    EXPECT_EQ(reborn.getOpacity(), 0.5);
}

/**
 * Third-party #rrggbbaa icc-color input: the parser reads the alpha, but the
 * current Color behavior drops it. Asserted here so a future change is visible.
 */
TEST(ColorsSpacesCms, thirdPartyAlphaHexIccParse)
{
    auto cmyk_profile = Inkscape::Colors::CMS::Profile::create_from_uri(cmyk_icc);
    auto cmyk = std::make_shared<CMS>(cmyk_profile);

    std::string const input = "#11223380 icc-color(" + cmyk->getName() + ", 0.1, 0.2, 0.3, 0.4)";

    Space::Type type = Space::Type::RGB;
    std::string cms_name;
    std::vector<double> values;
    std::vector<double> fallback;
    ASSERT_TRUE(Parsers::get().parse(input, type, cms_name, values, fallback));

    EXPECT_EQ(type, Space::Type::CMS);
    EXPECT_EQ(cms_name, cmyk->getName());
    ASSERT_EQ(values.size(), 4u);
    EXPECT_TRUE(VectorIsNear(values, std::vector<double>{0.1, 0.2, 0.3, 0.4}, 1e-6));

    // The parser does see the #rrggbbaa fallback with its alpha (0x80 = 0.502).
    ASSERT_EQ(fallback.size(), 4u);
    EXPECT_TRUE(VectorIsNear(fallback, std::vector<double>{0x11 / 255.0, 0x22 / 255.0, 0x33 / 255.0, 0x80 / 255.0}, 1e-6));

    // But the alpha is dropped: the color carries the icc channels only.
    auto color = Color(cmyk, values);
    EXPECT_FALSE(color.hasOpacity());
    EXPECT_EQ(color.getOpacity(), 1.0);
}

}; // namespace

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
