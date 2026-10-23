#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Headless validator for share/ui/toolbox-groups.ini.
#
# Cross-checks the classic-style toolbox group data against the upstream tool
# toolbar (share/ui/toolbar-tool.ui) and the tool registry
# (src/ui/tools/tool-data.cpp). Standard library only; run with python3 -I.
#
# Usage: check_toolbox_groups.py <toolbox-groups.ini> <toolbar-tool.ui> <tool-data.cpp>

import configparser
import re
import sys
import xml.etree.ElementTree as ET

CHECK_LINES = []


def check(name, ok, reason=""):
    CHECK_LINES.append(bool(ok))
    if ok:
        print(f"CHECK {name} OK")
    else:
        print(f"CHECK {name} FAIL: {reason}")


def split_list(value):
    return [item.strip() for item in value.split(";") if item.strip()]


def read_ini(path):
    parser = configparser.ConfigParser(
        interpolation=None,
        delimiters=("=",),
        comment_prefixes=("#",),
    )
    # GKeyFile keys are case-sensitive; do not accept "Tools=" the C++ parser ignores.
    parser.optionxform = str
    try:
        with open(path, encoding="utf-8") as handle:
            parser.read_file(handle)
    except (OSError, configparser.Error) as error:
        print(f"CHECK config FAIL: cannot read {path}: {error}")
        return None
    return parser


def read_ui_targets(path):
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as error:
        raise SystemExit(f"cannot parse {path}: {error}")

    targets = []
    for obj in tree.iter("object"):
        action_name = None
        action_target = None
        for prop in obj.findall("property"):
            name = prop.get("name")
            if name == "action-name":
                action_name = (prop.text or "").strip()
            elif name == "action-target":
                action_target = (prop.text or "").strip()
        if action_name == "win.tool-switch" and action_target:
            targets.append(action_target.strip("'\""))
    return targets


def read_tool_names(path):
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.readlines()
    except OSError as error:
        raise SystemExit(f"cannot read {path}: {error}")

    pattern = re.compile(r'^\s*\{"(\w+)",\s*\{TOOLS_')
    names = []
    for line in lines:
        if "get_tool_msg" in line:
            break
        match = pattern.match(line)
        if match:
            names.append(match.group(1))
    return names


def main(argv):
    if len(argv) != 4:
        print(f"usage: {argv[0]} <toolbox-groups.ini> <toolbar-tool.ui> <tool-data.cpp>",
              file=sys.stderr)
        return 2

    ini_path, ui_path, tool_data_path = argv[1:]
    parser = read_ini(ini_path)
    if parser is None:
        return 1

    ui_targets = read_ui_targets(ui_path)
    tool_names = read_tool_names(tool_data_path)

    group_sections = [section for section in parser.sections() if section.startswith("group:")]
    groups = []
    for section in group_sections:
        groups.append((section[len("group:"):], split_list(parser.get(section, "tools", fallback=""))))

    try:
        order = split_list(parser.get("toolbox", "order", fallback=""))
    except configparser.Error:
        order = []
    try:
        excluded = split_list(parser.get("toolbox", "not-in-toolbox", fallback=""))
    except configparser.Error:
        excluded = []

    grouped = [tool for _, tools in groups for tool in tools]
    grouped_set = set(grouped)
    excluded_set = set(excluded)

    duplicates = sorted({tool for tool in grouped + excluded if (grouped + excluded).count(tool) > 1})
    check("duplicate-tool", not duplicates,
          f"tool(s) listed more than once: {', '.join(duplicates)}" if duplicates else "")

    empty_groups = [name for name, tools in groups if not tools]
    check("empty-group", not empty_groups,
          f"group(s) with no tools: {', '.join(empty_groups)}" if empty_groups else "")

    defined_ids = [name for name, _ in groups]
    undefined_order = [name for name in order if name not in defined_ids]
    unordered = [name for name in defined_ids if name not in order]
    repeated_order = sorted({name for name in order if order.count(name) > 1})
    order_problems = []
    if repeated_order:
        order_problems.append(f"order lists group(s) more than once: {', '.join(repeated_order)}")
    if undefined_order:
        order_problems.append(f"order names undefined group(s): {', '.join(undefined_order)}")
    if unordered:
        order_problems.append(f"defined group(s) missing from order: {', '.join(unordered)}")
    check("order-defined", not order_problems, "; ".join(order_problems))

    ui_set = set(ui_targets)
    missing_ui = sorted(grouped_set - ui_set)
    extra_ui = sorted(ui_set - grouped_set)
    ui_problems = []
    if missing_ui:
        ui_problems.append(f"grouped but not in toolbar-tool.ui: {', '.join(missing_ui)}")
    if extra_ui:
        ui_problems.append(f"in toolbar-tool.ui but not grouped: {', '.join(extra_ui)}")
    check("grouped-vs-ui", not ui_problems, "; ".join(ui_problems))

    known = set(tool_names)
    covered = grouped_set | excluded_set
    unaccounted = sorted(known - covered)
    unknown = sorted(covered - known)
    coverage_problems = []
    if unaccounted:
        coverage_problems.append(
            f"tool(s) neither grouped nor in not-in-toolbox: {', '.join(unaccounted)}")
    if unknown:
        coverage_problems.append(f"unknown tool(s): {', '.join(unknown)}")
    check("coverage", not coverage_problems, "; ".join(coverage_problems))

    overlap = sorted(grouped_set & excluded_set)
    check("disjoint", not overlap,
          f"tool(s) both grouped and not-in-toolbox: {', '.join(overlap)}" if overlap else "")

    if not all(CHECK_LINES):
        return 1

    print(f"TOOLBOX_GROUPS_OK groups={len(groups)} tools={len(grouped_set)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
