#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the Rosepen classic-order menubar against the upstream menubar and the
real action/dialog/tool registries.

Usage:
    check_classic_menus.py <menus.ui> <menus-classic.ui> <dialog-data.cpp>
                         <toolbox-groups.ini> <src/actions dir> [extra sources...]

Stdlib only. Prints one "CHECK <name> OK|FAIL: <reason>" line per check and a
final "CHECK_CLASSIC_MENUS_OK items=<n> top=<n>" or "CHECK_CLASSIC_MENUS_FAIL".
Exit code 0 only when every check passed.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

TOP_LABELS = ['_File', '_Edit', '_View', '_Layout', '_Object', 'Effe_cts',
              '_Bitmaps', 'Te_xt', '_Tools', '_Window', '_Help']

# 04-04 items that have no counterpart in menus.ui (validated against the
# registries instead).
NEW_ITEMS = {
    ('app.page-previous', ''),
    ('app.page-next', ''),
    ('app.page-insert', 'before'),
    ('app.page-insert', 'after'),
    ('app.page-duplicate-after', ''),
    ('app.page-remove', 'contents'),
    ('app.page-orientation', 'portrait'),
    ('app.page-orientation', 'landscape'),
    ('app.preferences', ''),
    # 05-01 PowerClip: Rosepen-only command, no classic counterpart
    ('app.object-powerclip-place', ''),
    ('app.object-powerclip-edit', ''),
    ('app.object-powerclip-finish', ''),
    ('app.object-powerclip-extract', ''),
    # 05-02 Dimension tool: Rosepen-only tool, no classic menubar counterpart
    ('win.tool-switch', 'Dimension'),
    # 05-03 Artistic Media: Rosepen-only tool and command, no classic counterpart
    ('win.tool-switch', 'ArtisticMedia'),
    ('app.artistic-break', ''),
    # 05-04 Smart Fill tool: Rosepen-only tool, no classic menubar counterpart
    ('win.tool-switch', 'SmartFill'),
    # 05-05 LiveSketch tool: Rosepen-only tool, no classic menubar counterpart
    ('win.tool-switch', 'LiveSketch'),
}

# Classic pairs intentionally unreachable from the Rosepen menubar, with reasons.
DROPPED = {
    # Edit > P_references (win.dialog-open 'Preferences') is replaced by
    # Tools > _Options... (app.preferences); both open the Preferences dialog
    # and the key profile binds Ctrl+J to app.preferences.
    ('win.dialog-open', 'Preferences'),
}

# Commands deliberately shown under two labels, with reasons.
LABEL_VARIANTS_OK = {
    # File keeps the upstream "Document Properties..." (File is copied
    # unchanged); Layout uses the reference's "Document Options...".
    ('win.dialog-open', 'DocumentProperties'),
}

# Submenus/sections that GtkBuilder ships empty; build_menu() fills them at
# runtime from effect data and the recent-files list.
RUNTIME_FILLED = {'recent-files', 'filter-menu-filters', 'effect-menu-effects'}

REQUIRED_IDS = {'menus', 'recent-files', 'filter-menu-filters', 'effect-menu-effects'}

ADD_ACTION_RE = re.compile(r"add_action\w*\s*\(\s*\"([\w-]+)\"")
SNAP_TABLE_RE = re.compile(r'"((?:app|win|doc)\.snap-[\w-]+)"')
DIALOG_ID_RE = re.compile(r'\{\s*"(\w+)",\s*_')


def attributes(el):
    return {c.get('name'): (c.text or '') for c in el if c.tag == 'attribute'}


def items(menu_el):
    """Yield (action, target, label, element) for every item in a menu tree."""
    for item in menu_el.iter('item'):
        a = attributes(item)
        yield a.get('action', ''), a.get('target', ''), a.get('label', ''), item


def pairs(menu_el):
    return {(action, target) for action, target, _, _ in items(menu_el)}


def mnemonic(label):
    idx = label.find('_')
    if idx < 0 or idx + 1 >= len(label):
        return None
    return label[idx + 1].lower()


def collect_sources(args):
    files = []
    for arg in args:
        if os.path.isdir(arg):
            files.extend(os.path.join(arg, f) for f in sorted(os.listdir(arg))
                         if f.endswith('.cpp'))
        elif os.path.exists(arg):
            files.append(arg)
    return files


def action_registry(files):
    names = set()
    for path in files:
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        names.update(ADD_ACTION_RE.findall(text))
        names.update(m.split('.', 1)[1] for m in SNAP_TABLE_RE.findall(text))
    return names


