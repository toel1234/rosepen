#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless page-ops lock checker (03-01 task 2).

Runs one scenario from page-ops.expect.json against the page-ops.svg fixture
and verifies the result through real file I/O:

  pages  run `rosepen <fixture> --actions="<scenario actions>;page-list;
         export-filename:<outdir>/<scenario>.svg;export-do"`, then parse the
         exported SVG: <defs>/<view> elements in document order give the page
         viewBox, inkscape:label and inkscape:page-size.  They are asserted
         against the expected list (+-0.01 user units) and every page-list
         stdout line must agree with the SVG (positions are 1-based).  A
         document without <view> elements has the implicit single page; its
         page-list line comes from the root viewBox.
  items  every expected item id must exist and its bbox (x/y/width/height
         plus accumulated translate() on itself and its ancestors; any other
         transform kind is a FAIL "unsupported transform") must lie inside
         the expected page's viewBox.  "absent" ids must be missing; "extra"
         expectations match duplicated items by tag and bbox, not by id.
  pdf    (only with --pdf and a "pdf" section) export the exported SVG to
         PDF, inflate every FlateDecode stream bounded by its own
         "endstream", then assert the page count (/Type /Page not followed
         by "s") and the per-page /MediaBox dims in pt (+-0.5) in page order.

Usage:
    check_page_ops.py <rosepen> <fixture.svg> <expect.json> <outdir> <scenario> [--pdf]

Prints one "STAGE <name> OK|FAIL: <reason>" line per stage and exits
non-zero on the first FAIL.  A non-zero rosepen exit or CRITICAL/g_error on
stderr is a FAIL; a WARNING is only tolerated when the scenario lists its
substring in "allow_warnings".  An optional "stderr_contains" list requires
each substring on stderr (used for the page-nav g_message lines, since the
selected page is not exported), and an optional "stderr_absent" list forbids
substrings (page-nav: no wrap); both are checked by the pages stage.
python3 stdlib only, `python3 -I` safe.
"""

import json
import os
import re
import subprocess
import sys
import zlib
from xml.etree import ElementTree as ET

TIMEOUT_S = 120
EPS = 0.01
PDF_EPS = 0.5

SVG_NS = "http://www.w3.org/2000/svg"
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
TAG_VIEW = f"{{{SVG_NS}}}view"
ATTR_LABEL = f"{{{INKSCAPE_NS}}}label"
ATTR_PAGE_SIZE = f"{{{INKSCAPE_NS}}}page-size"

# The default page label is _("Page %d"); force the C locale so the
# positional default is "Page N" everywhere, independent of the host locale.
ROSEPEN_ENV = dict(os.environ, LC_ALL="C", LANG="C", LANGUAGE="")


class StageFail(Exception):
    """Raised when a stage invariant is violated; message is the reason."""


def fail(reason):
    raise StageFail(reason)


def near(got, wanted, eps):
    return abs(got - wanted) <= eps


# ---------------------------------------------------------------- running ---

def run_rosepen(binary, args, allow_warnings):
    """Run rosepen; non-zero exit, CRITICAL/g_error or an unlisted WARNING
    on stderr is a FAIL."""
    try:
        proc = subprocess.run([binary] + args, capture_output=True, text=True,
                              timeout=TIMEOUT_S, env=ROSEPEN_ENV)
    except subprocess.TimeoutExpired:
        fail(f"rosepen timed out after {TIMEOUT_S}s: {' '.join(args)}")
    except OSError as err:
        fail(f"cannot run rosepen: {err}")
    stderr = proc.stderr or ""
    if proc.returncode != 0:
        tail = " ".join(stderr.split())[-400:]
        fail(f"rosepen exited {proc.returncode} ({' '.join(args)}): {tail}")
    fatal = [ln for ln in stderr.splitlines()
             if "CRITICAL" in ln or re.search(r"ERROR \*\*", ln)]
    if fatal:
        fail(f"rosepen reported a fatal error: {' | '.join(fatal)[:400]}")
    for ln in stderr.splitlines():
        if "WARNING" in ln and not any(allowed in ln for allowed in allow_warnings):
            fail(f"unexpected WARNING on stderr: {ln.strip()[:400]}")
    return proc.stdout or "", stderr


def delete_stale(path):
    """The outdir is reused between runs: remove a stale target first so a
    run that exits 0 without writing can't pass on the previous output."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.lexists(path):
        os.remove(path)


# ------------------------------------------------------------ svg reading ---

def parse_viewbox(text):
    parts = (text or "").replace(",", " ").split()
    if len(parts) != 4:
        fail(f"malformed viewBox '{text}'")
    return [float(v) for v in parts]


