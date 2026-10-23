#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless lock for the drawing import path (06-01).

Every scenario in cdr-import.expect.json opens one in-tree fixture (or a copy
of it derived at test time) through rosepen and proves the import headlessly:

  prepare    for "derived:" inputs only: rebuild a multi-page copy of the
             single-page fixture (duplicate the page record inside the
             compressed drawing stream, recompute the container sizes and
             neutralise the source-path credit strings the fixture carries),
             then assert the derived copy has the requested number of page
             records and none of the masked strings
  run        the scenario's command line (argv export or actions); exit code,
             fatal output and the ordered "stderr_contains" are checked here
  page_list  a second run with the page-list action: line count and page sizes
  svg        plain or Inkscape SVG export: colour-profile count, top-level
             "Page N" layers, page elements, and the number of flat fill/stroke
             paints outside defs with and without icc-color(); for the CMYK
             scenario the same count is derived from a profile-less baseline
             run so the expectation stays relational
  png        PNG export at 96 dpi on white; non-blank coverage, per-page
             equality for the multi-page case and the mean per-channel
             difference against a profile-less baseline for the CMYK case
  pdf        with --pdf: the capypdf export has one page object per page and,
             when the scenario sets "cmyk_ops_min", at least that many CMYK
             colour operators (four-operand k/K, or sc/scn painting through a
             DeviceCMYK / 4-component ICCBased colour space)

Prints one "STAGE <name> OK|FAIL|SKIP[: <reason>]" line per stage and a final
"CDR_IMPORT_OK scenario=<s>"; exits non-zero on the first FAIL. python3 stdlib
only (runs under python3 -I).

