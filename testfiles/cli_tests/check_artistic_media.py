#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless lock for the Rosepen 05-03 artistic media strokes.

Per scenario: drive app.artistic-add / app.artistic-apply / app.artistic-set /
app.artistic-break (plus plain object actions) on the fixture, then prove the
result on four stages:

  list  app.artistic-list TSV: id, mode, shape, width, angle, nodes, length,
        source (+-1e-3 for the numbers) and the artistic-count line
  svg   plain-SVG export: effect attributes and embedded shape data, per-path
        structure (class, inkscape:path-effect, inkscape:original-d, style),
        sibling order and the geometry bbox of each path's d
  png   PNG export: RGB pixel samples given in page millimetres (+-8/channel)
  pdf   with --pdf: the capypdf export re-imported through Rosepen's own PDF
        importer, then the same pixel samples

The png/pdf helpers, run_rosepen and delete_stale are copied from
check_dimension.py (Rosepen 05-02, same project) so the two corpora behave
identically; python3 -I cannot import a sibling checker.

Prints one "STAGE <name> OK|FAIL: <reason>" line per stage and a final
"ARTISTIC_MEDIA_OK scenario=<s>"; exits non-zero on the first FAIL.
Every expected value in the .expect.json is hand-derived from the model in
artistic-media-geometry.h / artistic-media-library.h, never from a render.
"""

import json
import os
import re
import struct
import subprocess
import sys
import zlib
import xml.etree.ElementTree as ET

TIMEOUT_S = 300
COLOR_TOL = 8
EPS = 0.05
VALUE_EPS = 1e-3
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
EXPORT_BACKGROUND = ["--export-background=#ffffff", "--export-background-opacity=1"]

# The page label / messages are only used positionally; force the C locale.
ROSEPEN_ENV = dict(os.environ, LC_ALL="C", LANG="C", LANGUAGE="")

NUM_RE = r"[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?"


def fail(reason):
    raise RuntimeError(reason)


# ---------------------------------------------------------------------------
# Process / file helpers (copied from check_dimension.py)
# ---------------------------------------------------------------------------

def run_rosepen(binary, args, allow_warnings):
    """Run rosepen; non-zero exit, CRITICAL/g_error or an unlisted WARNING on
    stderr is a FAIL."""
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


# ---------------------------------------------------------------------------
# list stage
# ---------------------------------------------------------------------------

def check_list_run(expect, binary, fixture, tail, allow):
    """Run the actions + artistic-list once and check the TSV and stderr."""
    stdout, stderr = run_rosepen(binary, [fixture, "--actions=" + tail], allow)
    check_list_stage(expect, stdout)
    for needle in expect.get("stderr_contains") or []:
        if needle not in stderr:
            fail(f"stderr lacks {needle!r}")


def check_list_stage(expect, stdout):
    wanted = expect.get("list") or {}
    rows = []
    count = None
    for line in stdout.splitlines():
        if line.startswith("artistic-count "):
            try:
                count = int(line.split()[1])
            except (IndexError, ValueError):
                fail(f"malformed count line: {line!r}")
            continue
        fields = line.split("\t")
        if len(fields) == 8:
            rows.append(fields)

    want_rows = wanted.get("rows") or []
    if count is None:
        fail("artistic-list printed no artistic-count line")
    if count != wanted.get("count", len(want_rows)):
        fail(f"artistic-count is {count}, expected {wanted.get('count', len(want_rows))}")
    if len(rows) != len(want_rows):
        fail(f"artistic-list has {len(rows)} rows, expected {len(want_rows)}: {rows}")

    for row, want in zip(rows, want_rows):
        rid, mode, shape, width, angle, nodes, length, source = row
        if rid != want[0]:
            fail(f"id is {rid!r}, expected {want[0]!r}")
        if mode != want[1]:
            fail(f"mode of {rid} is {mode!r}, expected {want[1]!r}")
        if shape != want[2]:
            fail(f"shape of {rid} is {shape!r}, expected {want[2]!r}")
        if abs(float(width) - want[3]) > VALUE_EPS:
            fail(f"width of {rid} is {width}, expected {want[3]}")
        if abs(float(angle) - want[4]) > VALUE_EPS:
            fail(f"angle of {rid} is {angle}, expected {want[4]}")
        if int(nodes) != int(want[5]):
            fail(f"nodes of {rid} is {nodes}, expected {want[5]}")
        if abs(float(length) - want[6]) > VALUE_EPS:
            fail(f"length of {rid} is {length}, expected {want[6]}")
        if source != want[7]:
            fail(f"source of {rid} is {source!r}, expected {want[7]!r}")


# ---------------------------------------------------------------------------
# svg stage
# ---------------------------------------------------------------------------

def tag_of(node):
    return node.tag.rsplit("}", 1)[-1]


def attr_of(node, name):
    if name.startswith("inkscape:"):
        return node.get("{%s}%s" % (INKSCAPE_NS, name.split(":", 1)[1]))
    return node.get(name)


def iter_all(node):
    yield node
    for child in node:
        yield from iter_all(child)


def cubic_samples(p0, p1, p2, p3, steps=64):
    out = []
    for k in range(1, steps + 1):
        t = k / steps
        mt = 1.0 - t
        out.append((mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t ** 3 * p3[0],
                    mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def quad_samples(p0, p1, p2, steps=64):
    out = []
    for k in range(1, steps + 1):
        t = k / steps
        mt = 1.0 - t
        out.append((mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0],
                    mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]))
    return out


def path_points(d):
    """All sampled points of a path's d (M/L/H/V/C/S/Q/T/Z, absolute+relative).

    Curves are sampled at 64 steps; an elliptical arc is not supported and is
    a FAIL (the artistic media output never contains one).
    """
    tokens = re.findall(r"[MmLlHhVvCcSsQqTtZzAa]|" + NUM_RE, d)
    pts = []
    x = y = 0.0
    sx = sy = 0.0
    ctrl = None
    cmd = None
    i = 0

    def number():
        nonlocal i
        if i >= len(tokens):
            fail(f"path data ran out of numbers: {d!r}")
        value = float(tokens[i])
        i += 1
        return value

    while i < len(tokens):
        tok = tokens[i]
        if re.fullmatch(r"[A-Za-z]", tok):
            cmd = tok
            i += 1
            if cmd in "Zz":
                x, y = sx, sy
                pts.append((x, y))
                ctrl = None
                continue
            if cmd.upper() == "A":
                fail("A (elliptical arc) is not supported by the bbox checker")
        if cmd is None:
            fail(f"path data does not start with a command: {d!r}")

        rel = cmd.islower()
        upper = cmd.upper()

        if upper == "M":
            dx, dy = number(), number()
            x = x + dx if rel else dx
            y = y + dy if rel else dy
            sx, sy = x, y
            pts.append((x, y))
            ctrl = None
            cmd = "l" if rel else "L"
            continue

        if upper == "L":
            dx, dy = number(), number()
            x = x + dx if rel else dx
            y = y + dy if rel else dy
            pts.append((x, y))
            ctrl = None
        elif upper == "H":
            dx = number()
            x = x + dx if rel else dx
            pts.append((x, y))
            ctrl = None
        elif upper == "V":
            dy = number()
            y = y + dy if rel else dy
            pts.append((x, y))
            ctrl = None
        elif upper == "C":
            raw = [(number(), number()) for _ in range(3)]
            stops = [(x + dx, y + dy) if rel else (dx, dy) for dx, dy in raw]
            pts.extend(cubic_samples((x, y), *stops))
            ctrl = stops[1]
            x, y = stops[2]
        elif upper == "S":
            raw = [(number(), number()) for _ in range(2)]
            stops = [(x + dx, y + dy) if rel else (dx, dy) for dx, dy in raw]
            first = (2 * x - ctrl[0], 2 * y - ctrl[1]) if ctrl else (x, y)
            pts.extend(cubic_samples((x, y), first, *stops))
            ctrl = stops[0]
            x, y = stops[1]
        elif upper == "Q":
            raw = [(number(), number()) for _ in range(2)]
            stops = [(x + dx, y + dy) if rel else (dx, dy) for dx, dy in raw]
            pts.extend(quad_samples((x, y), *stops))
            ctrl = stops[0]
            x, y = stops[1]
        elif upper == "T":
            raw = [(number(), number())]
            stops = [(x + dx, y + dy) if rel else (dx, dy) for dx, dy in raw]
            first = (2 * x - ctrl[0], 2 * y - ctrl[1]) if ctrl else (x, y)
            pts.extend(quad_samples((x, y), first, stops[0]))
            ctrl = first
            x, y = stops[0]
    if not pts:
        fail("path has no coordinates")
    return pts


def path_bbox(d):
    pts = path_points(d)
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def check_svg_stage(expect, target):
    svg_expect = expect.get("svg") or {}
    try:
        tree = ET.parse(target)
    except ET.ParseError as err:
        fail(f"exported SVG does not parse: {err}")
    root = tree.getroot()

    by_id = {}
    parents = {}
    for parent in iter_all(root):
        for child in list(parent):
            parents[child] = parent
            rid = child.get("id")
            if rid:
                by_id[rid] = child

    effects = {}
    for node in iter_all(root):
        if tag_of(node) == "path-effect" and node.get("effect") == "artistic_media":
            if node.get("id"):
                effects[node.get("id")] = node
    if "effect_count" in svg_expect and len(effects) != svg_expect["effect_count"]:
        fail(f"found {len(effects)} artistic media effects, expected {svg_expect['effect_count']}")

    refs = 0
    for node in iter_all(root):
        href = attr_of(node, "inkscape:path-effect") or ""
        for target_id in re.findall(r"#([^,;\s]+)", href):
            if target_id in effects:
                refs += 1
                break
    if "artistic_refs" in svg_expect and refs != svg_expect["artistic_refs"]:
        fail(f"{refs} elements reference an artistic media effect, expected {svg_expect['artistic_refs']}")

    for want in svg_expect.get("effects") or []:
        node = effects.get(want.get("id"))
        if node is None:
            fail(f"no artistic media effect with id {want.get('id')!r}")
        for name, value in (want.get("attrs") or {}).items():
            got = node.get(name)
            if got != value:
                fail(f"effect {want['id']} attribute {name} is {got!r}, expected {value!r}")
        if "shape_d" in want:
            got = node.get("shape_d")
            if got != want["shape_d"]:
                fail(f"effect {want['id']} shape_d is {got!r}, expected {want['shape_d']!r}")
        for name in want.get("absent") or []:
            if node.get(name) is not None:
                fail(f"effect {want['id']} must not carry {name}")

    for want in svg_expect.get("paths") or []:
        rid = want.get("id")
        node = by_id.get(rid)
        if node is None:
            fail(f"no element with id {rid!r}")
        if "has_path_effect" in want:
            has = attr_of(node, "inkscape:path-effect") is not None
            if has != want["has_path_effect"]:
                fail(f"{rid} inkscape:path-effect presence is {has}, expected {want['has_path_effect']}")
        if "has_original_d" in want:
            has = attr_of(node, "inkscape:original-d") is not None
            if has != want["has_original_d"]:
                fail(f"{rid} inkscape:original-d presence is {has}, expected {want['has_original_d']}")
        if "class_has" in want:
            if want["class_has"] not in (node.get("class") or ""):
                fail(f"{rid} class is {node.get('class')!r}, expected it to contain {want['class_has']!r}")
        if "class_absent" in want:
            if want["class_absent"] in (node.get("class") or ""):
                fail(f"{rid} class {node.get('class')!r} must not contain {want['class_absent']!r}")
        if "label" in want:
            got = attr_of(node, "inkscape:label")
            if got != want["label"]:
                fail(f"{rid} label is {got!r}, expected {want['label']!r}")
        for needle in want.get("style_has") or []:
            if needle not in (node.get("style") or ""):
                fail(f"{rid} style {node.get('style')!r} lacks {needle!r}")
        if "bbox" in want:
            d = node.get("d")
            if not d:
                fail(f"{rid} has no d attribute")
            got = path_bbox(d)
            for g, w, name in zip(got, want["bbox"], "minx miny maxx maxy".split()):
                if abs(g - w) > EPS:
                    fail(f"{rid} bbox {name} is {g:.4f}, expected {w}")
        if "after" in want:
            previous = want["after"]
            parent = parents.get(node)
            siblings = [c.get("id") for c in list(parent)] if parent is not None else []
            if rid not in siblings or siblings.index(rid) == 0:
                fail(f"{rid} has no previous sibling")
            got = siblings[siblings.index(rid) - 1]
            if got != previous:
                fail(f"{rid} follows {got!r}, expected {previous!r}")


# ---------------------------------------------------------------------------
# png / pdf stages (copied from check_dimension.py)
# ---------------------------------------------------------------------------

def png_decode(path):
    """Minimal PNG reader: 8 bit RGB/RGBA/grayscale, non-interlaced."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail(f"{path} is not a PNG")
    pos = 8
    width = height = 0
    bit_depth = color_type = None
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        chunk = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if chunk == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif chunk == b"IDAT":
            idat += body
        elif chunk == b"IEND":
            break
    if bit_depth != 8 or color_type not in (0, 2, 6):
        fail(f"unsupported PNG format (depth {bit_depth}, color {color_type})")
    channels = {0: 1, 2: 3, 6: 4}[color_type]
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out += line
        prev = line
    return width, height, channels, bytes(out)


