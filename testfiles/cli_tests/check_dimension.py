#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless lock for the Rosepen 05-02 live dimensions.

Per scenario: drive app.dimension-add / app.dimension-set (plus plain object
actions) on the fixture, then prove the result on three stages:

  list  app.dimension-list TSV: id, kind, value (+-1e-3) and label text
  svg   plain-SVG export: dimension group count, label text/x, geometry bbox
  png   PNG export: dark/light pixel samples given in page millimetres
  pdf   with --pdf: capypdf export re-imported through Rosepen's own PDF
        importer, then the same pixel samples

Prints one "STAGE <name> OK|FAIL: <reason>" line per stage and a final
"DIMENSION_OK scenario=<s>"; exits non-zero on the first FAIL.
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
COLOR_TOL = 60
EPS = 0.05
VALUE_EPS = 1e-3
EXPORT_BACKGROUND = ["--export-background=#ffffff", "--export-background-opacity=1"]

# The page label / messages are only used positionally; force the C locale.
ROSEPEN_ENV = dict(os.environ, LC_ALL="C", LANG="C", LANGUAGE="")


def fail(reason):
    raise RuntimeError(reason)


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
    if "stderr_contains" in allow_warnings:
        pass  # handled by the caller
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


def tag_of(node):
    return node.tag.rsplit("}", 1)[-1]


def iter_all(node):
    yield node
    for child in node:
        yield from iter_all(child)


def has_class(node, needle):
    return needle in (node.get("class") or "")


def parse_numbers(d):
    return [float(tok) for tok in re.findall(r"[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?", d)]


def path_bbox(d):
    """Exact bbox for axis-aligned straight-line paths (our linear
    dimensions); a coarse upper bound otherwise. Supports M/L/H/V/C/Z in
    absolute and relative forms."""
    tokens = re.findall(r"[MmLlHhVvCcZz]|[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?", d)
    x = y = 0.0
    sx = sy = 0.0
    pts = []
    i = 0
    cmd = None
    while i < len(tokens):
        tok = tokens[i]
        if re.match(r"[A-Za-z]", tok):
            cmd = tok
            i += 1
            if cmd in "Zz":
                pts.append((sx, sy))
                x, y = sx, sy
            continue
        if cmd in "Mm":
            dx, dy = float(tokens[i]), float(tokens[i + 1])
            i += 2
            x = (x + dx) if cmd == "m" else dx
            y = (y + dy) if cmd == "m" else dy
            sx, sy = x, y
            pts.append((x, y))
            cmd = "L" if cmd == "M" else "l"
        elif cmd in "Ll":
            dx, dy = float(tokens[i]), float(tokens[i + 1])
            i += 2
            x = (x + dx) if cmd == "l" else dx
            y = (y + dy) if cmd == "l" else dy
            pts.append((x, y))
        elif cmd in "Hh":
            dx = float(tokens[i])
            i += 1
            x = (x + dx) if cmd == "h" else dx
            pts.append((x, y))
        elif cmd in "Vv":
            dy = float(tokens[i])
            i += 1
            y = (y + dy) if cmd == "v" else dy
            pts.append((x, y))
        elif cmd in "Cc":
            for k in range(3):
                dx, dy = float(tokens[i]), float(tokens[i + 1])
                i += 2
                px = (x + dx) if cmd == "c" else dx
                py = (y + dy) if cmd == "c" else dy
                pts.append((px, py))
            x, y = pts[-1]
        else:
            i += 1
    if not pts:
        fail("line path has no coordinates")
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def check_list_stage(expect, stdout):
    rows = []
    for line in stdout.splitlines():
        fields = line.split("\t")
        if len(fields) == 4:
            rows.append(fields)
    wanted = expect.get("list") or []
    if len(rows) != len(wanted):
        fail(f"dimension-list has {len(rows)} rows, expected {len(wanted)}: {rows}")
    for row, want in zip(rows, wanted):
        rid, kind, value, label = row
        if rid != want["id"]:
            fail(f"id is {rid!r}, expected {want['id']!r}")
        if kind != want["kind"]:
            fail(f"kind is {kind!r}, expected {want['kind']!r}")
        if abs(float(value) - want["value"]) > VALUE_EPS:
            fail(f"value is {value}, expected {want['value']}")
        if label != want["label"]:
            fail(f"label is {label!r}, expected {want['label']!r}")