Fixture paths are relative to <testfiles_dir>/cli_tests/testcases/; profile
paths are relative to <testfiles_dir>. Every rosepen run gets its own fresh
INKSCAPE_PROFILE_DIR so a seeded preference cannot leak between runs.
"""

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib
import xml.etree.ElementTree as ET
from xml.sax.saxutils import quoteattr

TIMEOUT_S = 300
SVG_NS = "http://www.w3.org/2000/svg"
INK_NS = "http://www.inkscape.org/namespaces/inkscape"
XLINK_NS = "http://www.w3.org/1999/xlink"
EXPORT_BG = ["--export-background=#ffffff", "--export-background-opacity=1"]

# Messages are compared positionally; force the C locale.
ROS = dict(os.environ, LC_ALL="C", LANG="C", LANGUAGE="")

PREFERENCES_XML = (
    '<?xml version="1.0"?>'
    '<inkscape xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" '
    'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" version="1">'
    '<group id="options"><group id="document"><group id="outputprofile" '
    'uri=%s intent="0"/></group></group></inkscape>'
)


class StageFail(Exception):
    """Raised when a stage invariant is violated; message is the reason."""


def fail(reason):
    raise StageFail(reason)


# ------------------------------------------------------------------- running --

def run_rosepen(binary, args, profile_dir, expect_rc, allow_warnings, contains=()):
    """Run one rosepen command with a private profile; returns (stdout, stderr)."""
    try:
        proc = subprocess.run([binary] + args, capture_output=True, text=True,
                              timeout=TIMEOUT_S, env=dict(ROS, INKSCAPE_PROFILE_DIR=profile_dir))
    except subprocess.TimeoutExpired:
        fail(f"rosepen timed out after {TIMEOUT_S}s: {' '.join(args)}")
    except OSError as err:
        fail(f"cannot run rosepen: {err}")
    stdout = proc.stdout or ""
    stderr = proc.stderr or ""
    if proc.returncode != expect_rc:
        tail = " ".join(stderr.split())[-400:]
        fail(f"rosepen exited {proc.returncode}, expected {expect_rc} "
             f"({' '.join(args)}): {tail}")
    fatal = [ln for ln in stderr.splitlines()
             if "CRITICAL" in ln or re.search(r"ERROR \*\*", ln)]
    if fatal:
        fail(f"rosepen reported a fatal error: {' | '.join(fatal)[:400]}")
    for ln in stderr.splitlines():
        if "WARNING" in ln and not any(allowed in ln for allowed in allow_warnings):
            fail(f"unexpected WARNING on stderr: {ln.strip()[:400]}")
    position = 0
    for needle in contains:
        found = stderr.find(needle, position)
        if found < 0:
            fail(f"stderr lacks {needle!r} (in order)")
        position = found + len(needle)
    return stdout, stderr


def fresh_profile(outdir, tag, profile_uri):
    """A brand new profile directory per run, optionally seeding the preferred
    output profile. Without a uri the preferences file stays absent (defaults)."""
    path = os.path.join(outdir, "profile-" + tag)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path)
    if profile_uri:
        with open(os.path.join(path, "preferences.xml"), "w", encoding="utf-8") as f:
            f.write(PREFERENCES_XML % quoteattr(profile_uri))
    return path


def delete_stale(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.lexists(path):
        os.remove(path)


# -------------------------------------------------------------------- paths ---

def fixtures_base(testfiles_dir):
    return os.path.join(testfiles_dir, "cli_tests", "testcases")


def resolve_input(testfiles_dir, value):
    if value.startswith("derived:"):
        return value
    return os.path.abspath(os.path.join(fixtures_base(testfiles_dir), value))


def resolve_profile(testfiles_dir, value):
    if value is None:
        return None
    if os.path.isabs(value):
        return value
    return os.path.abspath(os.path.join(testfiles_dir, value))


def placeholder(value, testfiles_dir, outdir):
    return value.format(fixtures=fixtures_base(testfiles_dir), outdir=outdir)


# ---------------------------------------------------------------- derivation --

def riff_children(buf, start, end):
    off = start
    while off + 8 <= end:
        fourcc = buf[off:off + 4]
        size = struct.unpack("<I", buf[off + 4:off + 8])[0]
        body = off + 8
        if body + size > end:
            raise StageFail("derivation: a container chunk overruns its parent")
        yield off, fourcc, size, body
        off = body + size + (size & 1)


def derive_multipage(source, target, pages):
    """Rebuild the single drawing page of the fixture `pages` times.

    The drawing lives in one zlib-compressed `cmpr` list whose record lengths
    are indexes into a second, compressed length table; duplicating a page
    record verbatim keeps those indexes valid. The parser-ignored `osfp` chunk
    carries source-path credit strings: every printable run there is replaced
    with neutral bytes and the removed strings are asserted to be gone.
    """
    data = open(source, "rb").read()
    if data[:4] != b"RIFF":
        fail("derivation: the fixture is not a RIFF container")
    riff_end = 8 + struct.unpack("<I", data[4:8])[0]

    drawing = None
    for off, fourcc, size, body in riff_children(data, 12, riff_end):
        if fourcc != b"LIST" or data[body:body + 4] != b"cmpr":
            continue
        compressed = struct.unpack("<I", data[body + 4:body + 8])[0]
        if drawing is None or compressed > drawing[3]:
            drawing = (off, size, body, compressed)
    if drawing is None:
        fail("derivation: no compressed drawing chunk in the fixture")
    off, size, body, compressed = drawing
    if data[body + 20:body + 24] != b"CPng":
        fail("derivation: unexpected compressed-chunk header")

    stream = body + 28
    raw = zlib.decompress(data[stream:stream + compressed])
    block_bytes = zlib.decompress(data[stream + compressed:body + size])
    blocks = [struct.unpack("<I", block_bytes[i:i + 4])[0]
              for i in range(0, len(block_bytes) - 3, 4)]

    def records(start, end):
        found = []
        pos = start
        while pos + 8 <= end:
            fourcc = raw[pos:pos + 4]
            length = struct.unpack("<I", raw[pos + 4:pos + 8])[0]
            length = blocks[length] if len(blocks) > length else length
            found.append((pos, fourcc, length))
            pos += 8 + length
        return found

    top = records(0, len(raw))
    page_records = [(pos, length) for pos, fourcc, length in top
                    if fourcc == b"LIST" and raw[pos + 8:pos + 12] == b"page"]
    if not page_records:
        fail("derivation: the drawing stream has no page record")

    def has_drawing(pos, length):
        blob = raw[pos:pos + 8 + length]
        index = 0
        while True:
            index = blob.find(b"LIST", index)
            if index < 0:
                return False
            if blob[index + 8:index + 12] in (b"obj ", b"grp "):
                return True
            index += 4

    drawing_pages = [page for page in page_records if has_drawing(*page)]
    if not drawing_pages:
        fail("derivation: no page carries drawing objects")

    page_pos, page_length = drawing_pages[-1]
    page_bytes = raw[page_pos:page_pos + 8 + page_length]
    new_raw = raw[:page_records[0][0]] + page_bytes * pages

    rebuilt = zlib.compress(new_raw, 9)
    chunk = (b"cmpr" + struct.pack("<I", len(rebuilt)) + data[body + 8:body + 20] +
             b"CPng" + struct.pack("<H", 1) + struct.pack("<H", 4) +
             rebuilt + data[stream + compressed:body + size])
    padding = b"\0" if len(chunk) % 2 else b""
    out = bytearray(data[:off] + b"LIST" + struct.pack("<I", len(chunk)) + chunk +
                    padding + data[off + 8 + size + (size & 1):])
    out[4:8] = struct.pack("<I", len(out) - 8)

    # Neutralise the printable runs of the parser-ignored source-properties chunk.
    masked = []
    end = 8 + struct.unpack("<I", out[4:8])[0]
    for pos, fourcc, length, chunk_body in riff_children(out, 12, end):
        if fourcc != b"osfp":
            continue
        for match in re.finditer(rb"[ -~]{3,}", bytes(out[chunk_body:chunk_body + length])):
            run = bytes(match.group())
            masked.append(run)
            start = chunk_body + match.start()
            out[start:start + len(run)] = b"\0" * len(run)

    result = bytes(out)
    for run in masked:
        if run in result:
            fail("derivation: a neutralised source-path string survived")
    for _pos, fourcc, length, chunk_body in riff_children(result, 12,
                                                         8 + struct.unpack("<I", result[4:8])[0]):
        if fourcc == b"osfp" and re.search(rb"[ -~]{3,}",
                                           result[chunk_body:chunk_body + length]):
            fail("derivation: the source-properties chunk still has printable text")
    # The words of the masked strings must not survive anywhere else in the file.
    folded = result.lower()
    for run in masked:
        for word in re.findall(rb"[A-Za-z]{5,}", run):
            if word.lower() in folded:
                fail("derivation: a word of a neutralised string occurs elsewhere in the file")

    with open(target, "wb") as f:
        f.write(result)

    # Structural proof that the rebuilt stream has the requested page records.
    if new_raw.count(b"page") < pages:
        fail("derivation: rebuilt stream lacks page records")
    return target


# ------------------------------------------------------------- svg inspection --

def parse_style(style):
    out = {}
    for declaration in (style or "").split(";"):
        if ":" in declaration:
            key, value = declaration.split(":", 1)
            out[key.strip()] = value.strip()
    return out


def tag_of(node):
    return node.tag.rsplit("}", 1)[-1]


def paint_stats(path):
    """(root, flat paint values, values carrying an icc-color) outside <defs>."""
    root = ET.parse(path).getroot()
    flat = []
    with_icc = []

    def walk(node, in_defs):
        local = tag_of(node)
        if local == "defs":
            in_defs = True
        if not in_defs:
            style = parse_style(node.get("style"))
            for prop in ("fill", "stroke"):
                value = style.get(prop, node.get(prop))
                if value is None:
                    continue
                value = value.strip()
                if (not value or value.startswith("url(") or value in (
                        "none", "currentColor", "inherit", "context-fill", "context-stroke")):
                    continue
                # Any other value is a flat colour (#hex, rgb(), a keyword).
                flat.append(value)
                if "icc-color(" in value:
                    with_icc.append(value)
        for child in node:
            walk(child, in_defs)

    walk(root, False)
    return root, flat, with_icc


def count_elements(root, local):
    return sum(1 for node in root.iter() if tag_of(node) == local)


def output_profile(root):
    for node in root.iter():
        if tag_of(node) == "namedview":
            return node.get("{%s}output-profile" % INK_NS)
    return None


def layer_labels(root):
    return [node.get("{%s}label" % INK_NS) for node in root
            if node.get("{%s}groupmode" % INK_NS) == "layer"]


def check_svg(expect, path, label, baseline_flat=None):
    root, flat, with_icc = paint_stats(path)
    wanted_profiles = expect.get("color_profiles")
    if wanted_profiles is not None:
        found = count_elements(root, "color-profile")
        if found != wanted_profiles:
            fail(f"{label}: {found} color-profile elements, expected {wanted_profiles}")
    wanted_pages = expect.get("page_elements")
    if wanted_pages is not None:
        found = count_elements(root, "view")
        if found != wanted_pages:
            fail(f"{label}: {found} page elements, expected {wanted_pages}")
    wanted_flat = expect.get("flat_paints")
    if wanted_flat is not None and len(flat) != wanted_flat:
        fail(f"{label}: {len(flat)} flat colour paints, expected {wanted_flat}")
    wanted_icc = expect.get("icc_paints")
    if wanted_icc is not None and len(with_icc) != wanted_icc:
        fail(f"{label}: {len(with_icc)} paints with icc-color, expected {wanted_icc}")
    if expect.get("require_icc_on_all_flat") and len(with_icc) != len(flat):
        fail(f"{label}: {len(flat) - len(with_icc)} of {len(flat)} flat paints "
             f"are not icc-color")
    if expect.get("baseline_flat_paints") and baseline_flat is not None:
        if len(flat) != baseline_flat:
            fail(f"{label}: {len(flat)} flat paints, the profile-less baseline "
                 f"has {baseline_flat}")
    wanted_name = expect.get("icc_name")
    if wanted_name:
        needle = "icc-color(" + wanted_name + ","
        for value in with_icc:
            if needle not in value:
                fail(f"{label}: paint is not an icc-color of {wanted_name}: {value}")
    suffix = expect.get("profile_href")
    if suffix:
        hrefs = []
        for node in root.iter():
            if tag_of(node) != "color-profile":
                continue
            href = node.get("{%s}href" % XLINK_NS) or node.get("href")
            if href:
                hrefs.append(href)
        if not any(href.endswith(suffix) for href in hrefs):
            fail(f"{label}: no color-profile href ends with {suffix}: {hrefs}")
    wanted_profile = expect.get("output_profile")
    if wanted_profile:
        found = output_profile(root)
        if not found:
            fail(f"{label}: no inkscape:output-profile on the namedview")
        if isinstance(wanted_profile, str) and found != wanted_profile:
            fail(f"{label}: output profile {found!r}, expected {wanted_profile!r}")
    wanted_layers = expect.get("layers")
    if wanted_layers is not None:
        found = layer_labels(root)
        if found != wanted_layers:
            fail(f"{label}: top-level layers {found}, expected {wanted_layers}")
    for pattern in expect.get("absent") or []:
        text = open(path, encoding="utf-8", errors="replace").read()
        if re.search(pattern, text):
            fail(f"{label}: forbidden pattern present: {pattern}")


# ------------------------------------------------------------------ png reading --

def png_decode(path):
    """(width, height, channels, pixel bytes) for 8-bit RGB/RGBA, no interlacing."""
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
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if ihdr is None:
        fail("png: no IHDR chunk")
    width, height, depth, color, _comp, _filt, interlace = ihdr
    if interlace != 0 or depth != 8 or color not in (2, 6):
        fail(f"png: unsupported format (depth {depth}, color {color})")
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    if len(raw) != (stride + 1) * height:
        fail("png: unexpected decompressed size")
    out = bytearray(stride * height)
    previous = bytearray(stride)
    src = 0
    for y in range(height):
        ftype = raw[src]
        src += 1
        line = bytearray(raw[src:src + stride])
        src += stride
        if ftype == 1:
            for x in range(channels, stride):
                line[x] = (line[x] + line[x - channels]) & 0xFF
        elif ftype == 2:
            for x in range(stride):
                line[x] = (line[x] + previous[x]) & 0xFF
        elif ftype == 3:
            for x in range(stride):
                left = line[x - channels] if x >= channels else 0
                line[x] = (line[x] + ((left + previous[x]) >> 1)) & 0xFF
        elif ftype == 4:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                b = previous[x]
                c = previous[x - channels] if x >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        elif ftype != 0:
            fail(f"png: filter type {ftype} not supported")
        out[y * stride:(y + 1) * stride] = line
        previous = line
    return width, height, channels, bytes(out)


def nonwhite_percent(path):
    width, height, channels, pixels = png_decode(path)
    painted = 0
    for i in range(width * height):
        offset = i * channels
        if pixels[offset:offset + 3] != b"\xff\xff\xff":
            painted += 1
    return 100.0 * painted / (width * height)


def mean_channel_difference(path_a, path_b):
    wa, ha, ba, pa = png_decode(path_a)
    wb, hb, bb, pb = png_decode(path_b)
    if (wa, ha) != (wb, hb):
        fail(f"png: {os.path.basename(path_a)} is {wa}x{ha}, "
             f"{os.path.basename(path_b)} is {wb}x{hb}")
    total = 0
    for i in range(wa * ha):
        oa = i * ba
        ob = i * bb
        total += (abs(pa[oa] - pb[ob]) + abs(pa[oa + 1] - pb[ob + 1]) +
                  abs(pa[oa + 2] - pb[ob + 2]))
    return total / (wa * ha * 3)


# ---------------------------------------------------------------- pdf reading --

def pdf_inflated_text(path):
    """The PDF's objects and stream bodies as text, inflating every FlateDecode
    stream first (a page object may live inside a compressed object stream)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"%PDF"):
        fail("pdf: output does not start with %PDF")
    parts = []
    pos = 0
    while True:
        start = data.find(b"stream", pos)
        if start == -1:
            break
        parts.append(data[pos:start].decode("latin-1", "replace"))
        match = re.match(rb"stream(\r\n|\n|\r)", data[start:start + 8])
        body_start = start + (match.end() if match else len(b"stream"))
        end = data.find(b"endstream", body_start)
        if end == -1:
            fail("pdf: 'stream' without 'endstream'")
        body = data[body_start:end]
        window = data[max(pos, start - 4096):start].decode("latin-1", "replace")
        if "/FlateDecode" in window:
            for offset in range(0, min(64, len(body))):
                try:
                    parts.append(zlib.decompress(body[offset:]).decode("latin-1", "replace"))
                    break
                except zlib.error:
                    continue
            else:
                fail("pdf: a FlateDecode stream failed to inflate")
        pos = end + len(b"endstream")
    parts.append(data[pos:].decode("latin-1", "replace"))
    return "\n".join(parts)


