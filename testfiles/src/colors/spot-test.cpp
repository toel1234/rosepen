// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure spot color helpers.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/spot.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "colors/cms/profile.h"
#include "colors/color.h"
#include "colors/spaces/cms.h"

using namespace Inkscape::Colors;

namespace {

std::string const cmyk_icc = INKSCAPE_TESTS_DIR "/data/colors/default_cmyk.icc";

} // namespace

TEST(ColorsSpot, normalizeName)
{
    EXPECT_EQ(Spot::normalize_name("  PANTONE   185 \t C  "), "PANTONE 185 C");
    EXPECT_EQ(Spot::normalize_name("Gr\xc3\xbcn 1"), "Gr\xc3\xbcn 1");

    EXPECT_FALSE(Spot::normalize_name(""));
    EXPECT_FALSE(Spot::normalize_name(" \t\n "));
    EXPECT_FALSE(Spot::normalize_name("bad\x01name"));
    EXPECT_FALSE(Spot::normalize_name("bad\xffname"));
    EXPECT_FALSE(Spot::normalize_name(std::string(128, 'a')));
    EXPECT_TRUE(Spot::normalize_name(std::string(127, 'a')));
}

TEST(ColorsSpot, swatchId)
{
    EXPECT_EQ(Spot::swatch_id("PANTONE 185 C"), "spot-PANTONE_185_C");
    EXPECT_EQ(Spot::swatch_id("PANTONE/185#C"), "spot-PANTONE_185_C");
    EXPECT_EQ(Spot::swatch_id("spot-x"), "spot-spot-x");
    EXPECT_EQ(Spot::swatch_id("PANTONE 185 C"), Spot::swatch_id("PANTONE 185 C"));
}

TEST(ColorsSpot, alternateForDeviceCmyk)
{
    auto color = Color::parse("device-cmyk(0 0.9 0.8 0.1)");
    ASSERT_TRUE(color);

    auto const alternate = Spot::alternate_for(*color);
    EXPECT_EQ(alternate.model, Spot::Model::CMYK);
    ASSERT_EQ(alternate.values.size(), 4u);
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(alternate.values[i], color->get(i));
    }
}

TEST(ColorsSpot, alternateForRgb)
{
    auto color = Color::parse("#ff0000");
    ASSERT_TRUE(color);

    auto const alternate = Spot::alternate_for(*color);
    EXPECT_EQ(alternate.model, Spot::Model::RGB);
    EXPECT_EQ(alternate.values, (std::vector<double>{1.0, 0.0, 0.0}));
}

TEST(ColorsSpot, alternateDropsAlpha)
{
    Color const color(Space::Type::RGB, {1.0, 0.0, 0.0, 0.5});
    ASSERT_EQ(color.size(), 4u);

    auto const alternate = Spot::alternate_for(color);
    EXPECT_EQ(alternate.model, Spot::Model::RGB);
    EXPECT_EQ(alternate.values, (std::vector<double>{1.0, 0.0, 0.0}));
}

TEST(ColorsSpot, alternateForCmsCmyk)
{
    auto profile = CMS::Profile::create_from_uri(cmyk_icc);
    ASSERT_TRUE(profile);
    auto space = std::make_shared<Space::CMS>(profile);
    Color const color(space, {0.1, 0.2, 0.3, 0.4});

    auto const alternate = Spot::alternate_for(color);
    EXPECT_EQ(alternate.model, Spot::Model::CMYK);
    EXPECT_EQ(alternate.values, (std::vector<double>{0.1, 0.2, 0.3, 0.4}));
}