def svg_pages(root):
    """(views, root_viewbox, parent_map): <view> elements in document order."""
    views = [el for el in root.iter() if el.tag == TAG_VIEW]
    root_vb = parse_viewbox(root.get("viewBox"))
    parent_map = {child: parent for parent in root.iter() for child in parent}
    return views, root_vb, parent_map


def page_rect(view):
    return parse_viewbox(view.get("viewBox"))


def effective_label(view, position):
    """The label page-list reports: the attribute, else the positional
    default ("Page N", C locale)."""
    label = view.get(ATTR_LABEL)
    return label if label is not None else f"Page {position}"


def check_pages_stage(expect, svg_path, page_list_stdout):
    try:
        root = ET.parse(svg_path).getroot()
    except (ET.ParseError, OSError) as err:
        fail(f"cannot parse exported SVG {svg_path}: {err}")

    views, root_vb, _ = svg_pages(root)

    for key, wanted in (expect.get("root") or {}).items():
        got = parse_viewbox(root.get(key))
        if len(got) != len(wanted) or any(not near(g, w, EPS) for g, w in zip(got, wanted)):
            fail(f"root {key} is '{root.get(key)}', expected {wanted}")

    expected_pages = expect.get("pages") or []
    if len(views) != len(expected_pages):
        fail(f"exported SVG has {len(views)} <view> pages, expected {len(expected_pages)}")

    for i, (view, want) in enumerate(zip(views, expected_pages)):
        pos = i + 1
        rect = page_rect(view)
        if not near(rect[2], want["w"], EPS) or not near(rect[3], want["h"], EPS):
            fail(f"page {pos}: size is {rect[2]}x{rect[3]}, expected {want['w']}x{want['h']}")
        if not near(rect[0], want.get("x", rect[0]), EPS) or not near(rect[1], want.get("y", rect[1]), EPS):
            fail(f"page {pos}: origin is {rect[0]},{rect[1]}, expected {want.get('x')},{want.get('y')}")
        label = effective_label(view, pos)
        if label != want["label"]:
            fail(f"page {pos}: label is '{label}', expected '{want['label']}'")
        preset = view.get(ATTR_PAGE_SIZE)
        if "preset" in want:
            if preset != want["preset"]:
                fail(f"page {pos}: page-size is {preset!r}, expected {want['preset']!r}")

    # page-list stdout must agree with the SVG.
    lines = []
    for ln in (page_list_stdout or "").splitlines():
        fields = ln.split("\t")
        if len(fields) == 5 and fields[0].isdigit():
            lines.append(fields)
    if views:
        if len(lines) != len(views):
            fail(f"page-list printed {len(lines)} lines for {len(views)} SVG pages")
        for i, (fields, view) in enumerate(zip(lines, views)):
            pos = i + 1
            rect = page_rect(view)
            ident, label, size, preset = fields[1], fields[2], fields[3], fields[4]
            if fields[0] != str(pos):
                fail(f"page-list line {pos}: position field is {fields[0]}")
            want_id = view.get("id")
            if ident != (want_id or "-"):
                fail(f"page-list line {pos}: id is '{ident}', SVG id is {want_id!r}")
            if label != effective_label(view, pos):
                fail(f"page-list line {pos}: label is '{label}', SVG label is '{effective_label(view, pos)}'")
            m = re.match(r"^(\S+)x(\S+)$", size)
            if not m:
                fail(f"page-list line {pos}: malformed size '{size}'")
            if not near(float(m.group(1)), rect[2], EPS) or not near(float(m.group(2)), rect[3], EPS):
                fail(f"page-list line {pos}: size {size} disagrees with SVG {rect[2]}x{rect[3]}")
            want_preset = view.get(ATTR_PAGE_SIZE)
            if preset != (want_preset or "-"):
                fail(f"page-list line {pos}: preset is '{preset}', SVG page-size is {want_preset!r}")
    else:
        implicit = (expect.get("implicit") or {})
        if implicit:
            if len(lines) != 1:
                fail(f"implicit document: page-list printed {len(lines)} lines, expected 1")
            f = lines[0]
            wanted = f"1\t-\t{implicit['label']}\t{implicit['w']:.3f}x{implicit['h']:.3f}\t-".split("\t")
            if f != wanted:
                fail(f"implicit page-list line is {f}, expected {wanted}")
        elif lines:
            fail(f"page-list printed lines for a document without pages: {lines[0]}")


# ------------------------------------------------------------ item reading ---