CMYK_OPERANDS = re.compile(
    r"(?<![-0-9.])"
    r"([-+]?[0-9]*\.?[0-9]+)\s+([-+]?[0-9]*\.?[0-9]+)\s+"
    r"([-+]?[0-9]*\.?[0-9]+)\s+([-+]?[0-9]*\.?[0-9]+)\s+"
    r"[kK](?![a-zA-Z0-9])")
CS_OPERATORS = re.compile(r"(?<![a-zA-Z0-9/])/[A-Za-z0-9]+\s+[cC][sS](?![a-zA-Z0-9])")
SC_OPERATORS = re.compile(r"(?<![a-zA-Z0-9])sc[n]?(?![a-zA-Z0-9])")


def pdf_cmyk_ops(path, text):
    """CMYK colour operators in the export: `k`/`K` with four operands, plus
    the `sc`/`scn` paints once the content selects a DeviceCMYK or a
    4-component ICCBased colour space with `cs`/`CS` (the ICC component count
    lives in the uncompressed stream dictionary, hence the raw read)."""
    ops = len(CMYK_OPERANDS.findall(text))
    if CS_OPERATORS.search(text):
        with open(path, "rb") as f:
            raw = f.read().decode("latin-1", "replace")
        if (re.search(r"/DeviceCMYK(?![a-zA-Z0-9])", text)
                or re.search(r"/N\s+4(?![0-9])", raw)):
            ops += len(SC_OPERATORS.findall(text))
    return ops


