#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless PowerClip lock (05-01 task 3).

Runs one scenario from powerclip.expect.json against powerclip.svg and verifies
the PowerClip model and its rendering through real file I/O:

  model  run `rosepen <fixture> --actions="<scenario actions>;object-powerclip-list;
         export-filename:<outdir>/<scenario>.svg;export-do"`; a non-zero exit,
         CRITICAL/g_error or an unlisted WARNING on stderr is a FAIL, and a
         "stderr_contains" text (the refusal messages) must be present.  The
         `powerclip ...` stdout lines are compared with the expectation.
         Frames are matched by container id; "*" matches one generated id
         (duplicated frames get unpredictable ids).  Each parsed frame's clip
         <use> must name its own container unless the scenario sets
         "shared_clip_ok" (a duplicate shares the clipPath until normalize
         forks it), in which case the use may name any container id.
  svg    parse the exported SVG and check the structural invariants of every
         frame: a role-frame <g> whose first item child is the role-container,
         a role-contents <g> with clip-path=url(#K) and NO transform attribute,
         K existing with exactly one <use> child whose href names the frame's
         own container (or, with "shared_clip_ok", a container with identical
         geometry), the expected number of frames with the expected container
         ids, and for "extract"/"refuse-single" the absence of role attributes
         and (extract) of clipPaths.
  png    for every expected page, export that page of the exported SVG
         (--export-page=N --export-dpi=96 on an opaque white background) and
         sample page-relative user-unit points; each sample must match its
         expected flat sRGB colour within +-2 per channel.

Usage:
    check_powerclip.py <rosepen> <fixture.svg> <expect.json> <outdir> <scenario>
    check_powerclip.py --self-test <rosepen> <fixture.svg> <expect.json>

Prints one "STAGE <name> OK|FAIL: <reason>" line per stage and exits non-zero
on the first FAIL.  With --self-test the checker proves its own teeth: flipping
one expected colour must give "STAGE png FAIL" and doubling the expected frame
count must give "STAGE model FAIL" (temp copies in a /tmp directory, removed
afterwards).  python3 stdlib only, `python3 -I` safe.

The PNG decoding helpers are copied from check_master_layer.py (03-02);
`python3 -I` puts no script dir on sys.path so checkers cannot import each
other.
"""

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from xml.etree import ElementTree as ET

TIMEOUT_S = 120
COLOR_TOL = 2

SVG_NS = "http://www.w3.org/2000/svg"
XLINK_NS = "http://www.w3.org/1999/xlink"
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
TAG_GROUP = f"{{{SVG_NS}}}g"
TAG_CLIP_PATH = f"{{{SVG_NS}}}clipPath"
ATTR_HREF = f"{{{XLINK_NS}}}href"
ATTR_POWERCLIP = f"{{{INKSCAPE_NS}}}powerclip"
ROLE_FRAME = "frame"
ROLE_CONTAINER = "container"
ROLE_CONTENTS = "contents"

EXPORT_BACKGROUND = ["--export-background=#ffffff", "--export-background-opacity=1"]

# The list output and messages are only matched positionally; force the C locale.
ROSEPEN_ENV = dict(os.environ, LC_ALL="C", LANG="C", LANGUAGE="")


class StageFail(Exception):
    """Raised when a stage invariant is violated; message is the reason."""


def fail(reason):
    raise StageFail(reason)


# ---------------------------------------------------------------- running ---

def run_rosepen(binary, args):
    """Run rosepen and return (rc, stdout, stderr) without judging them."""
    try:
        proc = subprocess.run([binary] + args, capture_output=True, text=True,
                              timeout=TIMEOUT_S, env=ROSEPEN_ENV)
    except subprocess.TimeoutExpired:
        fail(f"rosepen timed out after {TIMEOUT_S}s: {' '.join(args)}")
    except OSError as err:
        fail(f"cannot run rosepen: {err}")
    return proc.returncode, proc.stdout or "", proc.stderr or ""


def judge_stderr(stderr, allow_warnings, stderr_contains):
    """FAIL on a fatal error or an unlisted WARNING; require the listed texts
    (the headless refusal messages, which rosepen writes to cerr)."""
    fatal = [ln for ln in stderr.splitlines()
             if "CRITICAL" in ln or re.search(r"ERROR \*\*", ln)]
    if fatal:
        fail(f"rosepen reported a fatal error: {' | '.join(fatal)[:400]}")
    for ln in stderr.splitlines():
        if "WARNING" in ln and not any(allowed in ln for allowed in allow_warnings):
            fail(f"unexpected WARNING on stderr: {ln.strip()[:400]}")
    for text in stderr_contains or []:
        if text not in stderr:
            fail(f"stderr does not contain '{text}'")


def delete_stale(path):
    """The outdir is reused between runs: remove a stale target first so a
    run that exits 0 without writing can't pass on the previous output."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.lexists(path):
        os.remove(path)


# ------------------------------------------------------------ model stage ---

def parse_model(stdout):
    """The `powerclip <frame> container=<id> contents=<ids> clip=<id> use=<id>`
    lines plus the final count."""
    frames = []
    count = None
    for ln in stdout.splitlines():
        m = re.match(r"powerclip (\S+) container=(\S+) contents=(\S*) clip=(\S+) use=(\S+)$", ln)
        if m:
            frames.append({
                "frame": m.group(1), "container": m.group(2),
                "contents": [c for c in m.group(3).split(",") if c and c != "-"],
                "clip": m.group(4), "use": m.group(5),
            })
            continue
        m = re.match(r"powerclip-count (\d+)$", ln)
        if m:
            count = int(m.group(1))
    if count is None:
        fail("no 'powerclip-count' line in the list output")
    if count != len(frames):
        fail(f"powerclip-count {count} but {len(frames)} powerclip lines")
    return frames


def id_matches(expected, actual):
    """Exact id, or '*' for any generated id."""
    return expected == "*" or expected == actual


def check_model_stage(expect, stdout):
    parsed = parse_model(stdout)
    list_expect = expect.get("list") or {}
    wanted = list_expect.get("frames") or []
    if "count" in list_expect and list_expect["count"] != len(parsed):
        fail(f"{len(parsed)} frames listed, expected {list_expect['count']}")
    if parsed and len(parsed) != len(wanted):
        fail(f"{len(parsed)} frames listed, expected {len(wanted)}")

    containers = [p["container"] for p in parsed]
    if len(set(containers)) != len(containers):
        fail(f"duplicate container ids in the list output: {containers}")

    remaining = list(wanted)
    for got in parsed:
        match = None
        for cand in remaining:
            if id_matches(cand.get("container", "*"), got["container"]):
                match = cand
                remaining.remove(cand)
                break
        if match is None:
            fail(f"unexpected frame with container '{got['container']}'")

        wcontents = match.get("contents")
        if wcontents is not None:
            if len(got["contents"]) != len(wcontents):
                fail(f"frame '{got['container']}': {len(got['contents'])} contents "
                     f"({','.join(got['contents'])}), expected {len(wcontents)}")
            for wcid, gcid in zip(wcontents, got["contents"]):
                if not id_matches(wcid, gcid):
                    fail(f"frame '{got['container']}': content '{gcid}', expected '{wcid}'")

        # The clip must be a live use of this frame's own container; a
        # duplicated frame shares the clip with the original until a
        # normalize forks it, so there the use may name any container.
        use = got["use"]
        if use == "-":
            fail(f"frame '{got['container']}': clip has no <use>")
        if use != got["container"] and not (expect.get("shared_clip_ok") and use in containers):
            fail(f"frame '{got['container']}': clip use '{use}' is not its container "
                 f"'{got['container']}'")
    if remaining:
        fail(f"frames not found in the list output: "
             f"{[c.get('container') for c in remaining]}")


# -------------------------------------------------------------- svg stage ---

ITEM_LOCALS = {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
               "text", "flowRoot", "g", "use", "image", "switch", "foreignObject"}
GEOMETRY_ATTRS = ("d", "x", "y", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry",
                  "width", "height", "points", "transform")


def local_name(element):
    return element.tag.rsplit("}", 1)[-1]


def is_item(element):
    return (element.tag.startswith(f"{{{SVG_NS}}}")
            and local_name(element) in ITEM_LOCALS
            and element.get(ATTR_POWERCLIP) != ROLE_CONTENTS)


def parse_viewbox(text):
    parts = (text or "").replace(",", " ").split()
    if len(parts) != 4:
        fail(f"malformed viewBox '{text}'")
    return [float(v) for v in parts]


def same_geometry(a, b):
    """True when two elements carry the same shape: same tag and identical
    geometry/transform attributes (a duplicate copies the shape verbatim; the
    moved frame carries the transform instead)."""
    if local_name(a) != local_name(b):
        return False
    for attr in GEOMETRY_ATTRS:
        if a.get(attr) != b.get(attr):
            return False
    return True


def check_svg_stage(expect, path):
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as err:
        fail(f"cannot parse exported SVG {path}: {err}")

    svg_expect = expect.get("svg") or {}

    if svg_expect.get("no_roles"):
        for el in root.iter():
            if isinstance(el.tag, str) and el.get(ATTR_POWERCLIP) is not None:
                fail(f"role attribute found on <{local_name(el)} id={el.get('id')}> "
                     "but the document must be PowerClip-free")

    clip_paths = {el.get("id"): el for el in root.iter(TAG_CLIP_PATH)}
    if svg_expect.get("no_clip_paths") and clip_paths:
        fail(f"{len(clip_paths)} clipPath(s) left in defs, expected none")

    frames = [el for el in root.iter(TAG_GROUP) if el.get(ATTR_POWERCLIP) == ROLE_FRAME]
    wanted = svg_expect.get("frames") or []
    if len(frames) != len(wanted):
        fail(f"{len(frames)} role-frame groups in the SVG, expected {len(wanted)}")

    by_id = {el.get("id"): el for el in root.iter() if isinstance(el.tag, str) and el.get("id")}
    remaining = list(wanted)
    for frame in frames:
        items = [child for child in frame if isinstance(child.tag, str) and is_item(child)]
        if not items:
            fail(f"frame '{frame.get('id')}' has no item children")
        container = items[0]
        if container.get(ATTR_POWERCLIP) != ROLE_CONTAINER:
            fail(f"frame '{frame.get('id')}': first item child is not the container")
        if container.get("id") is None:
            fail("container has no id")
        cid = container.get("id")

        match = None
        for cand in remaining:
            if id_matches(cand.get("container", "*"), cid):
                match = cand
                remaining.remove(cand)
                break
        if match is None:
            fail(f"unexpected frame with container '{cid}'")

        contents = None
        for child in frame:
            if isinstance(child.tag, str) and child.get(ATTR_POWERCLIP) == ROLE_CONTENTS:
                contents = child
                break
        if contents is None:
            fail(f"frame '{frame.get('id')}': no contents group")
        if contents.get("transform") is not None:
            fail(f"frame '{frame.get('id')}': contents group carries a transform")

        clip_ref = contents.get("clip-path") or ""
        m = re.match(r"url\(#([^)]+)\)$", clip_ref)
        if not m:
            fail(f"frame '{frame.get('id')}': contents clip-path '{clip_ref}' is not url(#id)")
        clip = clip_paths.get(m.group(1))
        if clip is None:
            fail(f"frame '{frame.get('id')}': clipPath '{m.group(1)}' does not exist")
        children = [child for child in clip if isinstance(child.tag, str)]
        if len(children) != 1 or local_name(children[0]) != "use":
            fail(f"clipPath '{m.group(1)}' must hold exactly one <use>")
        href = children[0].get(ATTR_HREF) or children[0].get("href") or ""
        if not href.startswith("#"):
            fail(f"clipPath '{m.group(1)}': use href '{href}' is not a fragment")
        target = by_id.get(href[1:])
        if target is None:
            fail(f"clipPath '{m.group(1)}': use target '{href[1:]}' does not exist")
        if target.get("id") != cid and not (
                svg_expect.get("shared_clip_ok") and same_geometry(target, container)):
            fail(f"frame '{frame.get('id')}': clip use targets '{target.get('id')}' "
                 f"which is not the container '{cid}' (nor identical geometry)")

    if remaining:
        fail(f"frames not found in the SVG: {[c.get('container') for c in remaining]}")

    if svg_expect.get("distinct_containers"):
        ids = [el.get("id") for f in frames for el in f
               if isinstance(el.tag, str) and el.get(ATTR_POWERCLIP) == ROLE_CONTAINER]
        if len(set(ids)) != len(ids):
            fail(f"duplicated frames must have distinct containers, got {ids}")


# -------------------------------------------------------------- png stage ---

# --- PNG helpers copied from check_master_layer.py (03-02) ---

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

# --- end PNG helpers copied from check_master_layer.py ---


def pages_of(svg_root):
    """Page id -> (1-based export index, viewBox rect) in document order."""
    pages = {}
    index = 0
    for el in svg_root.iter():
        if isinstance(el.tag, str) and local_name(el) == "view":
            index += 1
            pages[el.get("id")] = (index, parse_viewbox(el.get("viewBox")))
    if not pages:
        fail("exported SVG has no <view> pages")
    return pages


def check_png_stage(binary, expect, target, outdir, tag):
    try:
        svg_root = ET.parse(target).getroot()
    except (OSError, ET.ParseError) as err:
        fail(f"cannot parse exported SVG {target}: {err}")
    pages = pages_of(svg_root)
    for page_name, samples in (expect.get("png") or {}).items():
        if page_name not in pages:
            fail(f"page '{page_name}' does not exist in the exported SVG")
        page_num, (_px, _py, pw, ph) = pages[page_name]
        path = os.path.join(outdir, f"{tag}-{page_name}.png")
        delete_stale(path)
        run_rosepen(binary, [target, f"--export-page={page_num}", "--export-type=png",
                             "--export-dpi=96"] + EXPORT_BACKGROUND +
                    [f"--export-filename={path}"])
        img = png_decode(path)
        width, height, _bpp, _raw = img
        for name, sample in samples.items():
            # Samples are page-relative user units; the page's own origin is
            # already gone from the exported PNG.
            sx = int(sample[0] / pw * width)
            sy = int(sample[1] / ph * height)
            actual = png_pixel(img, sx, sy)
            exp = bytes.fromhex(sample[2].lstrip("#"))
            if any(abs(g - w) > COLOR_TOL for g, w in zip(actual, exp)):
                fail(f"page {page_name} sample '{name}' at ({sample[0]},{sample[1]}) is "
                     f"#{actual.hex()} expected {sample[2]}")


# ------------------------------------------------------------------ stages ---

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


def check_model(run_rosepen_fail_fn, judge_fn, model_fn):
    """The model stage folds the run judgement (rc, stderr) and the list
    comparison into one stage, per the plan's 'each printing STAGE ... line'."""
    try:
        run_rosepen_fail_fn()
        judge_fn()
        model_fn()
    except StageFail as err:
        print(f"STAGE model FAIL: {err}")
        return False
    print("STAGE model OK")
    return True


# --------------------------------------------------------------- self-test ---

def self_test(binary, fixture, expect_path):
    """Prove the checker fails when an expectation is wrong: one flipped colour
    must trip the png stage, one doubled frame count must trip the model stage.
    Temp copies live in a /tmp directory, removed afterwards."""
    with open(expect_path, encoding="utf-8") as f:
        document_expect = json.load(f)

    me = os.path.abspath(__file__)
    tmp = tempfile.mkdtemp(prefix="powerclip-selftest-")
    try:
        flipped = json.loads(json.dumps(document_expect))
        flipped["scenarios"]["place"]["png"]["p1"]["red_inside"] = [50, 50, "#00ff00"]
        flip_path = os.path.join(tmp, "flip.json")
        with open(flip_path, "w", encoding="utf-8") as f:
            json.dump(flipped, f)
        proc = subprocess.run(
            [sys.executable, "-I", me, binary, fixture, flip_path,
             os.path.join(tmp, "out-flip"), "place"],
            capture_output=True, text=True, timeout=TIMEOUT_S)
        if proc.returncode != 1 or "STAGE png FAIL" not in proc.stdout:
            print(f"SELFTEST FAIL: flipped colour gave rc {proc.returncode}:\n{proc.stdout}")
            return 1

        doubled = json.loads(json.dumps(document_expect))
        doubled["scenarios"]["place"]["list"]["count"] = 2
        double_path = os.path.join(tmp, "double.json")
        with open(double_path, "w", encoding="utf-8") as f:
            json.dump(doubled, f)
        proc = subprocess.run(
            [sys.executable, "-I", me, binary, fixture, double_path,
             os.path.join(tmp, "out-double"), "place"],
            capture_output=True, text=True, timeout=TIMEOUT_S)
        if proc.returncode != 1 or "STAGE model FAIL" not in proc.stdout:
            print(f"SELFTEST FAIL: doubled count gave rc {proc.returncode}:\n{proc.stdout}")
            return 1

        print("POWERCLIP_SELFTEST_OK")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# -------------------------------------------------------------------- main ---

def main(argv):
    selftest = "--self-test" in argv[1:]
    rest = [a for a in argv[1:] if a != "--self-test"]
    if selftest:
        if len(rest) != 3:
            print(f"usage: {os.path.basename(argv[0])} --self-test "
                  "<rosepen> <fixture.svg> <expect.json>", file=sys.stderr)
            return 2
        return self_test(rest[0], rest[1], rest[2])

    if len(rest) != 5:
        print(f"usage: {os.path.basename(argv[0])} <rosepen> <fixture.svg> "
              f"<expect.json> <outdir> <scenario> [--self-test]", file=sys.stderr)
        return 2
    binary, fixture, expect_path, outdir, scenario = rest

    with open(expect_path, encoding="utf-8") as f:
        document_expect = json.load(f)
    expect = document_expect.get("scenarios", {}).get(scenario)
    if expect is None:
        print(f"STAGE setup FAIL: scenario '{scenario}' not in {expect_path}")
        return 1

    target = os.path.join(outdir, scenario + ".svg")
    delete_stale(target)

    actions = expect["actions"] + ";object-powerclip-list"
    actions += f";export-filename:{target};export-do"

    # Model stage: run, judge rc/stderr, then compare the list output.
    result = {}

    def do_run():
        result["out"] = run_rosepen(binary, [fixture, f"--actions={actions}"])

    def do_judge():
        rc, stdout, stderr = result["out"]
        if rc != 0:
            tail = " ".join(stderr.split())[-400:]
            fail(f"rosepen exited {rc}: {tail}")
        judge_stderr(stderr, expect.get("allow_warnings") or [],
                     expect.get("stderr_contains"))

    def do_model():
        check_model_stage(expect, result["out"][1])

    if not check_model(do_run, do_judge, do_model):
        return 1
    if not run_stage("svg", lambda: check_svg_stage(expect, target)):
        return 1
    if expect.get("png"):
        if not run_stage("png", lambda: check_png_stage(binary, expect, target, outdir, scenario)):
            return 1
    print(f"POWERCLIP_OK scenario={scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
