// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the document spot-swatch layer.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "colors/spot-swatch.h"

#include <gtest/gtest.h>
#include <glib.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>

#include "colors/color.h"
#include "colors/spot.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-gradient.h"
#include "object/sp-stop.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::Colors;

namespace {

char const *SVG =
    "<svg xmlns='http://www.w3.org/2000/svg' "
    "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'>"
    "<defs/>"
    "</svg>";

char const *SVG_WITH_COLLIDING_ID =
    "<svg xmlns='http://www.w3.org/2000/svg' "
    "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'>"
    "<defs><linearGradient id='spot-PANTONE_185_C'/></defs>"
    "</svg>";

class ColorsSpotSwatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Inkscape::Application::create(false);
    }

    void TearDown() override
    {
        _doc.reset();
    }

    void load(char const *svg)
    {
        _doc = SPDocument::createNewDocFromMem(std::span<char const>(svg, std::strlen(svg)));
        ASSERT_TRUE(_doc);
        _doc->ensureUpToDate();
    }

    SPDocument *doc() const { return _doc.get(); }

    static Color cmyk(char const *value)
    {
        auto color = Color::parse(value);
        EXPECT_TRUE(color);
        return *color;
    }

    std::unique_ptr<SPDocument> _doc;
};

} // namespace

TEST_F(ColorsSpotSwatchTest, ensureSpotCreatesSwatch)
{
    load(SVG);
    auto const alternate = cmyk("device-cmyk(0 0.92 0.76 0)");
    auto const spot = Spot::ensure_spot(doc(), "PANTONE 185 C", alternate);

    ASSERT_TRUE(spot);
    EXPECT_EQ(std::string(spot->getId()), "spot-PANTONE_185_C");
    EXPECT_TRUE(spot->isSwatch());
    EXPECT_TRUE(spot->isSolid());
    ASSERT_NE(spot->getAttribute(Spot::SPOT_ATTR), nullptr);
    EXPECT_EQ(std::string(spot->getAttribute(Spot::SPOT_ATTR)), "PANTONE 185 C");
    EXPECT_EQ(doc()->getResourceList("gradient").size(), 1u);
}

TEST_F(ColorsSpotSwatchTest, ensureSpotReusesAndKeepsAlternate)
{
    load(SVG);
    auto const first = cmyk("device-cmyk(0 0.92 0.76 0)");
    auto const spot = Spot::ensure_spot(doc(), "  PANTONE   185   C  ", first);
    ASSERT_TRUE(spot);

    auto const second = cmyk("device-cmyk(0.1 0.2 0.3 0.4)");
    auto const same = Spot::ensure_spot(doc(), "PANTONE 185 C", second);
    EXPECT_EQ(spot, same);
    EXPECT_EQ(doc()->getResourceList("gradient").size(), 1u);

    auto const stop = spot->getFirstStop();
    ASSERT_TRUE(stop);
    auto const stop_color = stop->getColor();
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(stop_color.get(i), first.get(i));
    }
}

TEST_F(ColorsSpotSwatchTest, idCollisionGetsSuffix)
{
    load(SVG_WITH_COLLIDING_ID);
    auto const spot = Spot::ensure_spot(doc(), "PANTONE 185 C", cmyk("device-cmyk(0 0.92 0.76 0)"));

    ASSERT_TRUE(spot);
    EXPECT_EQ(std::string(spot->getId()), "spot-PANTONE_185_C-2");
    EXPECT_EQ(std::string(spot->getAttribute(Spot::SPOT_ATTR)), "PANTONE 185 C");
}

TEST_F(ColorsSpotSwatchTest, renamedIdIsRegistered)
{
    load(SVG);
    auto const first = Spot::ensure_spot(doc(), "PANTONE 185 C", cmyk("device-cmyk(0 0.92 0.76 0)"));
    ASSERT_TRUE(first);
    EXPECT_EQ(doc()->getObjectById("spot-PANTONE_185_C"), first);

    // Different ink name, same sanitized id: the first rename must be visible to collision detection.
    auto const second = Spot::ensure_spot(doc(), "PANTONE_185_C", cmyk("device-cmyk(0 0.5 0.5 0)"));
    ASSERT_TRUE(second);
    EXPECT_NE(first, second);
    EXPECT_EQ(std::string(second->getId()), "spot-PANTONE_185_C-2");
    EXPECT_EQ(doc()->getObjectById("spot-PANTONE_185_C-2"), second);
    EXPECT_EQ(doc()->getObjectById("spot-PANTONE_185_C"), first);
}

TEST_F(ColorsSpotSwatchTest, saveReloadRoundTrip)
{
    load(SVG);
    auto const alternate = cmyk("device-cmyk(0 0.92 0.76 0)");
    ASSERT_TRUE(Spot::ensure_spot(doc(), "PANTONE 185 C", alternate));

    auto const path_alloc = g_build_filename(g_get_tmp_dir(), "spot-swatch-roundtrip.svg", nullptr);
    std::string const path(path_alloc);
    g_free(path_alloc);
    ASSERT_TRUE(sp_repr_save_file(doc()->getReprDoc(), path.c_str()));

    auto reloaded = SPDocument::createNewDoc(path.c_str());
    std::remove(path.c_str());
    ASSERT_TRUE(reloaded);
    reloaded->ensureUpToDate();

    auto const spots = Spot::list_spots(reloaded.get());
    ASSERT_EQ(spots.size(), 1u);

    auto const name = Spot::get_spot_name(spots[0]);
    ASSERT_TRUE(name);
    EXPECT_EQ(*name, "PANTONE 185 C");

    auto const stop = spots[0]->getFirstStop();
    ASSERT_TRUE(stop);
    auto const stop_color = stop->getColor();
    EXPECT_EQ(stop_color.getSpace(), alternate.getSpace());
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(stop_color.get(i), alternate.get(i));
    }
}

TEST_F(ColorsSpotSwatchTest, setSpotNameAndList)
{
    load(SVG);
    auto const first = Spot::ensure_spot(doc(), "PANTONE 185 C", cmyk("device-cmyk(0 0.92 0.76 0)"));
    auto const second = Spot::ensure_spot(doc(), "PANTONE 300 C", cmyk("device-cmyk(1 0.5 0 0)"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(Spot::list_spots(doc()).size(), 2u);

    Spot::set_spot_name(first, std::nullopt);
    EXPECT_FALSE(Spot::get_spot_name(first));
    EXPECT_EQ(first->getAttribute(Spot::SPOT_ATTR), nullptr);
    EXPECT_EQ(Spot::list_spots(doc()).size(), 1u);
    EXPECT_TRUE(Spot::get_spot_name(second));

    EXPECT_EQ(Spot::find_spot(doc(), "PANTONE 185 C"), nullptr);
    EXPECT_EQ(Spot::find_spot(doc(), "PANTONE 300 C"), second);
}