# -------------------------------------------------------------------- stages --

class Context:
    def __init__(self, binary, testfiles_dir, expect, outdir, scenario):
        self.binary = binary
        self.testfiles_dir = testfiles_dir
        self.expect = expect
        self.outdir = outdir
        self.scenario = scenario
        self.profile = resolve_profile(testfiles_dir, expect.get("profile"))
        self.allow = expect.get("allow_warnings") or []
        self.input = resolve_input(testfiles_dir, expect["input"])
        source = expect["input"]
        if source.startswith("derived:"):
            copies = int(source.split(":", 1)[1].rsplit("-", 1)[-1])
            self.derived = os.path.join(outdir, source.replace(":", "-") + ".cdr")
            self.input = self.derived
            self.derive_copies = copies
        else:
            self.derived = None
            self.derive_copies = 0

    def command(self):
        actions = self.expect.get("actions")
        if actions:
            return [self.input, "--actions=" + placeholder(actions, self.testfiles_dir, self.outdir)]
        return [self.input] + [placeholder(arg, self.testfiles_dir, self.outdir)
                               for arg in self.expect.get("argv_extra") or []]

    def export(self, target, extra):
        """One export run for the svg/png stages; profile seeded like the run."""
        delete_stale(target)
        profile = fresh_profile(self.outdir, "export-" + os.path.basename(target), self.profile)
        run_rosepen(self.binary, [self.input] + extra + [f"--export-filename={target}"],
                    profile, 0, self.allow)
        if not os.path.isfile(target):
            fail(f"{os.path.basename(target)} was not created")


