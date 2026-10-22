#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the Rosepen key profile (share/keys/rosepen.xml) against the
Inkscape profile it inherits from and the real action/dialog/tool registries.

Usage:
    check_corel_keys.py <keys dir> <rosepen.xml> <dialog-data.cpp>
                        <toolbox-groups.ini> <src/actions dir> [extra sources...]

Resolves xi:include relative to the keys dir. The effective trigger map is the
document-order replay of the binds (last binding of a trigger wins, mirroring
Shortcuts::_add_shortcut). Stdlib only. Prints one "CHECK <name> OK|FAIL:
<reason>" line per check, "OVERRIDE <trigger>: <old> -> <new>" lines for every
changed Inkscape trigger, and a final "CHECK_COREL_KEYS_OK binds=<n>
overrides=<m>" or "CHECK_COREL_KEYS_FAIL". Exit code 0 only when all passed.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

ADD_ACTION_RE = re.compile(r"add_action\w*\s*\(\s*\"([\w-]+)\"")
SNAP_TABLE_RE = re.compile(r'"((?:app|win|doc)\.snap-[\w-]+)"')
DIALOG_ID_RE = re.compile(r'\{\s*"(\w+)",\s*_')
XINCLUDE_NS = '{http://www.w3.org/2001/XInclude}include'

# The golden CorelDRAW key table: normalized trigger -> exact gaction string.
EXPECTED = {
    # Tools
    'F10': "win.tool-switch('Node')",
    'F5': "win.tool-switch('Pencil')",
    'F6': "win.tool-switch('Rect')",
    'F7': "win.tool-switch('Arc')",
    'F8': "win.tool-switch('Text')",
    'y': "win.tool-switch('Star')",
    'a': "win.tool-switch('Spiral')",
    'z': "win.tool-switch('Zoom')",
    'F2': "win.tool-switch('Zoom')",
    'g': "win.tool-switch('Gradient')",
    'x': "win.tool-switch('Eraser')",
    'm': "win.tool-switch('Mesh')",
    'i': "win.tool-switch('Calligraphic')",
    's': "win.tool-switch('Select')",
    'primary+space': "win.tool-switch('Select')",
    'F1': 'win.help-url-manual',
    # Fill and outline dialogs
    'F11': "win.dialog-open('FillStroke')",
    'shift+F11': "win.dialog-open('FillStroke')",
    'F12': "win.dialog-open('FillStroke')",
    'shift+F12': "win.dialog-open('FillStroke')",
    # View
    'F3': 'win.canvas-zoom-out',
    'F4': 'win.canvas-zoom-drawing',
    'shift+F2': 'win.canvas-zoom-selection',
    'shift+F4': 'win.canvas-zoom-page',
    'F9': 'win.view-full-screen-focus',
    'shift+F9': 'win.canvas-display-mode-cycle',
    'alt+shift+r': 'win.canvas-rulers',
    # Object
    'primary+q': 'app.object-to-path',
    'primary+shift+q': 'app.object-stroke-to-path',
    'primary+l': 'app.path-combine',
    'primary+k': 'app.path-break-apart',
    'primary+g': 'app.selection-group',
    'primary+u': 'app.selection-ungroup',
    'primary+Page_Up': 'app.selection-raise',
    'primary+Page_Down': 'app.selection-lower',
    'shift+Page_Up': 'app.selection-top',
    'primary+Home': 'app.selection-top',
    'shift+Page_Down': 'app.selection-bottom',
    'primary+End': 'app.selection-bottom',
    # Align
    'l': "app.object-align('left last')",
    'r': "app.object-align('right last')",
    't': "app.object-align('top last')",
    'b': "app.object-align('bottom last')",
    'c': "app.object-align('hcenter last')",
    'e': "app.object-align('vcenter last')",
    # Pages
    'Page_Up': 'app.page-previous',
    'KP_Page_Up': 'app.page-previous',
    'Page_Down': 'app.page-next',
    'KP_Page_Down': 'app.page-next',
    # Dockers and dialogs
    'alt+Return': "win.dialog-open('ObjectProperties')",
    'alt+F7': "win.dialog-open('Transform')",
    'alt+F8': "win.dialog-open('Transform')",
    'alt+F9': "win.dialog-open('Transform')",
    'alt+F10': "win.dialog-open('Transform')",
    'primary+shift+a': "win.dialog-open('AlignDistribute')",
    'primary+j': 'app.preferences',
    'primary+e': "win.dialog-open('Export')",
    'primary+i': 'win.document-import',
    'primary+f': "win.dialog-open('Find')",
    'primary+t': "win.dialog-open('Text')",
    'primary+shift+t': "win.dialog-open('Text')",
    'primary+F3': "win.dialog-open('Symbols')",
    # Snapping
    'primary+y': 'win.snap-grid',
    'alt+q': 'win.snap-global-toggle',
    'alt+shift+a': 'win.snap-alignment',
}

# Triggers inherited from inkscape.xml that rosepen.xml is allowed to rebind.
# Anything outside this set that changes action fails the check.
OVERRIDES = {
    'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F9', 'F11', 'F12',
    'shift+F2', 'shift+F4', 'shift+F9', 'shift+F11', 'shift+F12',
    'a', 'b', 'c', 'e', 'i', 'l', 'm', 'p', 'r', 't', 'x', 'y',
    'Page_Up', 'Page_Down', 'KP_Page_Up', 'KP_Page_Down',
    'primary+Page_Up', 'primary+Page_Down', 'shift+Page_Up', 'shift+Page_Down',
    'primary+q', 'primary+shift+q', 'primary+k', 'primary+l', 'primary+j',
    'primary+e', 'primary+y', 'primary+t', 'primary+F3', 'primary+space',
    'primary+F11',
    'alt+Return', 'alt+F7', 'alt+F8', 'alt+F9', 'alt+F10',
    'alt+q', 'alt+z', 'alt+shift+a', 'alt+shift+r',
}