def png_pixel_rgb(png, x_mm, y_mm, page_w_mm=200.0, page_h_mm=100.0):
    width, height, channels, pixels = png_decode(png)
    px = min(width - 1, max(0, int(round(x_mm / page_w_mm * width))))
    py = min(height - 1, max(0, int(round(y_mm / page_h_mm * height))))
    offset = (py * width + px) * channels
    if channels >= 3:
        return pixels[offset], pixels[offset + 1], pixels[offset + 2]
    value = pixels[offset]
    return value, value, value


def check_png_samples(png, samples, page_w_mm=200.0, page_h_mm=100.0):
    for x_mm, y_mm, colour in samples:
        want = tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5))
        got = png_pixel_rgb(png, x_mm, y_mm, page_w_mm, page_h_mm)
        if any(abs(g - w) > COLOR_TOL for g, w in zip(got, want)):
            fail(f"sample ({x_mm},{y_mm}) mm is rgb{got}, expected {colour}")


def check_png_stage(binary, expect, target, outdir, tag):
    path = os.path.join(outdir, tag + ".png")
    delete_stale(path)
    run_rosepen(binary, [target, "--export-type=png", "--export-dpi=384"] + EXPORT_BACKGROUND +
                [f"--export-filename={path}"], expect.get("allow_warnings") or [])
    check_png_samples(path, (expect.get("png") or {}).get("samples") or [])