def stage_prepare(ctx):
    if not ctx.derived:
        return "SKIP: no derived input"
    os.makedirs(ctx.outdir, exist_ok=True)
    delete_stale(ctx.derived)
    derive_multipage(os.path.join(fixtures_base(ctx.testfiles_dir),
                                  "librevenge_formats/cdr_draw2.cdr"),
                     ctx.derived, ctx.derive_copies)
    return "OK"


def stage_run(ctx):
    profile = fresh_profile(ctx.outdir, "run", ctx.profile)
    os.makedirs(ctx.outdir, exist_ok=True)
    run_rosepen(ctx.binary, ctx.command(), profile, ctx.expect.get("expect_rc", 0),
                ctx.allow, ctx.expect.get("stderr_contains") or [])
    for name in ctx.expect.get("expect_no_file") or []:
        path = os.path.join(ctx.outdir, name)
        if os.path.lexists(path):
            fail(f"{name} exists although the import failed")
    for name in ctx.expect.get("expect_file_nonblank") or []:
        path = os.path.join(ctx.outdir, name)
        if not os.path.isfile(path):
            fail(f"{name} was not created")
        if nonwhite_percent(path) < 1.0:
            fail(f"{name} is blank")
    return "OK"


def stage_page_list(ctx):
    wanted = ctx.expect.get("page_list")
    if not wanted:
        return "SKIP: no page-list expectation"
    profile = fresh_profile(ctx.outdir, "page-list", ctx.profile)
    stdout, _ = run_rosepen(ctx.binary, [ctx.input, "--actions=page-list"], profile, 0, ctx.allow)
    lines = [line for line in stdout.splitlines() if line.strip()]
    if len(lines) != wanted["lines"]:
        fail(f"page-list printed {len(lines)} lines, expected {wanted['lines']}")
    sizes = []
    for index, line in enumerate(lines, 1):
        fields = line.split("\t")
        if len(fields) != 5:
            fail(f"page-list line {index} has {len(fields)} fields: {line!r}")
        if fields[0] != str(index):
            fail(f"page-list line {index} starts with {fields[0]!r}")
        if not re.fullmatch(r"[0-9.]+x[0-9.]+", fields[3]):
            fail(f"page-list line {index} has no size: {line!r}")
        sizes.append(fields[3])
    if wanted.get("same_size") and len(set(sizes)) != 1:
        fail(f"page sizes differ: {sizes}")
    return "OK"