def normalize(trigger):
    """'<Primary>Page_Up' -> 'primary+Page_Up'; key name kept as written."""
    mods = set()
    rest = trigger.strip()
    while rest.startswith('<') and '>' in rest:
        end = rest.index('>')
        mod = rest[1:end].strip().lower()
        mods.add('primary' if mod in ('ctrl', 'control', 'primary') else mod)
        rest = rest[end + 1:]
    # gtk_accelerator_parse() lower-cases the keyval ("L" and "<primary>Q"
    # become "l" and "<Control>q"), so letter case never makes a new trigger.
    if len(rest) == 1 and rest.isalpha():
        rest = rest.lower()
    return '+'.join(sorted(mods) + [rest])


def parse_binds(path):
    """[(normalized trigger, gaction string)] in document order."""
    root = ET.parse(path).getroot()
    out = []
    for bind in root.iter('bind'):
        action = (bind.get('gaction') or '').strip()
        keys = bind.get('keys') or ''
        for key in keys.split(','):
            key = key.strip()
            if key and action:
                out.append((normalize(key), action))
    return out


def resolve_includes(path, keys_dir, seen=None):
    """Rosepen file with xi:includes resolved against the keys dir (doc order)."""
    seen = seen or set()
    real = os.path.realpath(path)
    if real in seen:
        return []
    seen.add(real)
    root = ET.parse(path).getroot()
    files = []
    for node in root.iter():
        if node.tag == XINCLUDE_NS:
            href = node.get('href')
            if href:
                files.extend(resolve_includes(os.path.join(keys_dir, href), keys_dir, seen))
        elif node.tag == 'bind':
            action = (node.get('gaction') or '').strip()
            keys = node.get('keys') or ''
            for key in keys.split(','):
                key = key.strip()
                if key and action:
                    files.append((normalize(key), action))
    return files


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


def quoted_arg(action):
    m = re.search(r"'([^']*)'", action)
    return m.group(1) if m else None


def main(argv):
    if len(argv) < 6:
        print(__doc__)
        return 2
    keys_dir, rosepen_xml, dialog_cpp, toolbox_ini = argv[1:5]
    registry_files = collect_sources(argv[5:])

    actions = action_registry(registry_files)
    dialogs = set(DIALOG_ID_RE.findall(open(dialog_cpp, encoding='utf-8', errors='replace').read()))
    tools = set()
    for line in open(toolbox_ini, encoding='utf-8', errors='replace'):
        line = line.strip()
        if line.startswith('tools='):
            tools.update(t for t in line[len('tools='):].split(';') if t)

    inkscape_path = os.path.join(keys_dir, 'inkscape.xml')
    inkscape_binds = resolve_includes(inkscape_path, keys_dir)
    profile_binds = resolve_includes(rosepen_xml, keys_dir)
    rosepen_binds = parse_binds(rosepen_xml)

    effective = dict(profile_binds)
    inkscape_map = dict(inkscape_binds)

    results = []

    def check(name, ok, reason=''):
        print(f'CHECK {name} {"OK" if ok else "FAIL: " + reason}')
        results.append(ok)

    # name ----------------------------------------------------------------------
    root = ET.parse(rosepen_xml).getroot()
    name = root.get('name') or ''
    check('name', root.tag == 'keys' and name == 'Rosepen' and 'corel' not in name.lower(),
          f'root={root.tag} name={name!r}')

    # actions-known --------------------------------------------------------------
    unknown, bad_tool, bad_dialog = [], [], []
    for _, action in rosepen_binds:
        base = action.split('(', 1)[0]
        lookup = base.split('.', 1)[1] if '.' in base else base
        if lookup not in actions:
            unknown.append(action)
        if base == 'win.tool-switch':
            target = quoted_arg(action)
            if target not in tools:
                bad_tool.append(action)
        if base == 'win.dialog-open':
            target = quoted_arg(action)
            if target not in dialogs:
                bad_dialog.append(action)
    check('actions-known', not unknown and not bad_tool and not bad_dialog,
          f'unregistered: {unknown}; bad tool targets: {bad_tool}; bad dialog targets: {bad_dialog}')

    # no-self-duplicate -----------------------------------------------------------
    seen = {}
    dupes = []
    for trigger, action in rosepen_binds:
        # "l, L" style case aliases of one action are harmless (inkscape.xml
        # uses them too); one trigger on two different actions is not.
        if trigger in seen and seen[trigger] != action:
            dupes.append(f'{trigger}: {seen[trigger]} and {action}')
        else:
            seen[trigger] = action
    check('no-self-duplicate', not dupes, f'duplicated triggers: {dupes}')

    # expected --------------------------------------------------------------------
    wrong = []
    for trigger, action in sorted(EXPECTED.items()):
        got = effective.get(trigger)
        if got != action:
            wrong.append(f'{trigger}: want {action}, got {got}')
    check('expected', not wrong, f'{len(wrong)} mismatches: ' + '; '.join(wrong))

    # overrides-documented ----------------------------------------------------------
    changed = {t for t in effective
               if t in inkscape_map and effective[t] != inkscape_map[t]}
    undocumented = sorted(changed - OVERRIDES)
    for trigger in sorted(changed):
        print(f'OVERRIDE {trigger}: {inkscape_map[trigger]} -> {effective[trigger]}')
    check('overrides-documented', not undocumented,
          f'undocumented overrides: {undocumented}')

    if all(results):
        print(f'CHECK_COREL_KEYS_OK binds={len(rosepen_binds)} overrides={len(changed)}')
        return 0
    print('CHECK_COREL_KEYS_FAIL')
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
