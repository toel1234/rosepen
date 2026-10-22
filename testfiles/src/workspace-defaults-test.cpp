// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure Corel-style workspace model.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/workspace-defaults.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "preferences.h"

namespace {

using Inkscape::UI::Workspace::PalettePlacement;
using Inkscape::UI::Workspace::PrefValue;
using Inkscape::UI::Workspace::apply_workspace;
using Inkscape::UI::Workspace::apply_workspace_revision;
using Inkscape::UI::Workspace::commandbar_ui_file;
using Inkscape::UI::Workspace::workspace_revision;
using Inkscape::UI::Workspace::workspace_revision_pref;
using Inkscape::UI::Workspace::default_palette_lines;
using Inkscape::UI::Workspace::inkscape_workspace;
using Inkscape::UI::Workspace::is_default_keys_file;
using Inkscape::UI::Workspace::menubar_ui_file;
using Inkscape::UI::Workspace::palette_lines_key;
using Inkscape::UI::Workspace::palette_placement;
using Inkscape::UI::Workspace::rosepen_keys_file;
using Inkscape::UI::Workspace::rosepen_workspace;

class WorkspaceDefaultsTest : public ::testing::Test
{
protected:
    using Transaction = decltype(std::declval<Inkscape::Preferences &>().temporaryPreferences());

    Inkscape::Preferences *prefs = nullptr;
    // Every pref written by a test is rolled back in TearDown.
    std::unique_ptr<Transaction> transaction;

    void SetUp() override
    {
        prefs = Inkscape::Preferences::get();
        transaction.reset(new auto(prefs->temporaryPreferences()));
    }

    void TearDown() override
    {
        transaction.reset();
        prefs = nullptr;
    }
};

} // namespace

TEST(WorkspaceDefaults, menubarUiFile)
{
    EXPECT_EQ(menubar_ui_file(""), "menus-corel.ui");
    EXPECT_EQ(menubar_ui_file("corel"), "menus-corel.ui");
    EXPECT_EQ(menubar_ui_file("bogus"), "menus-corel.ui");
    EXPECT_EQ(menubar_ui_file("classic"), "menus.ui");
}

TEST(WorkspaceDefaults, commandbarUiFile)
{
    EXPECT_EQ(commandbar_ui_file("corel"), "toolbar-commands-corel.ui");
    EXPECT_EQ(commandbar_ui_file(""), "toolbar-commands-corel.ui");
    EXPECT_EQ(commandbar_ui_file("classic"), "toolbar-commands.ui");
}

TEST(WorkspaceDefaults, palettePlacement)
{
    EXPECT_EQ(palette_placement(""), PalettePlacement::Bottom);
    EXPECT_EQ(palette_placement("bottom"), PalettePlacement::Bottom);
    EXPECT_EQ(palette_placement("x"), PalettePlacement::Bottom);
    EXPECT_EQ(palette_placement("right"), PalettePlacement::Right);
}

TEST(WorkspaceDefaults, paletteLinesKeyAndDefault)
{
    EXPECT_STREQ(palette_lines_key(PalettePlacement::Bottom), "rows");
    EXPECT_STREQ(palette_lines_key(PalettePlacement::Right), "columns");
    EXPECT_EQ(default_palette_lines(PalettePlacement::Bottom), 1);
    EXPECT_EQ(default_palette_lines(PalettePlacement::Right), 1);
}

TEST(WorkspaceDefaults, isDefaultKeysFile)
{
    EXPECT_TRUE(is_default_keys_file(""));
    EXPECT_TRUE(is_default_keys_file("inkscape.xml"));
    EXPECT_TRUE(is_default_keys_file("default.xml"));
    EXPECT_TRUE(is_default_keys_file(rosepen_keys_file));
    EXPECT_FALSE(is_default_keys_file("corel-draw-x8.xml"));
}

TEST(WorkspaceDefaults, presetParity)
{
    auto const rosepen = rosepen_workspace();
    auto const inkscape = inkscape_workspace();

    ASSERT_EQ(rosepen.size(), inkscape.size());
    ASSERT_FALSE(rosepen.empty());

    // Same paths, in the same order, each exactly once.
    for (std::size_t i = 0; i < rosepen.size(); ++i) {
        EXPECT_STREQ(rosepen[i].path, inkscape[i].path) << "at index " << i;
        for (std::size_t j = 0; j < i; ++j) {
            EXPECT_STRNE(rosepen[i].path, rosepen[j].path) << "duplicate path " << rosepen[i].path;
        }
    }

    // Every value pair differs except the palette placement, which the
    // reference and upstream agree on and which both presets therefore reset
    // to Bottom.
    std::vector<std::string> equal_paths;
    for (auto const &entry : rosepen) {
        for (auto const &other : inkscape) {
            if (std::strcmp(entry.path, other.path) != 0) {
                continue;
            }
            bool const same = (entry.value == nullptr && other.value == nullptr) ||
                              (entry.value != nullptr && other.value != nullptr &&
                               std::strcmp(entry.value, other.value) == 0);
            if (same) {
                equal_paths.emplace_back(entry.path);
            }
        }
    }
    ASSERT_EQ(equal_paths.size(), 1u);
    EXPECT_STREQ(equal_paths.front().c_str(), "/embedded/swatches/placement");

    // The values carry the model's own decisions.
    for (auto const &entry : rosepen) {
        auto const *path = entry.path;
        if (std::strcmp(path, "/options/menubar/layout") == 0) {
            EXPECT_EQ(menubar_ui_file(entry.value), "menus-corel.ui");
        } else if (std::strcmp(path, "/options/kbshortcuts/shortcutfile") == 0) {
            EXPECT_STREQ(entry.value, rosepen_keys_file);
        } else if (std::strcmp(path, "/embedded/swatches/placement") == 0) {
            EXPECT_EQ(palette_placement(entry.value), PalettePlacement::Bottom);
        }
    }
    for (auto const &entry : inkscape) {
        auto const *path = entry.path;
        if (std::strcmp(path, "/options/menubar/layout") == 0) {
            EXPECT_EQ(menubar_ui_file(entry.value), "menus.ui");
        } else if (std::strcmp(path, "/embedded/swatches/placement") == 0) {
            EXPECT_EQ(palette_placement(entry.value), PalettePlacement::Bottom);
        }
    }
}

