#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless master-layer render lock (03-02 task 2).

Runs one scenario from master-layer.expect.json against master-layer.svg and
verifies the master-layer model and its rendering through real file I/O:

  model  run `rosepen <fixture> --actions="<scenario actions>;layer-master-list;
         export-filename:<outdir>/<scenario>.svg;export-do"` and compare the
         layer/offset stdout lines with the expectation (positions are 1-based,
         offsets are page-relative user units / millimetres).
  svg    parse the exported SVG: the layers carrying inkscape:master-layer="true"
         must be exactly the expected ids, each must hold exactly the expected
         direct child ids in document order, absent ids must be gone, and the
         <view> page count must match.
  png    for every expected page, export that page of the exported SVG
         (--export-page=N --export-dpi=96 on an opaque white background) and
         sample page-relative millimetre points; each sample must match its
         expected flat sRGB colour within +-2 per channel.
  pdf    (only with --pdf and a "pdf" section) export the exported SVG to PDF
         through capypdf, assert the page count, then re-import each page with
         Rosepen's own PDF importer (--pages=N) and rasterize it to PNG, using
         the same page-relative samples.  No external rasterizer is used.

Usage:
    check_master_layer.py <rosepen> <fixture.svg> <expect.json> <outdir> <scenario> [--pdf]