def check_svg_stage(expect, target):
    svg_expect = expect.get("svg") or {}
    try:
        tree = ET.parse(target)
    except ET.ParseError as err:
        fail(f"exported SVG does not parse: {err}")
    root = tree.getroot()
    dims = [n for n in iter_all(root) if tag_of(n) == "g" and has_class(n, "rosepen-dimension")]
    if len(dims) != svg_expect.get("dimensions", 1):
        fail(f"found {len(dims)} dimension groups, expected {svg_expect.get('dimensions', 1)}")
    if not dims:
        return
    group = dims[0]
    texts = [n for n in group.iter() if tag_of(n) == "text" and has_class(n, "rosepen-dimension-label")]
    if len(texts) != 1:
        fail(f"found {len(texts)} label texts, expected 1")
    label = texts[0]
    content = "".join(t.text or "" for t in label.iter() if tag_of(t) == "tspan")
    if "label" in svg_expect and content != svg_expect["label"]:
        fail(f"label text is {content!r}, expected {svg_expect['label']!r}")
    if "label_x" in svg_expect:
        lx = float(label.get("x", "nan"))
        if abs(lx - svg_expect["label_x"]) > EPS:
            fail(f"label x is {lx}, expected {svg_expect['label_x']}")
    if "bbox" in svg_expect:
        paths = [n for n in group.iter() if tag_of(n) == "path" and has_class(n, "rosepen-dimension-line")]
        if len(paths) != 1:
            fail(f"found {len(paths)} dimension line paths, expected 1")
        if paths[0].get("{http://www.inkscape.org/namespaces/inkscape}original-d") is None:
            fail("dimension line path lost inkscape:original-d")
        got = path_bbox(paths[0].get("d", ""))
        want = svg_expect["bbox"]
        for g, w, name in zip(got, want, "minx miny maxx maxy".split()):
            if abs(g - w) > EPS:
                fail(f"line bbox {name} is {g:.3f}, expected {w}")


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


def png_pixel(png, x_mm, y_mm, page_w_mm=200.0, page_h_mm=100.0):
    width, height, channels, pixels = png_decode(png)
    px = min(width - 1, max(0, int(round(x_mm / page_w_mm * width))))
    py = min(height - 1, max(0, int(round(y_mm / page_h_mm * height))))
    best = 255
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            xx = min(width - 1, max(0, px + dx))
            yy = min(height - 1, max(0, py + dy))
            offset = (yy * width + xx) * channels
            value = min(pixels[offset:offset + 3]) if channels >= 3 else pixels[offset]
            best = min(best, value)
    return best


def check_png_samples(png, samples, page_w_mm=200.0, page_h_mm=100.0):
    for sample in samples:
        value = png_pixel(png, sample["x"], sample["y"], page_w_mm, page_h_mm)
        if sample.get("dark") and value > 255 - COLOR_TOL - 60:
            fail(f"sample ({sample['x']},{sample['y']}) mm is not dark (value {value})")
        if not sample.get("dark") and value <= 255 - COLOR_TOL - 60:
            fail(f"sample ({sample['x']},{sample['y']}) mm is not light (value {value})")


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
    binary, fixture, expect_path, outdir, scenario = rest

    with open(expect_path, encoding="utf-8") as f:
        document_expect = json.load(f)
    expect = document_expect.get("scenarios", {}).get(scenario)
    if expect is None:
        print(f"STAGE setup FAIL: scenario '{scenario}' not in {expect_path}")
        return 1
    allow = expect.get("allow_warnings") or []

    target = os.path.join(outdir, scenario + ".svg")
    delete_stale(target)

    actions = expect["actions"] + ";"

    if not run_stage("list", lambda: check_list_stage(
            expect, run_rosepen(binary, [fixture, f"--actions={actions}dimension-list"], allow)[0])):
        return 1
    if not run_stage("svg", lambda: (
            run_rosepen(binary, [fixture, f"--actions={actions}export-filename:{target};export-do"], allow),
            check_svg_stage(expect, target))):
        return 1
    if not run_stage("png", lambda: check_png_stage(binary, expect, target, outdir, scenario)):
        return 1
    if pdf and (expect.get("png") or {}).get("samples"):
        if not run_stage("pdf", lambda: check_pdf_stage(binary, expect, target, outdir, scenario)):
            return 1
    print(f"DIMENSION_OK scenario={scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