def translate_offset(el, parent_map, root):
    """Accumulated translate() offset of an element (itself + ancestors).
    Any other transform kind is a FAIL: this fixture has none and a document
    that grows one must not silently pass a bbox check."""
    dx = dy = 0.0
    node = el
    while node is not None:
        transform = node.get("transform")
        if transform:
            for m in re.finditer(r"([a-zA-Z]+)\s*\(([^)]*)\)", transform):
                kind = m.group(1)
                if kind != "translate":
                    fail(f"unsupported transform '{kind}' on <{node.tag.split('}')[-1]} id={node.get('id')}>")
                args = [float(v) for v in re.split(r"[\s,]+", m.group(2).strip()) if v]
                if len(args) == 1:
                    args = args + [0.0]
                if len(args) != 2:
                    fail(f"unsupported translate arguments '{m.group(2)}'")
                dx += args[0]
                dy += args[1]
        if node is root:
            break
        node = parent_map.get(node)
    return dx, dy


def item_bbox(el, root, parent_map):
    """(x, y, w, h) for the geometry the fixture uses (rect); the offset
    includes accumulated translate() transforms."""
    dx, dy = translate_offset(el, parent_map, root)
    if el.tag == f"{{{SVG_NS}}}rect":
        x = float(el.get("x", "0")) + dx
        y = float(el.get("y", "0")) + dy
        w = el.get("width")
        h = el.get("height")
        if w is None or h is None:
            fail(f"rect id={el.get('id')!r} has no width/height")
        return x, y, float(w), float(h)
    fail(f"unsupported element <{el.tag.split('}')[-1]} id={el.get('id')!r}> for bbox check")


def elements_by_id(root):
    return {el.get("id"): el for el in root.iter() if el.get("id")}


def page_viewbox(views, root_vb, pos):
    """The viewBox of the 1-based page; a document without <view> elements
    has the implicit page, whose viewBox is the root viewBox."""
    if not views:
        return root_vb
    if not 1 <= pos <= len(views):
        fail(f"no page {pos} (document has {len(views)})")
    return page_rect(views[pos - 1])


def check_inside(views, root_vb, pos, bbox, what):
    x, y, w, h = bbox
    vx, vy, vw, vh = page_viewbox(views, root_vb, pos)
    if not (near(x, vx, EPS) or x > vx) or not (near(y, vy, EPS) or y > vy) \
            or not (near(x + w, vx + vw, EPS) or x + w < vx + vw) \
            or not (near(y + h, vy + vh, EPS) or y + h < vy + vh):
        fail(f"{what}: bbox {bbox} is not inside page {pos} viewBox {[vx, vy, vw, vh]}")


def check_items_stage(expect, svg_path):
    try:
        root = ET.parse(svg_path).getroot()
    except (ET.ParseError, OSError) as err:
        fail(f"cannot parse exported SVG {svg_path}: {err}")

    views, root_vb, parent_map = svg_pages(root)
    by_id = elements_by_id(root)

    for absent in (expect.get("absent") or []):
        if absent in by_id:
            fail(f"id '{absent}' must be absent but is present")

    expected_items = expect.get("items") or {}
    for ident, pos in expected_items.items():
        el = by_id.get(ident)
        if el is None:
            fail(f"expected item '{ident}' is missing from the exported SVG")
        check_inside(views, root_vb, pos, item_bbox(el, root, parent_map), f"item '{ident}'")

    expected_ids = set(expected_items) | set(expect.get("absent") or [])

    for want in (expect.get("extra") or []):
        tag = f"{{{SVG_NS}}}{want['tag']}"
        excluded = expected_ids | set(want.get("exclude") or [])
        bbox = want["bbox"]
        matches = []
        for el in root.iter():
            if el.tag != tag or (el.get("id") or "") in excluded:
                continue
            got = item_bbox(el, root, parent_map)
            if all(near(g, w, EPS) for g, w in zip(got, bbox)):
                matches.append((el.get("id"), got))
        if len(matches) != 1:
            fail(f"expected exactly one new {want['tag']} with bbox {bbox} on page "
                 f"{want['page']}, found {len(matches)}: {matches}")
        check_inside(views, root_vb, want["page"], matches[0][1],
                     f"duplicated {want['tag']} id={matches[0][0]!r}")


# ------------------------------------------------------------ pdf reading ---

def pdf_flatten(data):
    """Concatenation of the uncompressed object text and every stream's
    inflated content; each stream is bounded by its own endstream."""
    parts = []
    pos = 0
    while True:
        start = data.find(b"stream", pos)
        if start == -1:
            break
        parts.append(data[pos:start].decode("latin-1", "replace"))
        m = re.match(rb"stream(\r\n|\n|\r)", data[start:start + 8])
        body_start = start + (m.end() if m else len(b"stream"))
        end = data.find(b"endstream", body_start)
        if end == -1:
            fail("PDF: 'stream' without 'endstream' (truncated file?)")
        body = data[body_start:end]
        dict_window = data[max(pos, start - 4096):start].decode("latin-1", "replace")
        if "/FlateDecode" in dict_window:
            inflated = None
            for offset in range(0, min(64, len(body))):
                try:
                    inflated = zlib.decompress(body[offset:])
                    break
                except zlib.error:
                    continue
            if inflated is None:
                fail("PDF: a FlateDecode stream failed to inflate")
            parts.append(inflated.decode("latin-1", "replace"))
        else:
            parts.append(body.decode("latin-1", "replace"))
        pos = end + len(b"endstream")
    parts.append(data[pos:].decode("latin-1", "replace"))
    return "\n".join(parts)


