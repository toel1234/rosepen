#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless color round-trip corpus checker (02-04).

Runs one SVG fixture through four rosepen export stages and checks the
invariants recorded next to it in <fixture>.expect.json:

  svg    export Inkscape SVG to <outdir>/a.svg, then re-export a.svg to
         <outdir>/sub/b.svg (a different directory). The per-id property and
         "absent" expectations apply to BOTH files, the color properties of
         every expected id must be equal between a.svg and b.svg, and every
         <color-profile> xlink:href in both files must resolve on disk
         (href rebasing on save-elsewhere).
  plain  export plain SVG (--export-plain-svg); the inkscape:* spot/output
         profile attributes are gone while url(#...), stop-color and
         icc-color stay.
  pdf    export PDF; every FlateDecode stream is inflated, bounded by its own
         "endstream" (never regex-scanned blindly: binary streams truncate),
         and concatenated with the uncompressed object text before the
         count/absent regexes run.
  png    export PNG with --export-width and --export-background-opacity=0;
         the PNG is decoded with the stdlib (IHDR, concatenated IDAT, zlib,
         filter types 0-4, RGB8/RGBA8 only) and the requested pixels are
         sampled against [r, g, b] +- tolerance.

Usage:
    check_color_corpus.py <rosepen-binary> <fixture.svg> <expect.json> <outdir>

Prints one "STAGE <name> OK|FAIL: <reason>" line per stage and exits non-zero
on the first FAIL. A non-zero rosepen exit or CRITICAL/g_error output on
stderr is a stage FAIL; a WARNING is only tolerated for fixtures whose
expectation lists it. python3 stdlib only.
"""

import json
import os
import re
import struct
import subprocess
import sys
import zlib
from urllib.parse import unquote, urlparse
from xml.etree import ElementTree as ET

TIMEOUT_S = 120

SVG_NS = "http://www.w3.org/2000/svg"
NS_MAP = {
    "inkscape": "http://www.inkscape.org/namespaces/inkscape",
    "sodipodi": "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd",
    "xlink": "http://www.w3.org/1999/xlink",
}

# Color properties collected for the a.svg/b.svg equality check; style
# declarations always win over presentation attributes.
COLOR_PROPS = ("fill", "stroke", "stop-color", "flood-color", "lighting-color")
OPACITY_PROPS = ("fill-opacity", "stroke-opacity", "stop-opacity", "flood-opacity")


class StageFail(Exception):
    """Raised when a stage invariant is violated; message is the reason."""


def fail(reason):
    raise StageFail(reason)


# ---------------------------------------------------------------- running ---

def run_rosepen(binary, args, warning_ok):
    """Run one rosepen export; non-zero exit, CRITICAL/g_error or an
    un-permitted WARNING on stderr is a FAIL."""
    try:
        proc = subprocess.run([binary] + args, capture_output=True,
                              text=True, timeout=TIMEOUT_S)
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
    if not warning_ok:
        warnings = [ln.strip() for ln in stderr.splitlines() if "WARNING" in ln]
        if warnings:
            fail(f"unexpected WARNING on stderr: {' | '.join(warnings)[:400]}")


def run_export(binary, outdir, target, extra_args, warning_ok):
    """The output dir is reused between runs: remove a stale target first so
    a run that exits 0 without writing can't pass on the previous output."""
    os.makedirs(outdir, exist_ok=True)
    if os.path.lexists(target):
        os.remove(target)
    run_rosepen(binary, extra_args, warning_ok)


# ------------------------------------------------------------ svg reading ---

def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def parse_style(style):
    out = {}
    for declaration in (style or "").split(";"):
        if ":" not in declaration:
            continue
        key, value = declaration.split(":", 1)
        out[key.strip()] = value.strip()
    return out


def attribute_value(el, name):
    """Look up a (possibly prefixed) XML attribute, e.g. inkscape:spot-name."""
    if ":" in name:
        prefix, local = name.split(":", 1)
        uri = NS_MAP.get(prefix)
        if uri is not None:
            return el.get(f"{{{uri}}}{local}")
    return el.get(name)


def prop_value(el, prop):
    """Property value from the style attribute, else the presentation
    (or namespaced) attribute; None if unset. style wins, like CSS."""
    style = parse_style(el.get("style"))
    if prop in style:
        return style[prop]
    return attribute_value(el, prop)


def collect_props(el):
    """All color-relevant properties of one element (style + presentation
    attributes), used for the a.svg/b.svg equality check."""
    props = parse_style(el.get("style"))
    for name in COLOR_PROPS + OPACITY_PROPS:
        value = el.get(name)
        if value is not None:
            props.setdefault(name, value)
    return props


def elements_by_id(root):
    return {el.get("id"): el for el in root.iter() if el.get("id")}


def check_absent(text, absent, label):
    for pattern in absent or []:
        if re.search(pattern, text):
            fail(f"{label}: forbidden pattern present: {pattern}")


def check_id_expectations(root, id_props, label):
    by_id = elements_by_id(root)
    for obj_id, expectations in id_props.items():
        el = by_id.get(obj_id)
        if el is None:
            fail(f"{label}: id '{obj_id}' not found")
        for prop, pattern in expectations.items():
            value = prop_value(el, prop)
            if value is None:
                fail(f"{label}: #{obj_id} has no '{prop}' (style/attribute)")
            if not re.fullmatch(pattern, value):
                fail(f"{label}: #{obj_id} {prop} = '{value}' does not match '{pattern}'")


def check_profile_hrefs(root, path, label):
    """Every <color-profile> href must resolve from the file's own directory
    (catches a save-elsewhere that broke the relative href rebasing)."""
    base = os.path.dirname(os.path.abspath(path))
    for el in root.iter(f"{{{SVG_NS}}}color-profile"):
        href = attribute_value(el, "xlink:href")
        if href is None:
            fail(f"{label}: <color-profile id='{el.get('id')}'> has no xlink:href")
        target = href
        parsed = urlparse(href)
        if parsed.scheme in ("file", ""):
            target = unquote(parsed.path) if parsed.scheme == "file" else href
            if not os.path.isabs(target):
                target = os.path.join(base, target)
        if not os.path.isfile(target):
            fail(f"{label}: color-profile href '{href}' does not resolve to '{target}'")


def apply_svg_expectations(expect, path, label):
    text = read_text(path)
    root = ET.fromstring(text)
    section = expect.get("svg") or {}
    id_props = {k: v for k, v in section.items() if k != "absent"}
    check_absent(text, section.get("absent"), label)
    check_id_expectations(root, id_props, label)
    check_profile_hrefs(root, path, label)
    return {obj_id: collect_props(by_id)
            for obj_id, by_id in elements_by_id(root).items()
            if obj_id in id_props}


# ------------------------------------------------------------- pdf reading ---

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
        # the dict owning this stream ends immediately before 'stream'
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


def apply_pdf_expectations(expect, path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"%PDF"):
        fail("pdf: output does not start with %PDF")
    text = pdf_flatten(data)
    pdf = expect.get("pdf") or {}
    for pattern, wanted in (pdf.get("count") or {}).items():
        found = len(re.findall(pattern, text))
        if found != wanted:
            fail(f"pdf: pattern '{pattern}' found {found}x, expected {wanted}")
    check_absent(text, pdf.get("absent"), "pdf")


# ------------------------------------------------------------- png reading ---

def png_decode(path):
    """(width, height, bytes-per-pixel, unfiltered pixel bytes) for RGB8/RGBA8,
    filter types 0-4; anything else is a FAIL."""
    with open(path, "rb") as f:
        data = f.read()
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
    # With --export-background-opacity=0 an unpainted pixel is fully
    # transparent and its RGB is just the page color: never a color match.
    if bpp == 4 and raw[offset + 3] == 0:
        fail(f"png: sample ({x}, {y}) is fully transparent (nothing painted)")
    return raw[offset:offset + 3]


def apply_png_expectations(expect, path):
    img = png_decode(path)
    width, _height, _bpp, _raw = img
    png = expect.get("png") or {}
    wanted_width = png.get("width")
    if wanted_width is not None and width != wanted_width:
        fail(f"png: width {width}, expected {wanted_width}")
    for sample in png.get("samples") or []:
        x, y, rgb, tol = sample
        actual = png_pixel(img, x, y)
        for channel, (got, want) in enumerate(zip(actual, rgb)):
            if abs(got - want) > tol:
                fail(f"png: sample ({x}, {y}) channel {channel} = {got}, "
                     f"expected {want} +- {tol} (pixel {tuple(actual)} vs {tuple(rgb)})")


# ------------------------------------------------------------ softproof ----

SOFTPROOF_PREFS_XML = (
    '<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
    '<inkscape version="1">\n'
    '  <group id="options">\n'
    '    <group id="softproof" enable="1"/>\n'
    '  </group>\n'
    '</inkscape>\n'
)


def seed_softproof_prefs():
    """Fixture softproof-gamut: enable /options/softproof/enable (see
    src/colors/cms/system.cpp readProofSettings and the group id="softproof"
    enable attribute in src/preferences-skeleton.h) in the isolated test
    profile before the PNG stage. Export must never be proofed."""
    profile = os.environ.get("INKSCAPE_PROFILE_DIR")
    if not profile:
        fail("png: INKSCAPE_PROFILE_DIR not set (softproof seeding needs it)")
    os.makedirs(profile, exist_ok=True)
    with open(os.path.join(profile, "preferences.xml"), "w", encoding="utf-8") as f:
        f.write(SOFTPROOF_PREFS_XML)


# ------------------------------------------------------------------ stages --

def stage_svg(binary, expect, fixture, outdir, warning_ok):
    a_svg = os.path.join(outdir, "a.svg")
    sub = os.path.join(outdir, "sub")
    b_svg = os.path.join(sub, "b.svg")
    run_export(binary, outdir, a_svg, [fixture,
                                f"--export-type=svg",
                                f"--export-filename={a_svg}"], warning_ok)
    if not os.path.isfile(a_svg):
        fail("svg: a.svg was not created")
    run_export(binary, sub, b_svg, [a_svg,
                             "--export-type=svg",
                             f"--export-filename={b_svg}"], warning_ok)
    if not os.path.isfile(b_svg):
        fail("svg: b.svg was not created")
    props_a = apply_svg_expectations(expect, a_svg, "svg a.svg")
    props_b = apply_svg_expectations(expect, b_svg, "svg b.svg")
    for obj_id in sorted(props_a):
        if props_a[obj_id] != props_b[obj_id]:
            fail(f"svg: #{obj_id} color props differ between a.svg "
                 f"{props_a[obj_id]} and b.svg {props_b[obj_id]}")


def stage_plain(binary, expect, fixture, outdir, warning_ok):
    plain_svg = os.path.join(outdir, "plain.svg")
    run_export(binary, outdir, plain_svg, [fixture,
                                "--export-plain-svg",
                                f"--export-filename={plain_svg}"], warning_ok)
    if not os.path.isfile(plain_svg):
        fail("plain: plain.svg was not created")
    text = read_text(plain_svg)
    root = ET.fromstring(text)
    section = expect.get("plain") or {}
    id_props = {k: v for k, v in section.items() if k != "absent"}
    check_absent(text, section.get("absent"), "plain")
    check_id_expectations(root, id_props, "plain")


def stage_pdf(binary, expect, fixture, outdir, warning_ok):
    out_pdf = os.path.join(outdir, "out.pdf")
    run_export(binary, outdir, out_pdf, [fixture,
                                "--export-type=pdf",
                                f"--export-filename={out_pdf}"], warning_ok)
    if not os.path.isfile(out_pdf):
        fail("pdf: out.pdf was not created")
    apply_pdf_expectations(expect, out_pdf)


def stage_png(binary, expect, fixture, outdir, warning_ok):
    if expect.get("_softproof_prefs"):
        seed_softproof_prefs()
    out_png = os.path.join(outdir, "out.png")
    width = (expect.get("png") or {}).get("width")
    if width is None:
        fail("png: expectation has no 'width' (needed for --export-width)")
    run_export(binary, outdir, out_png, [fixture,
                                "--export-type=png",
                                f"--export-width={width}",
                                "--export-background-opacity=0",
                                f"--export-filename={out_png}"], warning_ok)
    if not os.path.isfile(out_png):
        fail("png: out.png was not created")
    apply_png_expectations(expect, out_png)


# -------------------------------------------------------------------- main --

def main(argv):
    if len(argv) != 5:
        print(f"usage: {argv[0]} <rosepen-binary> <fixture.svg> "
              f"<expect.json> <outdir>", file=sys.stderr)
        return 2
    binary, fixture, expect_path, outdir = argv[1:5]

    with open(expect_path, encoding="utf-8") as f:
        expect = json.load(f)

    warning_ok = bool(expect.get("_allow_warning"))
    stages = [("svg", stage_svg), ("plain", stage_plain),
              ("pdf", stage_pdf), ("png", stage_png)]

    for name, stage in stages:
        if expect.get(name) is None:
            print(f"STAGE {name} FAIL: no '{name}' section in {expect_path}")
            return 1
        try:
            stage(binary, expect, fixture, outdir, warning_ok)
        except StageFail as err:
            print(f"STAGE {name} FAIL: {err}")
            return 1
        print(f"STAGE {name} OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
