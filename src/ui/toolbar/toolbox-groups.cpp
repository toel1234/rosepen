// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ToolboxGroups - Corel-style toolbox group model.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/toolbar/toolbox-groups.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include <glibmm/fileutils.h>
#include <glibmm/i18n.h>
#include <glibmm/keyfile.h>

#include "io/resource.h"
#include "preferences.h"
#include "ui/tools/tool-data.h"

namespace Inkscape::UI::Toolbar {

namespace {

Glib::ustring trim(Glib::ustring const &value)
{
    auto const first = value.find_first_not_of(" \t\r\n");
    if (first == Glib::ustring::npos) {
        return {};
    }
    auto const last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

/**
 * Semicolon-separated list from a key file. Missing keys and malformed lists
 * give an empty list; the caller reports the resulting structural problems.
 */
std::vector<Glib::ustring> read_list(Glib::KeyFile const &keyfile,
                                     Glib::ustring const &group,
                                     Glib::ustring const &key)
{
    std::vector<Glib::ustring> result;
    if (!keyfile.has_group(group) || !keyfile.has_key(group, key)) {
        return result;
    }

    try {
        for (auto const &item : keyfile.get_string_list(group, key)) {
            auto const trimmed = trim(item);
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
        }
    } catch (Glib::Error const &) {
        result.clear();
    }

    return result;
}

Glib::ustring pref_path(Glib::ustring const &group_id)
{
    return "/toolbox/flyouts/" + group_id + "/active";
}

bool is_member(ToolGroup const &group, Glib::ustring const &tool)
{
    return std::find(group.tools.begin(), group.tools.end(), tool) != group.tools.end();
}

} // namespace

bool use_flyout_layout()
{
    return Preferences::get()->getString(toolbox_layout_path, "corel") != "classic";
}

std::vector<ToolGroup> parse_toolbox_groups(std::string const &keyfile_data,
                                            std::vector<Glib::ustring> &errors)
{
    auto const keyfile = Glib::KeyFile::create();

    try {
        if (!keyfile->load_from_data(keyfile_data)) {
            errors.push_back(_("toolbox groups: the group file is malformed"));
            return {};
        }
    } catch (Glib::Error const &error) {
        errors.push_back(Glib::ustring::compose(_("toolbox groups: the group file is malformed: %1"),
                                                error.what()));
        return {};
    }

    auto const &tool_data = get_tool_data();
    auto const is_known_tool = [&tool_data](Glib::ustring const &tool) {
        return tool_data.find(tool) != tool_data.end();
    };

    std::set<Glib::ustring> seen;
    std::vector<Glib::ustring> grouped;
    std::vector<ToolGroup> defined;

    for (auto const &group_name : keyfile->get_groups()) {
        if (group_name.compare(0, 6, "group:") != 0) {
            continue;
        }

        auto const id = group_name.substr(6);
        if (id.empty()) {
            errors.push_back(_("toolbox groups: a group has an empty id"));
            continue;
        }

        ToolGroup group{id, {}};
        for (auto const &tool : read_list(*keyfile, group_name, "tools")) {
            if (!is_known_tool(tool)) {
                errors.push_back(Glib::ustring::compose(
                    _("toolbox group '%1' names unknown tool '%2'"), id, tool));
                continue;
            }
            if (!seen.insert(tool).second) {
                errors.push_back(Glib::ustring::compose(
                    _("tool '%1' is listed more than once"), tool));
                continue;
            }
            group.tools.push_back(tool);
            grouped.push_back(tool);
        }

        if (group.tools.empty()) {
            errors.push_back(Glib::ustring::compose(
                _("toolbox group '%1' has no tools"), id));
            continue;
        }

        defined.push_back(std::move(group));
    }

    std::vector<Glib::ustring> excluded;
    for (auto const &tool : read_list(*keyfile, "toolbox", "not-in-toolbox")) {
        if (!is_known_tool(tool)) {
            errors.push_back(Glib::ustring::compose(
                _("not-in-toolbox names unknown tool '%1'"), tool));
            continue;
        }
        if (!seen.insert(tool).second) {
            errors.push_back(Glib::ustring::compose(
                _("tool '%1' is listed more than once"), tool));
            continue;
        }
        excluded.push_back(tool);
    }

    auto const order = read_list(*keyfile, "toolbox", "order");
    if (order.empty()) {
        errors.push_back(_("toolbox groups: [toolbox] has no 'order' list"));
        return {};
    }

    std::map<Glib::ustring, std::size_t> defined_by_id;
    for (std::size_t i = 0; i < defined.size(); ++i) {
        defined_by_id.emplace(defined[i].id, i);
    }

    std::vector<ToolGroup> result;
    std::set<Glib::ustring> ordered;
    for (auto const &id : order) {
        auto const position = defined_by_id.find(id);
        if (position == defined_by_id.end()) {
            errors.push_back(Glib::ustring::compose(
                _("order names undefined group '%1'"), id));
            continue;
        }
        if (!ordered.insert(id).second) {
            errors.push_back(Glib::ustring::compose(
                _("order lists group '%1' more than once"), id));
            continue;
        }
        result.push_back(defined[position->second]);
    }

    for (auto const &group : defined) {
        if (ordered.find(group.id) == ordered.end()) {
            errors.push_back(Glib::ustring::compose(
                _("group '%1' is not listed in order"), group.id));
        }
    }

    std::set<Glib::ustring> covered(grouped.begin(), grouped.end());
    covered.insert(excluded.begin(), excluded.end());
    for (auto const &entry : tool_data) {
        if (covered.find(entry.first) == covered.end()) {
            errors.push_back(Glib::ustring::compose(
                _("tool '%1' is neither grouped nor in not-in-toolbox"), entry.first));
        }
    }

    return result;
}

std::vector<ToolGroup> const &toolbox_groups()
{
    static std::vector<ToolGroup> const groups = [] {
        std::string const path = IO::Resource::get_filename(IO::Resource::UIS,
                                                            "toolbox-groups.ini");
        if (path.empty()) {
            g_warning("Toolbox groups: toolbox-groups.ini not found; using the classic toolbox");
            return std::vector<ToolGroup>{};
        }

        std::string data;
        try {
            data = Glib::file_get_contents(path);
        } catch (Glib::Error const &error) {
            g_warning("Toolbox groups: cannot read '%s': %s", path.c_str(), error.what());
            return std::vector<ToolGroup>{};
        }

        std::vector<Glib::ustring> errors;
        auto result = parse_toolbox_groups(data, errors);
        for (auto const &error : errors) {
            g_warning("Toolbox groups: %s", error.c_str());
        }
        if (result.empty()) {
            g_warning("Toolbox groups: no usable groups; using the classic toolbox");
        }
        return result;
    }();

    return groups;
}

Glib::ustring group_label(Glib::ustring const &id)
{
    static std::map<Glib::ustring, char const *> const labels = {
        {"pick",           N_("Pick")},
        {"shape",          N_("Shape")},
        {"crop",           N_("Crop")},
        {"zoom",           N_("Zoom")},
        {"curve",          N_("Curve")},
        {"artistic-media", N_("Artistic media")},
        {"rectangle",      N_("Rectangle")},
        {"ellipse",        N_("Ellipse")},
        {"object",         N_("Object")},
        {"text",           N_("Text")},
        {"dimension",      N_("Dimension")},
        {"effects",        N_("Effects")},
        {"eyedropper",     N_("Eyedropper")},
        {"fill",           N_("Fill")},
    };

    auto const label = labels.find(id);
    return label == labels.end() ? id : _(label->second);
}

std::optional<std::size_t> group_index_for_tool(std::vector<ToolGroup> const &groups,
                                                Glib::ustring const &tool)
{
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (is_member(groups[i], tool)) {
            return i;
        }
    }
    return std::nullopt;
}

Glib::ustring active_tool(ToolGroup const &group,
                          std::function<bool(Glib::ustring const &)> const &visible)
{
    auto const is_visible = [&visible](Glib::ustring const &tool) {
        return !visible || visible(tool);
    };

    auto const remembered = Preferences::get()->getString(pref_path(group.id));
    if (!remembered.empty() && is_member(group, remembered) && is_visible(remembered)) {
        return remembered;
    }

    for (auto const &tool : group.tools) {
        if (is_visible(tool)) {
            return tool;
        }
    }

    return {};
}

void set_active_tool(ToolGroup const &group, Glib::ustring const &tool)
{
    if (!is_member(group, tool)) {
        return;
    }

    Preferences::get()->setString(pref_path(group.id), tool);
}

} // namespace Inkscape::UI::Toolbar
