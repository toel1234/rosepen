// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for color objects.
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include "colors/cms/profile.h"
#include "colors/cms/system.h"
#include "colors/cms/transform-surface.h"
#include "colors/spaces/enum.h"
#include "preferences.h"

static std::string icc_dir = INKSCAPE_TESTS_DIR "/data/colors/";
static std::string grb_profile = INKSCAPE_TESTS_DIR "/data/colors/SwappedRedAndGreen.icc";
static std::string display_profile = INKSCAPE_TESTS_DIR "/data/colors/display.icc";

using namespace Inkscape::Colors;
using namespace Inkscape::Colors::CMS;

namespace {

// ================= CMS::System ================= //

class ColorCmsSystem : public ::testing::Test
{
protected:
    void SetUp() override
    {
        cms = &CMS::System::get();
        cms->clearDirectoryPaths();
        cms->addDirectoryPath(icc_dir, false);
        cms->refreshProfiles();

        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        prefs->setString("/options/displayprofile/uri", display_profile);
        prefs->setBool("/options/displayprofile/enabled", true);
    }
    void TearDown() override
    {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        prefs->setBool("/options/softproof/enable", false);
        prefs->setBool("/options/softproof/gamutwarn", false);
        prefs->setString("/options/softproof/gamutcolor", "#808080");
        prefs->setInt("/options/softproof/intent", 0);
        prefs->setBool("/options/softproof/bpc", false);
        prefs->setString("/options/softproof/uri", "");
    }

    std::shared_ptr<Inkscape::Colors::CMS::Profile> cmykProfile()
    {
        return cms->getProfile("Artifex CMYK SWOP Profile");
    }
    Inkscape::Colors::CMS::System *cms = nullptr;
};

TEST_F(ColorCmsSystem, getDirectoryPaths)
{
    ASSERT_EQ(cms->getDirectoryPaths().size(), 1);
    ASSERT_EQ(cms->getDirectoryPaths()[0].first, icc_dir);
}

TEST_F(ColorCmsSystem, addDirectoryPath)
{
    cms->clearDirectoryPaths();
    cms->addDirectoryPath("nope", false);
    cms->addDirectoryPath("yep", true);
    ASSERT_EQ(cms->getDirectoryPaths().size(), 2);
    ASSERT_EQ(cms->getDirectoryPaths()[0].first, "nope");
    ASSERT_EQ(cms->getDirectoryPaths()[1].first, "yep");
}

TEST_F(ColorCmsSystem, clearDirectoryPaths)
{
    cms->clearDirectoryPaths();
    ASSERT_GE(cms->getDirectoryPaths().size(), 2);
}

TEST_F(ColorCmsSystem, getProfiles)
{
    auto profiles = cms->getProfiles();
    ASSERT_EQ(profiles.size(), 3);

    ASSERT_EQ(profiles[0]->getName(), "Artifex CMYK SWOP Profile");
    ASSERT_EQ(profiles[1]->getName(), "C.icc");
    ASSERT_EQ(profiles[2]->getName(), "Swapped Red and Green");
}

TEST_F(ColorCmsSystem, getProfileByName)
{
    auto profile = cms->getProfile("Swapped Red and Green");
    ASSERT_TRUE(profile);
    ASSERT_EQ(profile->getPath(), grb_profile);
}

TEST_F(ColorCmsSystem, getProfileByID)
{
    auto profile = cms->getProfile("f9eda5a42a222a28f0adb82a938eeb0e");
    ASSERT_TRUE(profile);
    ASSERT_EQ(profile->getName(), "Swapped Red and Green");
}

TEST_F(ColorCmsSystem, getProfileByPath)
{
    auto profile = cms->getProfile(grb_profile);
    ASSERT_TRUE(profile);
    ASSERT_EQ(profile->getId(), "f9eda5a42a222a28f0adb82a938eeb0e");
}

TEST_F(ColorCmsSystem, getDisplayProfiles)
{
    auto profiles = cms->getDisplayProfiles();
    ASSERT_EQ(profiles.size(), 1);
    ASSERT_EQ(profiles[0]->getName(), "C.icc");
}

TEST_F(ColorCmsSystem, getDisplayProfile)
{
    bool updated = false;
    auto profile = cms->getDisplayProfile(updated);
    ASSERT_TRUE(updated);
    ASSERT_TRUE(profile);
    ASSERT_EQ(profile->getName(), "C.icc");
}

TEST_F(ColorCmsSystem, getOutputProfiles)
{
    auto profiles = cms->getOutputProfiles();
    ASSERT_EQ(profiles.size(), 1);
    ASSERT_EQ(profiles[0]->getName(), "Artifex CMYK SWOP Profile");
}

TEST_F(ColorCmsSystem, getLinearRgbProfile)
{
    auto linear = CMS::Profile::create_linearrgb();
    EXPECT_EQ(linear->getName(), "linearRGB identity with D65");
}

TEST_F(ColorCmsSystem, refreshProfiles)
{
    ASSERT_EQ(cms->getDirectoryPaths().size(), 1);
    cms->clearDirectoryPaths();
    cms->refreshProfiles();
    ASSERT_GE(cms->getDirectoryPaths().size(), 5);
}

// ================= Soft proofing ================= //

TEST_F(ColorCmsSystem, selectProofProfileDisabled)
{
    auto document_output = cmykProfile();
    ASSERT_TRUE(document_output);

    ProofSettings settings;
    settings.enabled = false;
    settings.uri = grb_profile;

    auto by_uri = [](std::string const &) -> std::shared_ptr<Profile> { return nullptr; };
    EXPECT_EQ(System::selectProofProfile(settings, document_output, by_uri), nullptr);
}

TEST_F(ColorCmsSystem, selectProofProfileDocumentWins)
{
    auto document_output = cmykProfile();
    auto uri_profile = cms->getProfile("Swapped Red and Green");
    ASSERT_TRUE(document_output);
    ASSERT_TRUE(uri_profile);

    ProofSettings settings;
    settings.enabled = true;
    settings.uri = grb_profile;

    auto by_uri = [&](std::string const &uri) -> std::shared_ptr<Profile> {
        return uri == grb_profile ? uri_profile : nullptr;
    };
    EXPECT_EQ(System::selectProofProfile(settings, document_output, by_uri), document_output);
}

TEST_F(ColorCmsSystem, selectProofProfileFromUri)
{
    auto uri_profile = cmykProfile();
    ASSERT_TRUE(uri_profile);

    ProofSettings settings;
    settings.enabled = true;
    settings.uri = uri_profile->getPath();

    auto by_uri = [&](std::string const &uri) -> std::shared_ptr<Profile> {
        return uri == settings.uri ? uri_profile : nullptr;
    };
    EXPECT_EQ(System::selectProofProfile(settings, nullptr, by_uri), uri_profile);
}

TEST_F(ColorCmsSystem, selectProofProfileUnresolved)
{
    ProofSettings settings;
    settings.enabled = true;
    settings.uri = "/does/not/exist.icc";

    auto by_uri = [](std::string const &) -> std::shared_ptr<Profile> { return nullptr; };
    EXPECT_EQ(System::selectProofProfile(settings, nullptr, by_uri), nullptr);
}

TEST_F(ColorCmsSystem, getProofTransform)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/softproof/enable", true);

