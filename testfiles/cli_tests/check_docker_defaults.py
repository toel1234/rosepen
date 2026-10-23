#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Rosepen 04-03: validate share/ui/default-dialog-state.ini against the real
dialog registry.

Usage: python3 -I check_docker_defaults.py <default-dialog-state.ini> <dialog-data.cpp> <dialog-container.cpp>

Every docked or transient dialog id in the shipped defaults must exist in the
DialogData registry (dialog-data.cpp) AND in the DialogContainer factory
(dialog-container.cpp); the transient count must match the dialogsN keys; no
id may be docked and transient or docked twice; and the first notebook must
hold the Rosepen classic-style default set in the shipped order.
"""

import configparser
import re
import sys

CLASSIC_SET = ["ObjectProperties", "Objects", "FillStroke", "Pages"]

DATA_ROW = re.compile(r'^\s*\{"(\w+)",\s*_\(', re.MULTILINE)
FACTORY_ROW = re.compile(r'dialog_type == "(\w+)"')
NOTEBOOK_KEY = re.compile(r'^Notebook(\d+)Dialogs$')
TRANSIENT_KEY = re.compile(r'^dialogs(\d+)$')
WINDOW_COLUMN = re.compile(r'^Window\d+Column\d+$')


def ids_in(value):
    """Split a ';'-separated dialog list, dropping empties."""
    return [tok.strip() for tok in value.split(";") if tok.strip()]


def main(argv):
    if len(argv) != 4:
        print(f"usage: {argv[0]} <default-dialog-state.ini> <dialog-data.cpp> <dialog-container.cpp>",
              file=sys.stderr)
        return 2

    ini_path, data_path, container_path = argv[1:4]

    with open(data_path, encoding="utf-8") as f:
        registered = set(DATA_ROW.findall(f.read()))
    with open(container_path, encoding="utf-8") as f:
        produced = set(FACTORY_ROW.findall(f.read()))
    known = registered & produced

    cp = configparser.ConfigParser(interpolation=None, strict=False,
                                   comment_prefixes=("#",))
    cp.optionxform = str  # keep key case: Notebook0Dialogs etc.
    cp.read(ini_path, encoding="utf-8")

    docked = []  # (group, key, [ids])
    for section in cp.sections():
        if not WINDOW_COLUMN.match(section):
            continue
        for key in cp.options(section):
            if NOTEBOOK_KEY.match(key):
                docked.append((section, key, ids_in(cp.get(section, key))))

    transient = []  # (key, [ids])
    if cp.has_section("transient"):
        for key in cp.options("transient"):
            if TRANSIENT_KEY.match(key):
                transient.append((key, ids_in(cp.get("transient", key))))
    try:
        declared_count = cp.getint("transient", "count")
    except (configparser.NoSectionError, configparser.NoOptionError, ValueError):
        declared_count = None

    flat_docked = [did for _, _, ids in docked for did in ids]
    flat_transient = [did for _, ids in transient for did in ids]

    failed = False

    def check(name, ok, reason=""):
        nonlocal failed
        if ok:
            print(f"CHECK {name} OK")
        else:
            failed = True
            print(f"CHECK {name} FAIL: {reason}")

    # docked-known: every docked id is in the registry AND the factory.
    unknown = [did for did in flat_docked if did not in known]
    check("docked-known", not unknown,
          f"ids not in dialog-data.cpp or dialog-container.cpp: {sorted(set(unknown)) or unknown}")

    # transient-known: every transient id is known.
    unknown_t = [did for did in flat_transient if did not in known]
    check("transient-known", not unknown_t,
          f"ids not in dialog-data.cpp or dialog-container.cpp: {sorted(set(unknown_t)) or unknown_t}")

    # transient-count: [transient] count equals the number of dialogsN keys.
    check("transient-count", declared_count == len(transient),
          f"count={declared_count} but {len(transient)} dialogsN keys")

    # no-overlap: no id is both docked and transient.
    overlap = sorted(set(flat_docked) & set(flat_transient))
    check("no-overlap", not overlap, f"docked and transient: {overlap}")

    # no-duplicate: no id appears twice in the docked lists.
    dupes = sorted({did for did in flat_docked if flat_docked.count(did) > 1})
    check("no-duplicate", not dupes, f"docked more than once: {dupes}")

    # classic-set: the first notebook holds exactly the classic default set, in order.
    first = None
    for section, key, ids in docked:
        if section == "Window0Column0" and key == "Notebook0Dialogs":
            first = ids
            break
    check("classic-set", first == CLASSIC_SET,
          f"Window0Column0/Notebook0Dialogs={first!r}, expected {CLASSIC_SET!r}")

    verdict = "DOCKER_DEFAULTS_FAIL" if failed else "DOCKER_DEFAULTS_OK"
    print(f"{verdict} docked={len(flat_docked)} transient={len(flat_transient)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
