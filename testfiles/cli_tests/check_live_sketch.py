#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless lock for the Rosepen 05-05 Live Sketch action (app.live-sketch).

Per scenario: drive app.live-sketch on the fixture, then prove the result on
three (four with --pdf) stages:

  run   run the scenario's actions and export the result to SVG; a non-zero
        exit, a CRITICAL or an unlisted WARNING on stderr is a FAIL, and the
        scenario's expected "live-sketch: ..." warnings are matched here
  svg   plain-SVG export: count svg:path elements that are not in the fixture,
        and for each expected new path check its id, parent, sibling position,
        label, style, document bbox (ancestor translate/scale/matrix applied),
        node count, absolute/relative C/L command counts, that it is open, the
        worst distance from given input points to the curve, and the presence
        of expected attributes/classes; unchanged fixture ids keep byte-equal
        attributes
  png   PNG export at 384 dpi: RGB pixel samples given in page millimetres
        (+-8/channel)
  pdf   with --pdf: the capypdf export re-imported through Rosepen's own PDF
        importer, then the same pixel samples

The process / path / PNG / PDF helpers are copied from check_smart_fill.py
(Rosepen 05-04, which itself credits check_artistic_media.py) so the corpora
behave identically; python3 -I cannot import a sibling checker. The additions
here are the structure scan (nodes, C/L counts, open-path flag), the exact
point-to-segment / refined point-to-cubic distance used by max_point_distance,
and tolerance-aware style values. Every expected value in the .expect.json is
hand-derived from the fixture points and live-sketch-geometry, never from a
render.