    auto document_output = cmykProfile();
    ASSERT_TRUE(document_output);

    auto proof = cms->getProofTransform(document_output);
    ASSERT_TRUE(proof);
    EXPECT_TRUE(proof->isValid());

    auto display = cms->getDisplayTransform();
    ASSERT_TRUE(display);
    EXPECT_NE(proof.get(), display.get());
}

TEST_F(ColorCmsSystem, getProofTransformWithoutProofIsDisplay)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/softproof/enable", false);

    auto display = cms->getDisplayTransform();
    auto no_proof = cms->getProofTransform(cmykProfile());
    EXPECT_EQ(no_proof.get(), display.get());
}

TEST_F(ColorCmsSystem, getProofTransformGamutAlarm)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/softproof/enable", true);
    prefs->setBool("/options/softproof/gamutwarn", true);
    prefs->setString("/options/softproof/gamutcolor", "#00ff00");

    auto document_output = cmykProfile();
    ASSERT_TRUE(document_output);

    auto proof = cms->getProofTransform(document_output);
    ASSERT_TRUE(proof);
    ASSERT_TRUE(proof->isValid());

    // Pure blue is outside the CMYK output gamut -> alarm colour.
    uint8_t const input[4] = {0, 0, 255, 255};
    uint8_t alarm[4] = {};
    proof->do_transform(1, 1, input, alarm);
    EXPECT_NEAR(alarm[0], 0, 1);
    EXPECT_NEAR(alarm[1], 255, 1);
    EXPECT_NEAR(alarm[2], 0, 1);
    EXPECT_NEAR(alarm[3], 255, 1);

    prefs->setBool("/options/softproof/gamutwarn", false);
    auto proof_plain = cms->getProofTransform(document_output);
    ASSERT_TRUE(proof_plain);
    uint8_t plain[4] = {};
    proof_plain->do_transform(1, 1, input, plain);
    EXPECT_FALSE(plain[0] == 0 && plain[1] == 255 && plain[2] == 0);
}

TEST_F(ColorCmsSystem, getProofTransformCache)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->setBool("/options/softproof/enable", true);

    auto document_output = cmykProfile();
    ASSERT_TRUE(document_output);

    auto first = cms->getProofTransform(document_output);
    auto second = cms->getProofTransform(document_output);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.get(), second.get());

    prefs->setInt("/options/softproof/intent", 1);
    auto third = cms->getProofTransform(document_output);
    ASSERT_TRUE(third);
    EXPECT_NE(first.get(), third.get());
}

} // namespace

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