def stage_svg(ctx):
    sections = ctx.expect.get("svg")
    if not sections:
        return "SKIP: no svg expectation"
    baseline_flat = None
    if (sections.get("plain") or {}).get("baseline_flat_paints"):
        target = os.path.join(ctx.outdir, "baseline.svg")
        delete_stale(target)
        profile = fresh_profile(ctx.outdir, "baseline", None)
        run_rosepen(ctx.binary, [ctx.input, "--export-plain-svg",
                                 f"--export-filename={target}"], profile, 0, ctx.allow)
        if not os.path.isfile(target):
            fail("baseline.svg was not created")
        _, flat, _ = paint_stats(target)
        baseline_flat = len(flat)
    for mode in ("plain", "inkscape"):
        expect = sections.get(mode)
        if not expect:
            continue
        target = os.path.join(ctx.outdir, mode + ".svg")
        if mode == "plain":
            ctx.export(target, ["--export-plain-svg"])
        else:
            ctx.export(target, ["--export-type=svg"])
        check_svg(expect, target, f"svg {mode}", baseline_flat)
    return "OK"


def stage_png(ctx):
    expect = ctx.expect.get("png")
    if not expect:
        return "SKIP: no png expectation"
    pages = expect.get("pages") or [None]
    exported = []
    for page in pages:
        name = "page-%s.png" % (page if page is not None else "all")
        target = os.path.join(ctx.outdir, name)
        extra = ["--export-type=png", "--export-dpi=96"] + EXPORT_BG
        if page is not None:
            extra.append(f"--export-page={page}")
        ctx.export(target, extra)
        exported.append(target)
    if expect.get("min_nonwhite_percent") is not None:
        observed = nonwhite_percent(exported[0])
        if observed < expect["min_nonwhite_percent"]:
            fail(f"png: only {observed:.2f}% of the page is painted, "
                 f"expected at least {expect['min_nonwhite_percent']}%")
    limit = expect.get("pages_max_mean_diff")
    if limit is not None:
        for other in exported[1:]:
            delta = mean_channel_difference(exported[0], other)
            if delta > limit:
                fail(f"png: {os.path.basename(other)} differs from "
                     f"{os.path.basename(exported[0])} by {delta:.3f}, limit {limit}")
    limit = expect.get("baseline_max_mean_diff")
    if limit is not None:
        baseline = os.path.join(ctx.outdir, "baseline.png")
        delete_stale(baseline)
        profile = fresh_profile(ctx.outdir, "png-baseline", None)
        run_rosepen(ctx.binary, [ctx.input, "--export-type=png", "--export-dpi=96"] +
                    EXPORT_BG + [f"--export-filename={baseline}"], profile, 0, ctx.allow)
        delta = mean_channel_difference(baseline, exported[0])
        if delta > limit:
            fail(f"png: mean per-channel difference to the profile-less baseline "
                 f"is {delta:.3f}, limit {limit}")
        return f"OK (mean_cmyk_delta={delta:.3f})"
    return "OK"