Prints one "STAGE <name> OK|FAIL: <reason>" line per stage and exits non-zero
on the first FAIL.  A non-zero rosepen exit or CRITICAL/g_error on stderr is a
FAIL; a WARNING is only tolerated when the scenario lists its substring in
"allow_warnings".  python3 stdlib only, `python3 -I` safe.
"""

import json
import os
import re
import struct
import subprocess
import sys
import zlib
from xml.etree import ElementTree as ET

TIMEOUT_S = 120
EPS = 0.01
COLOR_TOL = 2

SVG_NS = "http://www.w3.org/2000/svg"
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
TAG_VIEW = f"{{{SVG_NS}}}view"
TAG_GROUP = f"{{{SVG_NS}}}g"
ATTR_LABEL = f"{{{INKSCAPE_NS}}}label"
ATTR_MASTER = f"{{{INKSCAPE_NS}}}master-layer"
ATTR_GROUP_MODE = f"{{{INKSCAPE_NS}}}groupmode"

# Page-relative sample points in fixture millimetres (1 user unit = 1 mm).
# S_bg, S_hdr, S_content, S_badge, S_out and S_wide from the plan.
SAMPLE_POINTS = {
    "bg": (20.0, 80.0),
    "hdr": (50.0, 14.0),
    "content": (33.0, 33.0),
    "badge": (43.0, 43.0),
    "out": (2.0, 2.0),
    "wide": (120.0, 50.0),
}

EXPORT_BACKGROUND = ["--export-background=#ffffff", "--export-background-opacity=1"]

# The page label / messages are only used positionally; force the C locale.
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
    return proc.stdout or ""


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


def load_svg(path):
    try:
        return ET.parse(path).getroot()
    except (ET.ParseError, OSError) as err:
        fail(f"cannot parse exported SVG {path}: {err}")


def svg_pages(root):
    """(x, y, w, h) of every <view> element in document order."""
    return [tuple(parse_viewbox(view.get("viewBox")))
            for view in root.iter() if view.tag == TAG_VIEW]


def elements_by_id(root):
    return {el.get("id"): el for el in root.iter() if el.get("id")}


def check_model_stage(expect, stdout):
    model = expect.get("model") or {}
    lines = [ln for ln in (stdout or "").splitlines()
             if ln.startswith("layer\t") or ln.startswith("offset\t")]
    got_layers = [ln for ln in lines if ln.startswith("layer\t")]
    got_offsets = [ln for ln in lines if ln.startswith("offset\t")]
    want_layers = model.get("layers") or []
    want_offsets = model.get("offsets") or []
    if got_layers != want_layers:
        fail(f"master layer lines are {got_layers}, expected {want_layers}")
    if got_offsets != want_offsets:
        fail(f"offset lines are {got_offsets}, expected {want_offsets}")


def check_svg_stage(expect, path):
    svg = expect.get("svg") or {}
    root = load_svg(path)
    by_id = elements_by_id(root)

    pages = svg_pages(root)
    if "pages" in svg and len(pages) != svg["pages"]:
        fail(f"exported SVG has {len(pages)} <view> pages, expected {svg['pages']}")

    wanted_layers = svg.get("master_layers") or []
    got_layers = []
    for el in root:
        if el.get(ATTR_MASTER) == "true":
            if el.tag != TAG_GROUP or el.get(ATTR_GROUP_MODE) != "layer":
                fail(f"master flag on a non-layer element id={el.get('id')!r}")
            got_layers.append(el.get("id"))
    if got_layers != wanted_layers:
        fail(f"master layers are {got_layers}, expected {wanted_layers}")

    for layer_id, wanted_children in (svg.get("children") or {}).items():
        el = by_id.get(layer_id)
        if el is None:
            fail(f"master layer '{layer_id}' is missing from the exported SVG")
        got_children = [child.get("id") for child in el
                        if child.get("id") and child.tag != TAG_VIEW]
        if got_children != wanted_children:
            fail(f"master layer '{layer_id}' holds {got_children}, expected {wanted_children}")

    for absent in (svg.get("absent") or []):
        if absent in by_id:
            fail(f"id '{absent}' must be absent but is present")

    for ident, wanted in (svg.get("counts") or {}).items():
        found = sum(1 for el in root.iter() if el.get("id") == ident)
        if found != wanted:
            fail(f"id '{ident}' occurs {found} times, expected {wanted}")


# ------------------------------------------------------------- png reading ---

def png_decode(path):
    """(width, height, bytes-per-pixel, unfiltered pixel bytes) for RGB8/RGBA8,
    filter types 0-4; anything else is a FAIL."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as err:
        fail(f"cannot read PNG {path}: {err}")
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("png: bad PNG signature")
    pos = 8
    ihdr = None
    idat = b""
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # header + data + CRC
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if ihdr is None:
        fail("png: no IHDR chunk")
    width, height, depth, color, _comp, _filt, interlace = ihdr
    if interlace != 0:
        fail("png: interlaced PNG not supported")
    if depth != 8:
        fail(f"png: bit depth {depth} not supported (8 only)")
    if color == 6:
        bpp = 4
    elif color == 2:
        bpp = 3
    else:
        fail(f"png: color type {color} not supported (RGB8/RGBA8 only)")
    try:
        raw = zlib.decompress(idat)
    except zlib.error as err:
        fail(f"png: IDAT does not inflate: {err}")
    stride = width * bpp
    if len(raw) != (stride + 1) * height:
        fail(f"png: decompressed size {len(raw)} != {(stride + 1) * height}")
    out = bytearray(stride * height)
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        ftype = raw[src]
        src += 1
        line = bytearray(raw[src:src + stride])
        src += stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xFF
        elif ftype == 2:  # Up
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ftype == 3:  # Average
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        else:
            fail(f"png: filter type {ftype} not supported (0-4 only)")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, bpp, bytes(out)


def png_pixel(img, x, y):
    width, height, bpp, raw = img
    if not (0 <= x < width and 0 <= y < height):
        fail(f"png: sample ({x}, {y}) outside the {width}x{height} image")
    offset = y * width * bpp + x * bpp
    if bpp == 4 and raw[offset + 3] == 0:
        fail(f"png: sample ({x}, {y}) is fully transparent (nothing painted)")
    return raw[offset:offset + 3]


def sample_png(path, page, colors, samples, page_rect):
    """Sample page-relative millimetre points on a decoded PNG."""
    img = png_decode(path)
    width, height, _bpp, _raw = img
    _px, _py, page_w, page_h = page_rect
    got = {}
    for name, color_name in samples.items():
        if name not in SAMPLE_POINTS:
            fail(f"unknown sample '{name}'")
        x_mm, y_mm = SAMPLE_POINTS[name]
        x = int(x_mm / page_w * width)
        y = int(y_mm / page_h * height)
        actual = png_pixel(img, x, y)
        wanted = colors[color_name]
        if any(abs(g - w) > COLOR_TOL for g, w in zip(actual, wanted)):
            got[name] = (tuple(actual), color_name, tuple(wanted))
    if got:
        details = ", ".join(f"{name}={actual} (expected {color_name} {wanted})"
                            for name, (actual, color_name, wanted) in got.items())
        fail(f"page {page}: wrong pixel colours: {details}")