def check_pdf_stage(binary, expect, target, outdir, tag):
    pdf_path = os.path.join(outdir, tag + ".pdf")
    delete_stale(pdf_path)
    run_rosepen(binary, ["--export-extension=org.inkscape.output.pdf.capypdf",
                         f"--export-filename={pdf_path}", target],
                expect.get("allow_warnings") or [])
    with open(pdf_path, "rb") as f:
        data = f.read()
    if not data.startswith(b"%PDF"):
        fail("pdf: output does not start with %PDF")
    # Rosepen's own importer is the rasterizer; a failure here is a finding.
    path = os.path.join(outdir, tag + "-pdf.png")
    delete_stale(path)
    run_rosepen(binary, [pdf_path, "--export-type=png", "--export-dpi=384"] + EXPORT_BACKGROUND +
                [f"--export-filename={path}"], expect.get("allow_warnings") or [])
    check_png_samples(path, (expect.get("png") or {}).get("samples") or [])


def run_stage(name, fn):
    try:
        fn()
    except RuntimeError as err:
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
    binary, fixture_arg, expect_path, outdir, scenario = rest

    with open(expect_path, encoding="utf-8") as f:
        document_expect = json.load(f)
    expect = document_expect.get("scenarios", {}).get(scenario)
    if expect is None:
        print(f"STAGE setup FAIL: scenario '{scenario}' not in {expect_path}")
        return 1
    allow = expect.get("allow_warnings") or []

    fixture = fixture_arg
    if expect.get("fixture"):
        fixture = os.path.join(os.path.dirname(fixture_arg), expect["fixture"])

    target = os.path.join(outdir, scenario + ".svg")
    delete_stale(target)

    prefix = ";".join(a for a in expect.get("actions", "").split(";") if a)

    def with_tail(tail):
        if not tail:
            return prefix
        return prefix + ";" + tail if prefix else tail

    # A bare SVG/PNG export does not evaluate live path effects (upstream), so a
    # scenario may end its actions with artistic-list to force the evaluation
    # the list stage needs anyway; never run it twice.
    list_tail = "" if "artistic-list" in prefix else "artistic-list"
    if not run_stage("list", lambda: check_list_run(
            expect, binary, fixture, with_tail(list_tail), allow)):
        return 1
    if not run_stage("svg", lambda: (
            run_rosepen(binary, [fixture, "--actions=" + with_tail(f"export-filename:{target};export-do")], allow),
            check_svg_stage(expect, target))):
        return 1
    if not run_stage("png", lambda: check_png_stage(binary, expect, target, outdir, scenario)):
        return 1
    if pdf and (expect.get("png") or {}).get("samples"):
        if not run_stage("pdf", lambda: check_pdf_stage(binary, expect, target, outdir, scenario)):
            return 1
    print(f"ARTISTIC_MEDIA_OK scenario={scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