TEST_F(WorkspaceDefaultsTest, applyInkscapePreset)
{
    apply_workspace(inkscape_workspace(), true);

    EXPECT_EQ(prefs->getString("/toolbox/layout"), "classic");
    EXPECT_FALSE(prefs->getBool("/toolbox/propertybar/object-sections", true));
    EXPECT_EQ(prefs->getString("/options/menubar/layout"), "classic");
    EXPECT_EQ(prefs->getString("/embedded/swatches/placement"), "bottom");
    EXPECT_EQ(prefs->getString("/options/kbshortcuts/shortcutfile"), "inkscape.xml");
    // /window/interface_mode is encoded as a null value ("the upstream
    // screen-aspect default"). The skeleton pins interface_mode="0" and
    // Preferences::remove() cannot delete an attribute on a group with
    // children, so the preset writes the caller's computed default instead:
    // on a widescreen the command bar goes back to the side, as upstream.
    ASSERT_TRUE(prefs->getEntry("/window/interface_mode").isSet());
    EXPECT_TRUE(prefs->getBool("/window/interface_mode", false));

    EXPECT_TRUE(prefs->getBool("/theme/darkTheme", false));
    EXPECT_TRUE(prefs->getBool("/theme/symbolicIcons", false));
    EXPECT_EQ(prefs->getInt("/options/notebooklabels/value", -1), 0);
    EXPECT_EQ(prefs->getInt("/toolbox/tools/iconsize", -1), 16);
    EXPECT_EQ(prefs->getString("/theme/iconTheme"), "multicolor");

    // On a non-widescreen display the upstream default is "off".
    apply_workspace(inkscape_workspace(), false);
    EXPECT_FALSE(prefs->getBool("/window/interface_mode", true));
}

TEST_F(WorkspaceDefaultsTest, applyRosepenPreset)
{
    apply_workspace(rosepen_workspace());

    EXPECT_EQ(prefs->getString("/toolbox/layout"), "corel");
    EXPECT_TRUE(prefs->getBool("/toolbox/propertybar/object-sections", false));
    EXPECT_EQ(prefs->getString("/options/menubar/layout"), "corel");
    EXPECT_EQ(prefs->getString("/embedded/swatches/placement"), "bottom");
    EXPECT_EQ(prefs->getString("/options/kbshortcuts/shortcutfile"), rosepen_keys_file);
    ASSERT_TRUE(prefs->getEntry("/window/interface_mode").isSet());
    EXPECT_FALSE(prefs->getBool("/window/interface_mode", true));
    // The look: light theme, full-color icons, larger toolbox icons and
    // labels on the active docker tab only.
    EXPECT_FALSE(prefs->getBool("/theme/preferDarkTheme", true));
    EXPECT_FALSE(prefs->getBool("/theme/darkTheme", true));
    EXPECT_FALSE(prefs->getBool("/theme/symbolicIcons", true));
    EXPECT_EQ(prefs->getInt("/options/notebooklabels/value", -1), 1);
    EXPECT_EQ(prefs->getInt("/toolbox/tools/iconsize", -1), 24);
    EXPECT_EQ(prefs->getString("/theme/iconTheme"), "rosepen");
    EXPECT_EQ(prefs->getInt("/toolbox/controlbars/iconsize", -1), 20);
}

TEST_F(WorkspaceDefaultsTest, applyWorkspaceRemovesNullValue)
{
    // A preference written by us is really removed by the null encoding, so
    // "Use Inkscape workspace" clears the Rosepen override rather than writing
    // the opposite value.
    prefs->setString("/options/kbshortcuts/shortcutfile", "custom.xml");
    ASSERT_TRUE(prefs->getEntry("/options/kbshortcuts/shortcutfile").isSet());

    std::vector<PrefValue> const values = {{"/options/kbshortcuts/shortcutfile", nullptr}};
    apply_workspace(values);

    EXPECT_FALSE(prefs->getEntry("/options/kbshortcuts/shortcutfile").isSet());
}

TEST_F(WorkspaceDefaultsTest, revisionMigratesOldProfileOnce)
{
    // An old profile: dark theme, symbolic icons, no revision marker.
    prefs->setInt(workspace_revision_pref, 0);
    prefs->setBool("/theme/darkTheme", true);
    prefs->setBool("/theme/preferDarkTheme", true);
    prefs->setBool("/theme/symbolicIcons", true);

    EXPECT_TRUE(apply_workspace_revision());
    EXPECT_FALSE(prefs->getBool("/theme/darkTheme", true));
    EXPECT_FALSE(prefs->getBool("/theme/preferDarkTheme", true));
    EXPECT_FALSE(prefs->getBool("/theme/symbolicIcons", true));
    EXPECT_EQ(prefs->getString("/options/menubar/layout"), "corel");
    EXPECT_EQ(prefs->getInt(workspace_revision_pref, 0), workspace_revision);

    // A later user choice survives the next start.
    prefs->setBool("/theme/darkTheme", true);
    EXPECT_FALSE(apply_workspace_revision());
    EXPECT_TRUE(prefs->getBool("/theme/darkTheme", false));
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