# ----------------------------------------------------------------- stages ---

def check_png_stage(binary, expect, target, outdir, tag):
    pages = svg_pages(load_svg(target))
    colors = expect["colors"]
    for entry in expect["png"]:
        page = entry["page"]
        if not 1 <= page <= len(pages):
            fail(f"page {page} does not exist in the exported SVG ({len(pages)} pages)")
        path = os.path.join(outdir, f"{tag}-p{page}.png")
        delete_stale(path)
        run_rosepen(binary, [target, f"--export-page={page}", "--export-type=png",
                             "--export-dpi=96"] + EXPORT_BACKGROUND +
                    [f"--export-filename={path}"],
                    expect.get("allow_warnings") or [])
        sample_png(path, page, colors, entry["samples"], pages[page - 1])


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


def check_pdf_stage(binary, expect, target, outdir, tag):
    pdf_expect = expect["pdf"]
    pdf_path = os.path.join(outdir, tag + ".pdf")
    delete_stale(pdf_path)
    run_rosepen(binary, ["--export-extension=org.inkscape.output.pdf.capypdf",
                         f"--export-filename={pdf_path}", target],
                expect.get("allow_warnings") or [])
    try:
        with open(pdf_path, "rb") as f:
            data = f.read()
    except OSError as err:
        fail(f"cannot read PDF {pdf_path}: {err}")
    if not data.startswith(b"%PDF"):
        fail("pdf: output does not start with %PDF")
    text = pdf_flatten(data)
    wanted_pages = pdf_expect.get("pages")
    found = len(re.findall(r"/Type\s*/Page(?![a-zA-Z])", text))
    if wanted_pages is not None and found != wanted_pages:
        fail(f"pdf: found {found} /Type /Page objects, expected {wanted_pages}")

    # Rosepen's own importer is the rasterizer; a failure here is a finding.
    pages = svg_pages(load_svg(target))
    colors = expect["colors"]
    for entry in pdf_expect.get("png") or []:
        page = entry["page"]
        path = os.path.join(outdir, f"{tag}-pdf{page}.png")
        delete_stale(path)
        run_rosepen(binary, [pdf_path, f"--pages={page}", "--export-type=png",
                             "--export-dpi=96"] + EXPORT_BACKGROUND +
                    [f"--export-filename={path}"],
                    expect.get("allow_warnings") or [])
        if not 1 <= page <= len(pages):
            fail(f"pdf page {page} has no matching source page")
        sample_png(path, page, colors, entry["samples"], pages[page - 1])


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


# -------------------------------------------------------------------- main ---

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
    expect["colors"] = document_expect.get("colors") or {}

    target = os.path.join(outdir, scenario + ".svg")
    delete_stale(target)

    actions = ""
    if expect.get("actions"):
        actions = expect["actions"] + ";"
    actions += f"layer-master-list;export-filename:{target};export-do"

    def stage_model():
        return run_rosepen(binary, [fixture, f"--actions={actions}"],
                           expect.get("allow_warnings") or [])

    try:
        stdout = stage_model()
    except StageFail as err:
        print(f"STAGE model FAIL: {err}")
        return 1
    if not run_stage("model", lambda: check_model_stage(expect, stdout)):
        return 1
    if not run_stage("svg", lambda: check_svg_stage(expect, target)):
        return 1
    if not run_stage("png", lambda: check_png_stage(binary, expect, target, outdir, scenario)):
        return 1
    if pdf and expect.get("pdf"):
        if not run_stage("pdf", lambda: check_pdf_stage(binary, expect, target, outdir, scenario)):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
