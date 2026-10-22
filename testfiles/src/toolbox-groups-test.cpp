// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the pure toolbox group model.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/toolbar/toolbox-groups.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "preferences.h"

namespace {

using Inkscape::UI::Toolbar::ToolGroup;
using Inkscape::UI::Toolbar::active_tool;
using Inkscape::UI::Toolbar::group_index_for_tool;
using Inkscape::UI::Toolbar::group_label;
using Inkscape::UI::Toolbar::parse_toolbox_groups;
using Inkscape::UI::Toolbar::set_active_tool;
using Inkscape::UI::Toolbar::toolbox_layout_path;
using Inkscape::UI::Toolbar::use_flyout_layout;

std::string read_shipped_ini()
{
    std::ifstream file(INKSCAPE_SHARE_DIR "/ui/toolbox-groups.ini");
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string replace_once(std::string text, std::string const &from, std::string const &to)
{
    auto const position = text.find(from);
    EXPECT_NE(position, std::string::npos) << "fixture text not found: " << from;
    if (position != std::string::npos) {
        text.replace(position, from.size(), to);
    }
    return text;
}

ToolGroup const *find_group(std::vector<ToolGroup> const &groups, Glib::ustring const &id)
{
    for (auto const &group : groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

class ToolboxGroupsTest : public ::testing::Test
{
protected:
    using Transaction = decltype(std::declval<Inkscape::Preferences &>().temporaryPreferences());

    Inkscape::Preferences *prefs = nullptr;
    // Every pref written by a test is rolled back (and the entry cache
    // cleared) in TearDown. Preferences::remove() is not enough: removing
    // the /toolbox/flyouts subtree leaves cached leaf entries behind, and an
    // attribute on a node with children (/toolbox/layout) is not removed.
    std::unique_ptr<Transaction> transaction;

    void SetUp() override
    {
        prefs = Inkscape::Preferences::get();
        ASSERT_TRUE(prefs->getString(toolbox_layout_path).empty());
        ASSERT_TRUE(prefs->getString("/toolbox/flyouts/shape/active").empty());
        transaction.reset(new auto(prefs->temporaryPreferences()));
    }

    void TearDown() override
    {
        transaction.reset();
        prefs = nullptr;
    }
};

} // namespace

TEST_F(ToolboxGroupsTest, shippedIniParses)
{
    auto const data = read_shipped_ini();
    ASSERT_FALSE(data.empty());

    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(data, errors);

    for (auto const &error : errors) {
        ADD_FAILURE() << error;
    }
    ASSERT_EQ(groups.size(), 14u);

    std::vector<Glib::ustring> const expected_ids = {
        "pick", "shape", "crop", "zoom", "curve", "artistic-media",
        "rectangle", "ellipse", "object", "text", "dimension", "effects",
        "eyedropper", "fill",
    };

    std::vector<Glib::ustring> ids;
    std::size_t tools = 0;
    for (auto const &group : groups) {
        ids.push_back(group.id);
        tools += group.tools.size();
    }

    EXPECT_EQ(ids, expected_ids);
    EXPECT_EQ(tools, 26u);
    EXPECT_EQ(groups.front().id, "pick");
    EXPECT_EQ(groups.front().tools, (std::vector<Glib::ustring>{"Select"}));
}

TEST_F(ToolboxGroupsTest, unknownToolIsDropped)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(
        replace_once(read_shipped_ini(), "tools=Select", "tools=Select;NotATool"), errors);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().find("NotATool") != Glib::ustring::npos);
    ASSERT_EQ(groups.size(), 14u);
    EXPECT_EQ(groups.front().tools, (std::vector<Glib::ustring>{"Select"}));
}

TEST_F(ToolboxGroupsTest, duplicateToolIsDropped)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(
        replace_once(read_shipped_ini(), "tools=Node;Tweak;Booleans",
                     "tools=Node;Tweak;Booleans;Select"), errors);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().find("Select") != Glib::ustring::npos);
    ASSERT_EQ(groups.size(), 14u);
    EXPECT_EQ(groups.front().tools, (std::vector<Glib::ustring>{"Select"}));
    auto const *shape = find_group(groups, "shape");
    ASSERT_NE(shape, nullptr);
    EXPECT_EQ(shape->tools, (std::vector<Glib::ustring>{"Node", "Tweak", "Booleans"}));
}

TEST_F(ToolboxGroupsTest, emptyGroupIsDropped)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(read_shipped_ini() + "\n[group:extra]\ntools=\n",
                                             errors);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().find("extra") != Glib::ustring::npos);
    EXPECT_EQ(groups.size(), 14u);
    EXPECT_EQ(find_group(groups, "extra"), nullptr);
}