Prints one "STAGE <name> OK|FAIL: <reason>" line per stage and a final
"LIVE_SKETCH_OK scenario=<s>"; exits non-zero on the first FAIL.
"""

import json
import math
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
NUM_TOL = 1e-4
DISTANCE_BUDGET = 0.01
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
EXPORT_BACKGROUND = ["--export-background=#ffffff", "--export-background-opacity=1"]
PAGE_W_MM = 200.0
PAGE_H_MM = 100.0

# The page label / messages are only used positionally; force the C locale.
ROSEPEN_ENV = dict(os.environ, LC_ALL="C", LANG="C", LANGUAGE="")

NUM_RE = r"[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?"


def fail(reason):
    raise RuntimeError(reason)


# ---------------------------------------------------------------------------
# Process / file helpers (copied from check_smart_fill.py)
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
# SVG / path helpers
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


def bbox_of_points(points):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return min(xs), min(ys), max(xs), max(ys)


IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def affine_mul(outer, inner):
    """Compose two SVG matrices: the result applies @inner then @outer."""
    a1, b1, c1, d1, e1, f1 = outer
    a2, b2, c2, d2, e2, f2 = inner
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2,
            a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)


def affine_apply(matrix, point):
    a, b, c, d, e, f = matrix
    x, y = point
    return (a * x + c * y + e, b * x + d * y + f)


def node_matrix(node):
    transform = node.get("transform")
    if not transform:
        return IDENTITY
    matrix = IDENTITY
    for name, args in re.findall(r"([A-Za-z]+)\s*\(([^)]*)\)", transform):
        nums = [float(v) for v in re.split(r"[ ,]+", args.strip()) if v]
        if name == "translate":
            local = (1.0, 0.0, 0.0, 1.0, nums[0], nums[1] if len(nums) > 1 else 0.0)
        elif name == "scale":
            sx = nums[0]
            sy = nums[1] if len(nums) > 1 else sx
            local = (sx, 0.0, 0.0, sy, 0.0, 0.0)
        elif name == "matrix":
            if len(nums) != 6:
                fail(f"malformed matrix transform {transform!r}")
            local = tuple(nums)
        else:
            fail(f"unsupported transform {transform!r}")
        matrix = affine_mul(matrix, local)
    return matrix


def document_matrix(node, parents):
    chain = []
    current = node
    while current is not None:
        chain.append(node_matrix(current))
        current = parents.get(current)
    matrix = IDENTITY
    for local in reversed(chain):  # root first, node last
        matrix = affine_mul(matrix, local)
    return matrix


def _tokens(d):
    return re.findall(r"[A-Za-z]|" + NUM_RE, d)


def path_structure(d):
    """Nodes, C/L command counts, subpath count and the open/closed flag.

    Nodes are a subpath's start point plus every segment end (a Z adds its
    closing end). H/V count as L, implicit repeats count once per segment.
    Q/S/T/A are a FAIL: LiveSketch writes only M/L/C/Z.
    """
    tokens = _tokens(d)
    i = 0
    cmd = None
    subpaths = 0
    nodes = 0
    counts = {"C": 0, "L": 0}
    closed = False

    def number():
        nonlocal i
        if i >= len(tokens) or re.fullmatch(r"[A-Za-z]", tokens[i]):
            fail(f"path data ran out of numbers: {d!r}")
        value = float(tokens[i])
        i += 1
        return value

    while i < len(tokens):
        token = tokens[i]
        if re.fullmatch(r"[A-Za-z]", token):
            cmd = token
            i += 1
            upper = cmd.upper()
            if upper in "AQST":
                fail(f"{upper} command is not supported by the checker")
            if upper == "Z":
                nodes += 1
                closed = True
                cmd = None
                continue
            if upper == "M":
                subpaths += 1
                number()
                number()
                nodes += 1
                cmd = "l" if cmd.islower() else "L"
                continue
        if cmd is None:
            fail(f"path data does not start with a command: {d!r}")

        upper = cmd.upper()
        if upper == "L":
            number()
            number()
            nodes += 1
            counts["L"] += 1
        elif upper == "H":
            number()
            nodes += 1
            counts["L"] += 1
        elif upper == "V":
            number()
            nodes += 1
            counts["L"] += 1
        elif upper == "C":
            for _ in range(6):
                number()
            nodes += 1
            counts["C"] += 1
        else:
            fail(f"unsupported path command {cmd!r}")

    if subpaths == 0 or nodes < 2:
        fail(f"path has no surface: {d!r}")
    return {"nodes": nodes, "commands": counts, "subpaths": subpaths,
            "closed": closed or subpaths != 1}


def path_segments(d):
    """Segments of the path as ("L", p0, p1) or ("C", p0, p1, p2, p3), in
    document units of the d attribute (no transform applied)."""
    tokens = _tokens(d)
    segments = []
    i = 0
    cmd = None
    x = y = sx = sy = 0.0

    def number():
        nonlocal i
        if i >= len(tokens) or re.fullmatch(r"[A-Za-z]", tokens[i]):
            fail(f"path data ran out of numbers: {d!r}")
        value = float(tokens[i])
        i += 1
        return value

    while i < len(tokens):
        token = tokens[i]
        if re.fullmatch(r"[A-Za-z]", token):
            cmd = token
            i += 1
            upper = cmd.upper()
            if upper in "AQST":
                fail(f"{upper} command is not supported by the checker")
            if upper == "Z":
                segments.append(("L", (x, y), (sx, sy)))
                x, y = sx, sy
                cmd = None
                continue
            if upper == "M":
                rel = cmd.islower()
                dx, dy = number(), number()
                x = x + dx if rel else dx
                y = y + dy if rel else dy
                sx, sy = x, y
                cmd = "l" if rel else "L"
                continue
        if cmd is None:
            fail(f"path data does not start with a command: {d!r}")

        rel = cmd.islower()
        upper = cmd.upper()
        if upper == "L":
            dx, dy = number(), number()
            nx = x + dx if rel else dx
            ny = y + dy if rel else dy
            segments.append(("L", (x, y), (nx, ny)))
            x, y = nx, ny
        elif upper == "H":
            dx = number()
            nx = x + dx if rel else dx
            segments.append(("L", (x, y), (nx, y)))
            x = nx
        elif upper == "V":
            dy = number()
            ny = y + dy if rel else dy
            segments.append(("L", (x, y), (x, ny)))
            y = ny
        elif upper == "C":
            raw = [(number(), number()) for _ in range(3)]
            stops = [(x + dx, y + dy) if rel else (dx, dy) for dx, dy in raw]
            segments.append(("C", (x, y), stops[0], stops[1], stops[2]))
            x, y = stops[2]
        else:
            fail(f"unsupported path command {cmd!r}")
    return segments


def segment_samples(segments, steps=128):
    points = []
    for seg in segments:
        if seg[0] == "L":
            points.append(seg[1])
            points.append(seg[2])
        else:
            points.append(seg[1])
            points.extend(cubic_samples(seg[1], seg[2], seg[3], seg[4], steps))
    return points


def point_segment_distance(point, a, b):
    ax, ay = a
    bx, by = b
    px, py = point
    dx, dy = bx - ax, by - ay
    length2 = dx * dx + dy * dy
    if length2 <= 1e-24:
        return math.hypot(px - ax, py - ay)
    t = ((px - ax) * dx + (py - ay) * dy) / length2
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def cubic_point(cubic, t):
    p0, p1, p2, p3 = cubic
    mt = 1.0 - t
    return (mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t ** 3 * p3[0],
            mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t ** 3 * p3[1])


def point_cubic_distance(point, cubic, steps=64):
    """Exact enough distance: a 64-step scan locates the local minima, each of
    which is then refined by a ternary search (the sampling error budget is
    DISTANCE_BUDGET for the limit comparison)."""

    def squared(t):
        q = cubic_point(cubic, t)
        return (q[0] - point[0]) ** 2 + (q[1] - point[1]) ** 2

    values = [squared(k / steps) for k in range(steps + 1)]
    best = min(values[0], values[-1])
    for k in range(1, steps):
        if values[k] <= values[k - 1] and values[k] <= values[k + 1]:
            lo = (k - 1) / steps
            hi = (k + 1) / steps
            for _ in range(60):
                m1 = lo + (hi - lo) / 3.0
                m2 = hi - (hi - lo) / 3.0
                if squared(m1) < squared(m2):
                    hi = m2
                else:
                    lo = m1
            best = min(best, squared((lo + hi) / 2.0))
    return math.sqrt(best)


def distance_to_path(point, segments):
    best = float("inf")
    for seg in segments:
        if seg[0] == "L":
            best = min(best, point_segment_distance(point, seg[1], seg[2]))
        else:
            best = min(best, point_cubic_distance(point, seg[1:]))
    return best


# ---------------------------------------------------------------------------
# svg stage
# ---------------------------------------------------------------------------

def parse_style(style):
    result = {}
    for declaration in (style or "").split(";"):
        if ":" not in declaration:
            continue
        key, value = declaration.split(":", 1)
        result[key.strip()] = value.strip()
    return result


def same_style_value(got, want):
    try:
        return abs(float(got) - float(want)) <= NUM_TOL
    except (TypeError, ValueError):
        return str(got).strip().lower() == str(want).strip().lower()


def fixture_ids(fixture_arg):
    try:
        tree = ET.parse(fixture_arg)
    except ET.ParseError as err:
        fail(f"fixture does not parse: {err}")
    return {node.get("id") for node in iter_all(tree.getroot()) if node.get("id")}


def check_unchanged(expect, root, fixture_root):
    by_id = {node.get("id"): node for node in iter_all(root) if node.get("id")}
    fixture_by_id = {node.get("id"): node for node in iter_all(fixture_root) if node.get("id")}
    for rid in expect.get("unchanged_ids") or []:
        want = fixture_by_id.get(rid)
        got = by_id.get(rid)
        if want is None:
            fail(f"unchanged id {rid!r} is not in the fixture")
        if got is None:
            fail(f"unchanged id {rid!r} disappeared from the export")
        if dict(got.attrib) != dict(want.attrib):
            fail(f"{rid} attributes changed: {dict(got.attrib)} != {dict(want.attrib)}")


GEOMETRY_KEYS = ("bbox", "bbox_within", "nodes", "nodes_min", "nodes_max",
                 "commands", "closed", "max_point_distance")


def check_svg_stage(expect, target, fixture_arg):
    spec = expect.get("new_paths") or {}
    try:
        tree = ET.parse(target)
        fixture_tree = ET.parse(fixture_arg)
    except ET.ParseError as err:
        fail(f"exported SVG does not parse: {err}")
    root = tree.getroot()
    fixture_root = fixture_tree.getroot()

    parents = {}
    by_id = {}
    for parent in iter_all(root):
        for child in list(parent):
            parents[child] = parent
            rid = child.get("id")
            if rid:
                by_id[rid] = child

    old_ids = fixture_ids(fixture_arg)
    new_paths = [node for node in iter_all(root)
                 if tag_of(node) == "path" and node.get("id") not in old_ids]

    want_count = spec.get("count", 0)
    if len(new_paths) != want_count:
        fail(f"{len(new_paths)} new paths, expected {want_count}")
    items = spec.get("items") or []
    if len(items) != want_count:
        fail(f"{len(items)} expected items but count is {want_count}")

    for index, want in enumerate(items):
        if want.get("id"):
            node = by_id.get(want["id"])
            if node is None:
                fail(f"no element with id {want['id']!r}")
            if node not in new_paths:
                fail(f"{want['id']} is not a new path")
        else:
            node = new_paths[index]
        rid = node.get("id") or f"new-path-{index}"

        if want.get("parent") is not None:
            parent = parents.get(node)
            got = parent.get("id") if parent is not None else None
            if got != want["parent"]:
                fail(f"{rid} parent is {got!r}, expected {want['parent']!r}")
        if "last_child" in want:
            parent = parents.get(node)
            siblings = list(parent) if parent is not None else []
            is_last = bool(siblings) and siblings[-1] is node
            if is_last != bool(want["last_child"]):
                fail(f"{rid} last-child is {is_last}, expected {bool(want['last_child'])}")

        if want.get("label") is not None:
            got = attr_of(node, "inkscape:label")
            if got != want["label"]:
                fail(f"{rid} label is {got!r}, expected {want['label']!r}")

        if want.get("class_contains") is not None:
            got = node.get("class") or ""
            if want["class_contains"] not in got:
                fail(f"{rid} class {got!r} does not contain {want['class_contains']!r}")

        for name in want.get("attrs_present") or []:
            if attr_of(node, name) is None:
                fail(f"{rid} has no {name} attribute")

        if "style" in want:
            got_style = parse_style(node.get("style"))
            want_style = want["style"]
            if set(got_style) != set(want_style):
                fail(f"{rid} style keys {sorted(got_style)} != {sorted(want_style)}")
            for key, value in want_style.items():
                if not same_style_value(got_style[key], value):
                    fail(f"{rid} style {key} is {got_style[key]!r}, expected {value!r}")

        if not any(key in want for key in GEOMETRY_KEYS):
            continue

        d = node.get("d")
        if not d:
            fail(f"{rid} has no d attribute")
        structure = path_structure(d)

        if "nodes" in want and structure["nodes"] != want["nodes"]:
            fail(f"{rid} has {structure['nodes']} nodes, expected {want['nodes']}")
        if "nodes_min" in want and structure["nodes"] < want["nodes_min"]:
            fail(f"{rid} has {structure['nodes']} nodes, expected >= {want['nodes_min']}")
        if "nodes_max" in want and structure["nodes"] > want["nodes_max"]:
            fail(f"{rid} has {structure['nodes']} nodes, expected <= {want['nodes_max']}")
        if "commands" in want:
            for command, count in want["commands"].items():
                got = structure["commands"].get(command, 0)
                if got != count:
                    fail(f"{rid} has {got} {command} segments, expected {count}")
        if "closed" in want and structure["closed"] != want["closed"]:
            fail(f"{rid} closed is {structure['closed']}, expected {want['closed']}")

        segments = path_segments(d)

        if "max_point_distance" in want:
            # The listed points are document coordinates: measure against the
            # path with its ancestor transforms applied (affine maps keep
            # lines and cubics, so mapping the control points is exact).
            matrix = document_matrix(node, parents)
            doc_segments = [(seg[0],) + tuple(affine_apply(matrix, p) for p in seg[1:])
                            for seg in segments]
            spec_distance = want["max_point_distance"]
            limit = spec_distance.get("limit", 0.0)
            worst = 0.0
            for point in spec_distance.get("points") or []:
                worst = max(worst, distance_to_path((point[0], point[1]), doc_segments))
            if worst > limit + DISTANCE_BUDGET:
                fail(f"{rid} worst point distance is {worst:.4f}, expected <= {limit}")

        if "bbox" in want or "bbox_within" in want:
            matrix = document_matrix(node, parents)
            sampled = [affine_apply(matrix, p) for p in segment_samples(segments)]
            got = bbox_of_points(sampled)
            if "bbox" in want:
                for value, expected, name in zip(got, want["bbox"], "minx miny maxx maxy".split()):
                    if abs(value - expected) > EPS:
                        fail(f"{rid} bbox {name} is {value:.4f}, expected {expected}")
            if "bbox_within" in want:
                box = want["bbox_within"]
                if (got[0] < box[0] - EPS or got[1] < box[1] - EPS or
                        got[2] > box[2] + EPS or got[3] > box[3] + EPS):
                    fail(f"{rid} bbox {tuple(round(v, 3) for v in got)} is outside {box}")

    check_unchanged(expect, root, fixture_root)


# ---------------------------------------------------------------------------
# png / pdf stages (copied from check_smart_fill.py)
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


def png_pixel_rgb(png, x_mm, y_mm, page_w_mm=PAGE_W_MM, page_h_mm=PAGE_H_MM):
    width, height, channels, pixels = png_decode(png)
    px = min(width - 1, max(0, int(round(x_mm / page_w_mm * width))))
    py = min(height - 1, max(0, int(round(y_mm / page_h_mm * height))))
    offset = (py * width + px) * channels
    if channels >= 3:
        return pixels[offset], pixels[offset + 1], pixels[offset + 2]
    value = pixels[offset]
    return value, value, value


def check_png_samples(png, samples):
    for x_mm, y_mm, colour in samples:
        want = tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5))
        got = png_pixel_rgb(png, x_mm, y_mm)
        if any(abs(g - w) > COLOR_TOL for g, w in zip(got, want)):
            fail(f"sample ({x_mm},{y_mm}) mm is rgb{got}, expected {colour}")


def check_png_stage(binary, expect, target, outdir, tag):
    path = os.path.join(outdir, tag + ".png")
    delete_stale(path)
    run_rosepen(binary, [target, "--export-type=png", "--export-area-page", "--export-dpi=384"] +
                EXPORT_BACKGROUND + [f"--export-filename={path}"],
                expect.get("allow_warnings") or [])
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
    run_rosepen(binary, [pdf_path, "--export-type=png", "--export-area-page", "--export-dpi=384"] +
                EXPORT_BACKGROUND + [f"--export-filename={path}"],
                expect.get("allow_warnings") or [])
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

    def check_run():
        stdout, stderr = run_rosepen(
            binary,
            [fixture, "--actions=" + expect.get("actions", "") +
             f";export-filename:{target};export-do"],
            allow)
        del stdout
        # In order of the calls: each message must follow the previous one.
        position = 0
        for needle in expect.get("stderr_contains") or []:
            found = stderr.find(needle, position)
            if found < 0:
                fail(f"stderr lacks {needle!r} (in order)")
            position = found + len(needle)

    if not run_stage("run", check_run):
        return 1
    if not run_stage("svg", lambda: check_svg_stage(expect, target, fixture)):
        return 1
    if not run_stage("png", lambda: check_png_stage(binary, expect, target, outdir, scenario)):
        return 1
    if pdf and (expect.get("png") or {}).get("samples"):
        if not run_stage("pdf", lambda: check_pdf_stage(binary, expect, target, outdir, scenario)):
            return 1
    print(f"LIVE_SKETCH_OK scenario={scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