def dialog_registry(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    return set(DIALOG_ID_RE.findall(text))


def tool_registry(path):
    names = set()
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if line.startswith('tools='):
            names.update(t for t in line[len('tools='):].split(';') if t)
    return names


def main(argv):
    if len(argv) < 6:
        print(__doc__)
        return 2
    classic_path, rosepen_path, dialog_cpp, toolbox_ini = argv[1:5]
    registry_files = collect_sources(argv[5:])

    classic = ET.parse(classic_path).getroot()
    rosepen = ET.parse(rosepen_path).getroot()

    actions = action_registry(registry_files)
    dialogs = dialog_registry(dialog_cpp)
    tools = tool_registry(toolbox_ini)

    results = []

    def check(name, ok, reason=''):
        if ok:
            print(f'CHECK {name} OK')
        else:
            print(f'CHECK {name} FAIL: {reason}')
        results.append(ok)

    # required-ids -------------------------------------------------------------
    counts = {}
    for el in rosepen.iter():
        el_id = el.get('id')
        if el_id in REQUIRED_IDS:
            counts[el_id] = counts.get(el_id, 0) + 1
    rosepen_menu = rosepen.find("menu[@id='menus']")
    counts['menus'] = 1 if rosepen_menu is not None else 0
    bad = {k: counts.get(k, 0) for k in sorted(REQUIRED_IDS) if counts.get(k, 0) != 1}
    check('required-ids', not bad,
          ', '.join(f'{k} appears {v}x (want 1)' for k, v in bad.items()) or 'all ids present once')

    # top-order ----------------------------------------------------------------
    tops = rosepen_menu.findall('submenu') if rosepen_menu is not None else []
    top_labels = [attributes(sm).get('label', '') for sm in tops]
    check('top-order', top_labels == TOP_LABELS,
          f'got {top_labels}')

    # top-mnemonics ------------------------------------------------------------
    mnemos = [mnemonic(l) for l in top_labels]
    dupes = sorted({m for m in mnemos if m and mnemos.count(m) > 1})
    check('top-mnemonics', None not in mnemos and not dupes,
          f'missing or duplicate mnemonics: mnemos={mnemos} dupes={dupes}')

    # no-empty -----------------------------------------------------------------
    empties = []
    for el in rosepen_menu.iter() if rosepen_menu is not None else []:
        if el.tag not in ('submenu', 'section'):
            continue
        if el.get('id') in RUNTIME_FILLED:
            continue
        has_item = any(True for _ in el.iter('item'))
        if not has_item:
            empties.append(el.tag + (f"#{el.get('id')}" if el.get('id') else ''))
    check('no-empty', not empties, f'empty containers: {empties}')

    # items-known + dialog-targets + tool targets ------------------------------
    rosepen_items = list(items(rosepen_menu)) if rosepen_menu is not None else []
    unknown, bad_dialog, bad_tool = [], [], []
    for action, target, _, _ in rosepen_items:
        pair = (action, target)
        if pair in pairs(classic) or pair in NEW_ITEMS:
            pass
        else:
            unknown.append(f'{action}|{target}')
        base = action.split('(', 1)[0]
        if base == 'win.dialog-open' and target not in dialogs:
            bad_dialog.append(target)
        if base == 'win.tool-switch' and target not in tools:
            bad_tool.append(target)
    check('items-known', not unknown,
          f'not in menus.ui or NEW_ITEMS: {unknown}')
    check('dialog-targets', not bad_dialog,
          f'not dialog-data ids: {sorted(set(bad_dialog))}')
    check('tool-targets', not bad_tool,
          f'not toolbox tool names: {sorted(set(bad_tool))}')

    # new items must resolve to registered actions -----------------------------
    unregistered = sorted({
        base for (base, _) in NEW_ITEMS
        if (base.split('.', 1)[1] if '.' in base else base) not in actions})
    check('new-items-registered', not unregistered,
          f'NEW_ITEMS actions not registered: {unregistered}')

    # coverage -----------------------------------------------------------------
    have = pairs(rosepen_menu) if rosepen_menu is not None else set()
    missing = sorted(pairs(classic) - DROPPED - have)
    check('coverage', not missing,
          f'{len(missing)} classic pairs missing from Rosepen menu: '
          + ', '.join(f'{a}|{t}' for a, t in missing))

    # label-consistency --------------------------------------------------------
    # A command reachable from two places must read the same in both (e.g. no
    # stale "Tool Controls Bar" next to "Property Bar"); mnemonics may differ.
    seen_labels = {}
    for action, target, label, _ in rosepen_items:
        if (action, target) in LABEL_VARIANTS_OK:
            continue
        norm = label.replace('_', '').rstrip('.').strip()
        seen_labels.setdefault((action, target), set()).add(norm)
    clashes = sorted(f'{a}|{t}: {sorted(ls)}' for (a, t), ls in seen_labels.items() if len(ls) > 1)
    check('label-consistency', not clashes, f'one command, several labels: {clashes}')

    # no-vendor-name -----------------------------------------------------------
    # The banned vendor substring, spelled in pieces so this checker stays free
    # of it too; neither it nor the internal codename may appear in a label.
    vendor_mark = 'cor' 'el'
    all_labels = [c.text or '' for c in rosepen.iter('attribute') if c.get('name') == 'label']
    offenders = [label for label in all_labels
                 if vendor_mark in label.lower() or 'tracedraw' in label.lower()]
    check('no-vendor-name', not offenders, f'offending labels: {offenders}')

    n_items = len(rosepen_items)
    n_top = len(tops)
    if all(results):
        print(f'CHECK_CLASSIC_MENUS_OK items={n_items} top={n_top}')
        return 0
    print('CHECK_CLASSIC_MENUS_FAIL')
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