TEST_F(ToolboxGroupsTest, undefinedOrderIdIsDropped)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(
        replace_once(read_shipped_ini(), "order=pick;", "order=nope;pick;"), errors);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().find("nope") != Glib::ustring::npos);
    EXPECT_EQ(groups.size(), 14u);
}

TEST_F(ToolboxGroupsTest, unorderedGroupIsDropped)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(
        replace_once(read_shipped_ini(), "order=pick;shape;", "order=shape;"), errors);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().find("pick") != Glib::ustring::npos);
    EXPECT_EQ(groups.size(), 13u);
    EXPECT_EQ(find_group(groups, "pick"), nullptr);
    ASSERT_FALSE(groups.empty());
    EXPECT_EQ(groups.front().id, "shape");
}

TEST_F(ToolboxGroupsTest, malformedKeyFileIsEmptyAndQuiet)
{
    std::vector<Glib::ustring> errors;
    std::vector<ToolGroup> groups;

    EXPECT_NO_THROW(groups = parse_toolbox_groups("this is not a key file", errors));

    EXPECT_TRUE(groups.empty());
    EXPECT_EQ(errors.size(), 1u);
}

TEST_F(ToolboxGroupsTest, unaccountedToolIsReported)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(
        replace_once(read_shipped_ini(), "not-in-toolbox=Marker;Picker",
                     "not-in-toolbox=Marker"), errors);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().find("Picker") != Glib::ustring::npos);
    EXPECT_EQ(groups.size(), 14u);
}

TEST_F(ToolboxGroupsTest, groupIndexForTool)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(read_shipped_ini(), errors);
    ASSERT_TRUE(errors.empty());

    auto const pen = group_index_for_tool(groups, "Pen");
    ASSERT_TRUE(pen.has_value());
    EXPECT_EQ(groups[*pen].id, "curve");

    auto const select = group_index_for_tool(groups, "Select");
    ASSERT_TRUE(select.has_value());
    EXPECT_EQ(groups[*select].id, "pick");

    EXPECT_FALSE(group_index_for_tool(groups, "Marker").has_value());
    EXPECT_FALSE(group_index_for_tool(groups, "NoSuchTool").has_value());
}

TEST_F(ToolboxGroupsTest, activeToolDefaultsAndRoundTrips)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(read_shipped_ini(), errors);
    ASSERT_TRUE(errors.empty());

    auto const *shape = find_group(groups, "shape");
    ASSERT_NE(shape, nullptr);

    EXPECT_EQ(active_tool(*shape), "Node");

    prefs->setString("/toolbox/flyouts/shape/active", "Booleans");
    EXPECT_EQ(active_tool(*shape), "Booleans");

    set_active_tool(*shape, "Tweak");
    EXPECT_EQ(active_tool(*shape), "Tweak");
    EXPECT_EQ(prefs->getString("/toolbox/flyouts/shape/active"), "Tweak");

    set_active_tool(*shape, "Select");
    EXPECT_EQ(active_tool(*shape), "Tweak");

    prefs->setString("/toolbox/flyouts/shape/active", "garbage");
    EXPECT_EQ(active_tool(*shape), "Node");
}

TEST_F(ToolboxGroupsTest, activeToolHonoursVisibility)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(read_shipped_ini(), errors);
    ASSERT_TRUE(errors.empty());

    auto const *shape = find_group(groups, "shape");
    ASSERT_NE(shape, nullptr);

    prefs->setString("/toolbox/flyouts/shape/active", "Booleans");

    auto const without_booleans = [](Glib::ustring const &tool) {
        return tool != "Booleans";
    };
    EXPECT_EQ(active_tool(*shape, without_booleans), "Node");

    auto const only_tweak = [](Glib::ustring const &tool) {
        return tool == "Tweak";
    };
    EXPECT_EQ(active_tool(*shape, only_tweak), "Tweak");

    auto const nothing_visible = [](Glib::ustring const &) {
        return false;
    };
    EXPECT_EQ(active_tool(*shape, nothing_visible), "");
}

TEST_F(ToolboxGroupsTest, everyShippedGroupHasALabel)
{
    std::vector<Glib::ustring> errors;
    auto const groups = parse_toolbox_groups(read_shipped_ini(), errors);
    ASSERT_TRUE(errors.empty());

    for (auto const &group : groups) {
        auto const label = group_label(group.id);
        EXPECT_FALSE(label.empty()) << group.id;
        EXPECT_NE(label, group.id) << group.id;
    }

    EXPECT_EQ(group_label("no-such-group"), "no-such-group");
}

TEST_F(ToolboxGroupsTest, flyoutLayoutPreference)
{
    EXPECT_TRUE(use_flyout_layout());

    prefs->setString(toolbox_layout_path, "classic");
    EXPECT_FALSE(use_flyout_layout());

    prefs->setString(toolbox_layout_path, "corel");
    EXPECT_TRUE(use_flyout_layout());
}