def stage_pdf(ctx):
    expect = ctx.expect.get("pdf")
    if not expect or not ctx.pdf:
        return "SKIP: pdf not requested"
    target = os.path.join(ctx.outdir, "out.pdf")
    ctx.export(target, ["--export-type=pdf"])
    text = pdf_inflated_text(target)
    found = len(re.findall(r"/Type\s*/Page\b", text))
    if found != expect["pages"]:
        fail(f"pdf: {found} page objects, expected {expect['pages']}")
    minimum = expect.get("cmyk_ops_min")
    if minimum is not None:
        ops = pdf_cmyk_ops(target, text)
        if ops < minimum:
            fail(f"pdf: {ops} CMYK colour operators in the export, "
                 f"expected at least {minimum}")
        return f"OK (cmyk_ops={ops})"
    return "OK"


STAGES = [("prepare", stage_prepare), ("run", stage_run),
          ("page_list", stage_page_list), ("svg", stage_svg),
          ("png", stage_png), ("pdf", stage_pdf)]


def main(argv):
    pdf = "--pdf" in argv[1:]
    rest = [arg for arg in argv[1:] if arg != "--pdf"]
    if len(rest) != 5:
        print(f"usage: {os.path.basename(argv[0])} <rosepen> <testfiles_dir> "
              f"<expect.json> <outdir> <scenario> [--pdf]", file=sys.stderr)
        return 2
    binary, testfiles_dir, expect_path, outdir, scenario = rest

    with open(expect_path, encoding="utf-8") as f:
        document = json.load(f)
    expect = (document.get("scenarios") or {}).get(scenario)
    if expect is None:
        print(f"STAGE setup FAIL: scenario '{scenario}' not in {expect_path}")
        return 1
    if "input" not in expect:
        print(f"STAGE setup FAIL: scenario '{scenario}' has no input")
        return 1

    os.makedirs(outdir, exist_ok=True)
    ctx = Context(binary, testfiles_dir, expect, outdir, scenario)
    ctx.pdf = pdf

    for name, stage in STAGES:
        try:
            outcome = stage(ctx)
        except StageFail as err:
            print(f"STAGE {name} FAIL: {err}")
            return 1
        except (OSError, ValueError, KeyError, zlib.error) as err:
            print(f"STAGE {name} FAIL: {err}")
            return 1
        print(f"STAGE {name} {outcome}")
    print(f"CDR_IMPORT_OK scenario={scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