def check_pdf_stage(binary, expect, pdf_path):
    delete_stale(pdf_path)
    run_rosepen(binary, ["--export-filename=" + pdf_path, pdf_path[:-4] + ".svg"],
                (expect.get("allow_warnings") or []))
    with open(pdf_path, "rb") as f:
        data = f.read()
    if not data.startswith(b"%PDF"):
        fail("pdf: output does not start with %PDF")
    text = pdf_flatten(data)

    want_pages = (expect.get("pdf") or {}).get("pages") or []
    marks = [m for m in re.finditer(r"/Type\s*/Page(?![a-zA-Z])", text)]
    if len(marks) != len(want_pages):
        fail(f"pdf: found {len(marks)} /Type /Page objects, expected {len(want_pages)}")

    for i, (mark, want) in enumerate(zip(marks, want_pages)):
        nxt = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        window = text[mark.start():min(nxt, mark.start() + 4096)]
        mbox = re.search(r"/MediaBox\s*\[([^\]]*)\]", window)
        if not mbox:
            fail(f"pdf: page {i + 1} has no /MediaBox near its /Type /Page")
        nums = [float(v) for v in mbox.group(1).split()]
        if len(nums) != 4:
            fail(f"pdf: page {i + 1} MediaBox '{mbox.group(1)}' does not have 4 numbers")
        if not near(nums[0], 0.0, PDF_EPS) or not near(nums[1], 0.0, PDF_EPS):
            fail(f"pdf: page {i + 1} MediaBox does not start at 0,0: {nums}")
        if not near(nums[2], want[0], PDF_EPS) or not near(nums[3], want[1], PDF_EPS):
            fail(f"pdf: page {i + 1} MediaBox is {nums[2]}x{nums[3]} pt, expected {want[0]}x{want[1]}")


# -------------------------------------------------------------------- main ---

def run_stage(name, fn):
    """Run one stage; print 'STAGE <name> OK' or 'STAGE <name> FAIL: <reason>'
    and return False on failure."""
    try:
        fn()
    except StageFail as err:
        print(f"STAGE {name} FAIL: {err}")
        return False
    print(f"STAGE {name} OK")
    return True


def main(argv):
    pdf = "--pdf" in argv[1:]
    rest = [a for a in argv[1:] if a != "--pdf"]
    if len(rest) != 5:
        print(f"usage: {os.path.basename(argv[0])} <rosepen> <fixture.svg> "
              f"<expect.json> <outdir> <scenario> [--pdf]", file=sys.stderr)
        return 2
    binary, fixture, expect_path, outdir, scenario = rest

    with open(expect_path, encoding="utf-8") as f:
        expect = json.load(f).get("scenarios", {}).get(scenario)
    if expect is None:
        print(f"STAGE setup FAIL: scenario '{scenario}' not in {expect_path}")
        return 1

    allow_warnings = expect.get("allow_warnings") or []
    target = os.path.join(outdir, scenario + ".svg")
    delete_stale(target)

    # The run itself belongs to the pages stage: its non-zero exit, fatal
    # errors and the page-list stdout are judged there.
    actions = expect["actions"] + f";page-list;export-filename:{target};export-do"

    def stage_run():
        return run_rosepen(binary, [fixture, f"--actions={actions}"], allow_warnings)

    try:
        page_list, stderr = stage_run()
    except StageFail as err:
        print(f"STAGE pages FAIL: {err}")
        return 1

    for wanted in expect.get("stderr_contains") or []:
        if wanted not in stderr:
            print(f"STAGE pages FAIL: stderr is missing {wanted!r}")
            return 1
    for unwanted in expect.get("stderr_absent") or []:
        if unwanted in stderr:
            print(f"STAGE pages FAIL: stderr unexpectedly has {unwanted!r}")
            return 1

    if not run_stage("pages", lambda: check_pages_stage(expect, target, page_list)):
        return 1
    if not run_stage("items", lambda: check_items_stage(expect, target)):
        return 1
    if pdf and expect.get("pdf"):
        if not run_stage("pdf", lambda: check_pdf_stage(binary, expect, os.path.join(outdir, scenario + ".pdf"))):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
