#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Headless PDF/X corpus lock (03-03 tasks 1 and 2).

Runs one scenario from pdfx.expect.json: exports a fixture through the capypdf
exporter with a pinned SOURCE_DATE_EPOCH and inspects the raw PDF objects.  X-4
files store their non-stream objects (page dicts, OutputIntent, Info, catalog)
in an object stream, so every stream -- including /ObjStm -- is inflated before
the rule list is asserted.  The checker asserts a named rule list derived from
ISO 15930-4/-7 and the plan; it is not a veraPDF-style validator.

  export     the target exists and starts with %PDF, or for "refused"
             scenarios the target is absent and stderr carries exactly the
             expected "PDF/X preflight: <id>: ..." lines (a scenario may pin
             the refusal exit code with refused_exit_code).
  header     %PDF-1.3 (X-1a) / 1.6 (X-4) / 2.0 (default); X-1a has a classic
             xref table and no object streams.
  intent     one OutputIntent, /S /GTS_PDFX, the UTF-16BE profile description
             as OutputConditionIdentifier, and a /N 4 DestOutputProfile whose
             inflated bytes are sha256-equal to testfiles/data/colors/
             default_cmyk.icc.
  info       /GTS_PDFXVersion, /Trapped /False, a non-empty /Title, /ID in the
             trailer, both dates pinned to (D:20231114221320Z); X-4 adds the
             XMP Metadata stream keys (version, dates, document id, rendition).
  boxes      per page (±0.01 pt): Trim page size and offset, Bleed = Trim
             grown by 3 mm, Media = Bleed or the mark extent, no ArtBox,
             BleedBox ⊆ MediaBox and TrimBox ⊆ BleedBox.
  marks      the first content operator is a "re W n" clip equal to the
             BleedBox; the last separation stroke colour is /Separation /All
             (DeviceCMYK alternate) at tint 1; the tail carries the eight
             5 mm segments of the gtest-locked geometry.  Without marks no
             /All separation exists.
  structure  no /OCProperties and no /Annots; X-1a has no /S /Transparency.
             The default scenario instead asserts no OutputIntents, no
             /GTS_PDFXVersion, no /Metadata and an ArtBox on every page.
  warnings   stderr carries (or does not carry) the listed substrings: the
             advisory findings that never refuse an export, e.g. a filter
             dropped because rasterization is off.
  color      only DeviceCMYK/DeviceGray/Separation reaches the file: no "rg"/
             "RG" operator, no /DeviceRGB and no ICCBased colour space that is
             reachable from content or resources.  capypdf always leaves one
             unreferenced "[ /ICCBased N 0 R ]" bookkeeping object beside the
             OutputIntent's profile stream; only that dangling object is
             tolerated (recorded as a capypdf finding).  Optional per scenario:
             exact device/output-profile CMYK passthrough, a /Separation with a
             /DeviceCMYK alternate, DeviceCMYK shadings and DeviceCMYK 8-bit
             images.  X-1a additionally has no /SMask, no /CA or /ca below 1
             and no /BM other than Normal.
  masters    every page of the master fixture paints the same XObject
             references (the 03-02 cached master/content forms).
  pixels     re-import the exported PDF through Rosepen's own importer
             (--pages=N, 96 dpi) and sample page-relative millimetres inside
             the trim.  The importer crops to the TrimBox, so the bleed zone is
             not in the raster.  Per-sample tolerances are scenario data.

Usage:
    check_pdfx.py <rosepen> <fixtures-dir> <expect.json> <outdir> <scenario>

Prints "STAGE <name> OK|FAIL: <reason>" and exits non-zero on the first FAIL.
python3 stdlib only, `python3 -I` safe.
"""

import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import zlib

TIMEOUT_S = 120
EPS = 0.01
SOURCE_DATE_EPOCH = "1700000000"
PINNED_DATE = "(D:20231114221320Z)"
PINNED_ISO = "2023-11-14T22:13:20Z"

ROSEPEN_ENV = dict(os.environ, LC_ALL="C", LANG="C", LANGUAGE="",
                   SOURCE_DATE_EPOCH=SOURCE_DATE_EPOCH)

PREFERENCES_XML = (
    '<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
    '<inkscape version="1">\n'
    '  <group id="extensions" org.inkscape.output.pdf.capypdf.cropMarks="%s" />\n'
    '</inkscape>\n'
)


class StageFail(Exception):
    """Raised when a stage invariant is violated; message is the reason."""


def fail(reason):
    raise StageFail(reason)


def near(a, b, eps=EPS):
    return abs(a - b) <= eps


# ---------------------------------------------------------------- running ---

def run_rosepen(binary, args):
    try:
        return subprocess.run([binary] + args, capture_output=True, text=True,
                              timeout=TIMEOUT_S, env=ROSEPEN_ENV)
    except subprocess.TimeoutExpired:
        fail(f"rosepen timed out after {TIMEOUT_S}s: {' '.join(args)}")
    except OSError as err:
        fail(f"cannot run rosepen: {err}")


def delete_stale(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.lexists(path):
        os.remove(path)


def seed_prefs(crop_marks):
    """Extension parameters live as dotted attributes on the extensions group
    (pref_name() = /extensions/<id>.<param>), not in a nested group.

    Filter rasterization is not seeded here: the CLI PDF export overwrites
    blurToBitmap from --export-ignore-filters, so a scenario uses that flag.
    """
    profile = os.environ.get("INKSCAPE_PROFILE_DIR")
    if not profile:
        if crop_marks:
            fail("crop marks need INKSCAPE_PROFILE_DIR to seed the extension pref")
        return
    os.makedirs(profile, exist_ok=True)
    with open(os.path.join(profile, "preferences.xml"), "w", encoding="utf-8") as f:
        f.write(PREFERENCES_XML % ("1" if crop_marks else "0"))


def export(binary, fixture, target, version, extra_args=()):
    args = [fixture, "--export-type=pdf", f"--export-filename={target}"]
    args.extend(extra_args)
    if version:
        args.append(f"--export-pdf-version={version}")
    return run_rosepen(binary, args)


def check_stderr_fatal(stderr):
    for line in stderr.splitlines():
        if "CRITICAL" in line or re.search(r"ERROR \*\*", line):
            fail(f"rosepen reported a fatal error: {line.strip()[:400]}")


def preflight_ids(stderr):
    return [m.group(1).strip() for m in
            re.finditer(r"PDF/X preflight:\s*([^:\n]+):", stderr)]


# ------------------------------------------------------------- pdf reading ---

OBJECT_HEADER = re.compile(rb"(\d+)\s+0\s+obj\s*\n")


def read_objects(data):
    """num -> (dictionary text, stream bytes or None) for top-level objects."""
    objects = {}
    pos = 0
    while True:
        match = OBJECT_HEADER.search(data, pos)
        if not match:
            break
        num = int(match.group(1))
        start = match.end()
        stream_pos = data.find(b"stream", start)
        end_obj = data.find(b"endobj", start)
        if stream_pos != -1 and (end_obj == -1 or stream_pos < end_obj):
            dictionary = data[start:stream_pos].decode("latin-1")
            length = re.search(r"/Length\s+(\d+)", dictionary)
            body_start = stream_pos + len(b"stream")
            if data[body_start:body_start + 2] == b"\r\n":
                body_start += 2
            elif data[body_start:body_start + 1] in (b"\n", b"\r"):
                body_start += 1
            if length:
                size = int(length.group(1))
                body = data[body_start:body_start + size]
                after = body_start + size
            else:
                end = data.find(b"endstream", body_start)
                body = data[body_start:end]
                after = end
            objects[num] = (dictionary, body)
            next_obj = data.find(b"endobj", after)
            pos = next_obj + 6 if next_obj != -1 else after
        else:
            dictionary = (data[start:end_obj].decode("latin-1") if end_obj != -1
                          else data[start:].decode("latin-1"))
            objects[num] = (dictionary, None)
            pos = end_obj + 6 if end_obj != -1 else len(data)
    return objects


def inflate(dictionary, body):
    if body is None:
        return b""
    if "/FlateDecode" in dictionary:
        try:
            return zlib.decompress(body)
        except zlib.error as err:
            fail(f"PDF: a FlateDecode stream failed to inflate: {err}")
    return body


def expand_object_streams(objects):
    """Add the dictionaries stored inside every /ObjStm (X-4) to the map."""
    for num, (dictionary, body) in list(objects.items()):
        if body is None or "/ObjStm" not in dictionary:
            continue
        raw = inflate(dictionary, body)
        first = re.search(r"/First\s+(\d+)", dictionary)
        count = re.search(r"/N\s+(\d+)", dictionary)
        if not first or not count:
            fail(f"PDF: malformed ObjStm object {num}")
        first = int(first.group(1))
        count = int(count.group(1))
        header = raw[:first].split()
        if len(header) < 2 * count:
            fail(f"PDF: ObjStm object {num} has a short header")
        pairs = [(int(header[2 * i]), int(header[2 * i + 1])) for i in range(count)]
        for i, (obj_num, offset) in enumerate(pairs):
            end = pairs[i + 1][1] if i + 1 < count else len(raw) - first
            objects[obj_num] = (raw[first + offset:first + end].decode("latin-1"), None)


def load_pdf(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"%PDF"):
        fail("pdf: output does not start with %PDF")
    objects = read_objects(data)
    expand_object_streams(objects)
    return data, objects


def object_dicts(objects):
    return [dictionary for dictionary, _body in objects.values()]


def page_objects(objects):
    return sorted((num, dictionary) for num, (dictionary, _body) in objects.items()
                  if re.search(r"/Type\s*/Page(?!s)", dictionary))


def parse_box(dictionary, name):
    match = re.search(r"/" + name +
                      r"\s*\[\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s*\]",
                      dictionary)
    return [float(group) for group in match.groups()] if match else None


def pdf_utf16be(hex_string):
    raw = bytes.fromhex(hex_string)
    if raw[:2] == b"\xfe\xff":
        raw = raw[2:]
    return raw.decode("utf-16-be", "replace")


def stream_text(objects, num):
    dictionary, body = objects[num]
    return inflate(dictionary, body).decode("latin-1", "replace")


# ----------------------------------------------------------------- stages ---

def stage_export(binary, scenario, fixture, target, expect):
    """Run the export and return its stderr for later warning assertions."""
    refused = scenario.get("refused", False)
    proc = export(binary, fixture, target, scenario.get("version"),
                  scenario.get("extra_args") or [])
    stderr = proc.stderr or ""
    check_stderr_fatal(stderr)
    if refused:
        if os.path.exists(target):
            fail("a refused PDF/X export wrote a target file")
        expected_rc = scenario.get("refused_exit_code")
        if expected_rc is not None and proc.returncode != expected_rc:
            fail(f"a refused PDF/X export exited {proc.returncode}, expected {expected_rc}")
        got = sorted(preflight_ids(stderr))
        wanted = sorted(scenario.get("refused_ids") or [])
        if got != wanted:
            fail(f"preflight ids are {got}, expected {wanted}")
        return stderr
    if proc.returncode != 0:
        tail = " ".join(stderr.split())[-400:]
        fail(f"rosepen exited {proc.returncode}: {tail}")
    if not os.path.isfile(target):
        fail("pdf: the export did not create the target")
    with open(target, "rb") as f:
        if not f.read(8).startswith(b"%PDF"):
            fail("pdf: the target does not start with %PDF")
    return stderr


def stage_warnings(stderr, scenario):
    """Advisory findings: substrings stderr must (not) carry."""
    for needle in scenario.get("warnings") or []:
        if needle not in stderr:
            tail = " ".join(stderr.split())[-400:]
            fail(f"stderr is missing {needle!r}: {tail}")
    for needle in scenario.get("warnings_absent") or []:
        if needle in stderr:
            fail(f"stderr unexpectedly carries {needle!r}")


def stage_separations(objects, scenario):
    """Exact count of /Separation colorant resources (distinctness lock)."""
    expected = scenario.get("separations")
    if expected is None:
        return
    found = sum(dictionary.count("/Separation") for dictionary in object_dicts(objects))
    if found != expected:
        fail(f"pdf: found {found} /Separation colorants, expected {expected}")


def stage_header(data, scenario):
    header = scenario.get("header")
    if header and not data.startswith(header.encode("latin-1")):
        fail(f"pdf: header is {data[:8]!r}, expected {header!r}")
    if scenario.get("version") == "X-1a":
        if b"/Type /XRef" in data or b"/Type/XRef" in data:
            fail("pdf: PDF/X-1a must not use an xref stream")
        if b"/Type /ObjStm" in data or b"/Type/ObjStm" in data:
            fail("pdf: PDF/X-1a must not use object streams")


def stage_intent(data, objects, scenario, fixtures_dir):
    # capypdf writes the value without a leading slash here: "/Type OutputIntent".
    intents = [dictionary for dictionary in object_dicts(objects)
               if re.search(r"/Type\s*/?OutputIntent\b", dictionary)]
    if len(intents) != 1:
        fail(f"pdf: found {len(intents)} OutputIntents, expected 1")
    dictionary = intents[0]
    if not re.search(r"/S\s*/GTS_PDFX\b", dictionary):
        fail("pdf: OutputIntent /S is not /GTS_PDFX")
    identifier = re.search(r"/OutputConditionIdentifier\s*<([0-9A-Fa-f]+)>", dictionary)
    if not identifier:
        fail("pdf: OutputConditionIdentifier is missing")
    name = pdf_utf16be(identifier.group(1))
    if name != "Artifex CMYK SWOP Profile":
        fail(f"pdf: OutputConditionIdentifier is {name!r}, expected the profile description")
    destination = re.search(r"/DestOutputProfile\s+(\d+)\s+0\s+R", dictionary)
    if not destination:
        fail("pdf: DestOutputProfile is missing")
    num = int(destination.group(1))
    if num not in objects:
        fail(f"pdf: DestOutputProfile object {num} is missing")
    profile_dict, profile_body = objects[num]
    if not re.search(r"/N\s+4\b", profile_dict):
        fail("pdf: DestOutputProfile is not a 4-channel ICC stream")
    embedded = inflate(profile_dict, profile_body)
    # fixtures-dir is testfiles/cli_tests/testcases/pdfx
    path = os.path.normpath(os.path.join(fixtures_dir, "..", "..", "..",
                                         "data", "colors", "default_cmyk.icc"))
    with open(path, "rb") as f:
        wanted = f.read()
    if hashlib.sha256(embedded).digest() != hashlib.sha256(wanted).digest():
        fail(f"pdf: DestOutputProfile ({len(embedded)} bytes) is not the source ICC file "
             f"({len(wanted)} bytes)")


def info_dictionary(objects):
    matches = [dictionary for dictionary in object_dicts(objects)
               if "/Producer" in dictionary and "/CreationDate" in dictionary]
    if len(matches) != 1:
        fail(f"pdf: found {len(matches)} Info dictionaries, expected 1")
    return matches[0]


def stage_info(data, objects, scenario):
    info = info_dictionary(objects)
    version = scenario.get("pdfx_version")
    if version:
        if f"/GTS_PDFXVersion ({version})" not in info:
            fail(f"pdf: Info /GTS_PDFXVersion is not ({version})")
        if "/Trapped /False" not in info:
            fail("pdf: Info /Trapped is not /False")
        if f"/CreationDate {PINNED_DATE}" not in info or f"/ModDate {PINNED_DATE}" not in info:
            fail("pdf: Info dates are not the pinned SOURCE_DATE_EPOCH")
    title = re.search(r"/Title\s+<([0-9A-Fa-f]+)>", info)
    if title:
        if not pdf_utf16be(title.group(1)).strip():
            fail("pdf: Info /Title is empty")
    elif re.search(r"/Title\s*\((.*?)\)", info):
        if not re.search(r"/Title\s*\((.+?)\)", info).group(1).strip():
            fail("pdf: Info /Title is empty")
    elif version:
        fail("pdf: Info /Title is missing")
    if not re.search(rb"/ID\s*\[", data):
        fail("pdf: the trailer has no /ID")
    if version is None:
        return
    if scenario.get("version") != "X-4":
        return
    catalog = [dictionary for dictionary in object_dicts(objects)
               if re.search(r"/Type\s*/Catalog\b", dictionary)]
    if len(catalog) != 1:
        fail("pdf: cannot find the document catalog")
    metadata = re.search(r"/Metadata\s+(\d+)\s+0\s+R", catalog[0])
    if not metadata:
        fail("pdf: PDF/X-4 has no /Metadata stream")
    xmp = stream_text(objects, int(metadata.group(1)))
    for pattern in ("GTS_PDFXVersion>PDF/X-4<", "xmpMM:DocumentID>uuid:",
                    "xmpMM:VersionID>", "xmpMM:RenditionClass>default<",
                    "pdf:Trapped>False<", f"xmp:CreateDate>{PINNED_ISO}<",
                    f"xmp:ModifyDate>{PINNED_ISO}<"):
        if pattern not in xmp:
            fail(f"pdf: XMP metadata is missing {pattern!r}")
    # Info and XMP must not disagree on a key both carry.
    producer = re.search(r"/Producer\s*\((.*?)\)", info)
    xmp_producer = re.search(r"<pdf:Producer>(.*?)</pdf:Producer>", xmp)
    if producer and xmp_producer and producer.group(1) != xmp_producer.group(1):
        fail(f"pdf: XMP pdf:Producer {xmp_producer.group(1)!r} differs from Info "
             f"/Producer {producer.group(1)!r}")


def stage_boxes(data, objects, scenario):
    pages = page_objects(objects)
    wanted = scenario.get("pages") or []
    if len(pages) != len(wanted):
        fail(f"pdf: found {len(pages)} pages, expected {len(wanted)}")
    for index, ((num, dictionary), expected) in enumerate(zip(pages, wanted), start=1):
        media = parse_box(dictionary, "MediaBox")
        bleed = parse_box(dictionary, "BleedBox")
        trim = parse_box(dictionary, "TrimBox")
        art = parse_box(dictionary, "ArtBox")
        if media is None or bleed is None or trim is None:
            fail(f"pdf: page {index} is missing MediaBox/BleedBox/TrimBox")
        if art is not None:
            fail(f"pdf: page {index} has an ArtBox; PDF/X needs Trim xor Art")
        for name, actual in (("media", media), ("bleed", bleed), ("trim", trim)):
            for got, want in zip(actual, expected[name]):
                if not near(got, want):
                    fail(f"pdf: page {index} {name} is {actual}, expected {expected[name]} (±{EPS})")
        if not (bleed[0] >= media[0] - EPS and bleed[1] >= media[1] - EPS and
                bleed[2] <= media[2] + EPS and bleed[3] <= media[3] + EPS):
            fail(f"pdf: page {index} BleedBox is not inside MediaBox")
        if not (trim[0] >= bleed[0] - EPS and trim[1] >= bleed[1] - EPS and
                trim[2] <= bleed[2] + EPS and trim[3] <= bleed[3] + EPS):
            fail(f"pdf: page {index} TrimBox is not inside BleedBox")


def page_content(objects, dictionary):
    match = re.search(r"/Contents\s+(\d+)\s+0\s+R", dictionary)
    if not match:
        fail("pdf: a page has no /Contents stream")
    return stream_text(objects, int(match.group(1)))


def resource_colorspaces(objects, dictionary):
    match = re.search(r"/Resources\s+(\d+)\s+0\s+R", dictionary)
    if not match:
        fail("pdf: a page has no /Resources object")
    resources = objects[int(match.group(1))][0]
    return {int(name): int(obj) for name, obj in
            re.findall(r"/CSpace(\d+)\s+(\d+)\s+0\s+R", resources)}


def expected_mark_segments(trim, offset, length):
    x1, y1, x2, y2 = trim
    return [
        ((x1 - offset, y2), (x1 - offset - length, y2)),
        ((x1, y2 + offset), (x1, y2 + offset + length)),
        ((x2 + offset, y2), (x2 + offset + length, y2)),
        ((x2, y2 + offset), (x2, y2 + offset + length)),
        ((x1 - offset, y1), (x1 - offset - length, y1)),
        ((x1, y1 - offset), (x1, y1 - offset - length)),
        ((x2 + offset, y1), (x2 + offset + length, y1)),
        ((x2, y1 - offset), (x2, y1 - offset - length)),
    ]


def check_marks_page(index, dictionary, objects, expected, scenario):
    content = page_content(objects, dictionary)
    clip = re.search(r"([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+re\s+W\s+n", content)
    if not clip:
        fail(f"pdf: page {index} has no 're W n' content clip")
    got_clip = [float(value) for value in clip.groups()]
    bleed = expected["bleed"]
    wanted_clip = [bleed[0], bleed[1], bleed[2] - bleed[0], bleed[3] - bleed[1]]
    for got, want in zip(got_clip, wanted_clip):
        if not near(got, want):
            fail(f"pdf: page {index} content clip is {got_clip}, expected the BleedBox "
                 f"{wanted_clip}")

    spaces = list(re.finditer(r"/CSpace(\d+)\s+CS", content))
    if not spaces:
        fail(f"pdf: page {index} has no separation stroke colour")
    tail = content[spaces[-1].start():]
    cs_id = int(spaces[-1].group(1))
    mapping = resource_colorspaces(objects, dictionary)
    if cs_id not in mapping:
        fail(f"pdf: page {index} /CSpace{cs_id} is not in the resource dictionary")
    separation = objects[mapping[cs_id]][0]
    if not re.search(r"/Separation\s+/All\b", separation):
        fail(f"pdf: page {index} marks are not drawn in /Separation /All")
    if "/DeviceCMYK" not in separation:
        fail(f"pdf: page {index} registration separation has no DeviceCMYK alternate")
    if not re.search(r"\b1(?:\.0+)?\s+SCN\b", tail):
        fail(f"pdf: page {index} registration tint is not 1")

    parsed = [tuple(float(value) for value in match.groups())
              for match in re.finditer(
                  r"([-\d.]+)\s+([-\d.]+)\s+m\s+([-\d.]+)\s+([-\d.]+)\s+l\s+S", tail)]
    wanted = expected_mark_segments(expected["trim"], scenario["mark_offset_pt"],
                                    scenario["mark_length_pt"])
    if len(parsed) != 8:
        fail(f"pdf: page {index} has {len(parsed)} mark segments, expected 8")
    used = [False] * len(parsed)
    for want in wanted:
        found = False
        for i, got in enumerate(parsed):
            if used[i]:
                continue
            direct = all(near(g, w) for g, w in zip(got, want[0] + want[1]))
            reversed_ = all(near(g, w) for g, w in zip(got, want[1] + want[0]))
            if direct or reversed_:
                used[i] = True
                found = True
                break
        if not found:
            fail(f"pdf: page {index} is missing mark segment {want}")
    if not all(used):
        fail(f"pdf: page {index} has extra mark segments")


def stage_marks(objects, scenario):
    pages = page_objects(objects)
    if scenario.get("crop_marks"):
        for index, (num, dictionary) in enumerate(pages, start=1):
            check_marks_page(index, dictionary, objects, scenario["pages"][index - 1],
                             scenario)
        return
    for dictionary in object_dicts(objects):
        if "/Separation" in dictionary and re.search(r"/Separation\s+/All\b", dictionary):
            fail("pdf: an /All registration separation exists without crop marks")
    for _num, dictionary in pages:
        if re.search(r"/CSpace\d+\s+CS", page_content(objects, dictionary)):
            fail("pdf: a page strokes with a separation colour without crop marks")


def stage_structure(data, objects, scenario):
    dictionaries = object_dicts(objects)
    if scenario.get("default_unchanged"):
        if any("/OutputIntents" in dictionary for dictionary in dictionaries):
            fail("pdf: the default path must not write /OutputIntents")
        if any("/GTS_PDFXVersion" in dictionary for dictionary in dictionaries):
            fail("pdf: the default path must not write /GTS_PDFXVersion")
        if any("/Metadata" in dictionary for dictionary in dictionaries):
            fail("pdf: the default path must not write XMP metadata")
        pages = page_objects(objects)
        if not pages:
            fail("pdf: no pages")
        for index, (_num, dictionary) in enumerate(pages, start=1):
            if parse_box(dictionary, "ArtBox") is None:
                fail(f"pdf: default page {index} has no ArtBox")
        return
    if any("/OCProperties" in dictionary for dictionary in dictionaries):
        fail("pdf: PDF/X must not write optional content")
    for dictionary in dictionaries:
        if re.search(r"/Annots\s*\[", dictionary):
            fail("pdf: PDF/X must not write annotations")
    if scenario.get("version") == "X-1a":
        if any(re.search(r"/S\s*/Transparency\b", dictionary) for dictionary in dictionaries):
            fail("pdf: PDF/X-1a must not write transparency groups")


def page_xobjects(objects, dictionary):
    """Map resource name -> object number for the XObjects a page paints."""
    match = re.search(r"/Resources\s+(\d+)\s+0\s+R", dictionary)
    if not match:
        fail("pdf: a page has no /Resources object")
    resources = objects[int(match.group(1))][0]
    xobjects = re.search(r"/XObject\s*<<(.*?)>>", resources, re.S)
    if not xobjects:
        fail("pdf: a page has no /XObject resources")
    names = dict(re.findall(r"/(\w+)\s+(\d+)\s+0\s+R", xobjects.group(1)))
    painted = re.findall(r"/(\w+)\s+Do\b", page_content(objects, dictionary))
    return {name: names.get(name) for name in painted}


def stage_masters(objects, scenario):
    pages = page_objects(objects)
    if len(pages) < 2:
        fail(f"pdf: the master fixture has {len(pages)} pages, expected at least 2")
    first = page_xobjects(objects, pages[0][1])
    if len(first) < 2:
        fail(f"pdf: page 1 paints {len(first)} XObjects, expected a master and a content form")
    if any(obj is None for obj in first.values()):
        fail("pdf: a painted XObject is missing from the page resources")
    for index, (_num, dictionary) in enumerate(pages[1:], start=2):
        current = page_xobjects(objects, dictionary)
        if current != first:
            fail(f"pdf: page {index} paints {current}, expected the shared forms {first}")


# --------------------------------------------------------------- colour -----

def content_streams(objects):
    """Inflated text of every vector content stream (page/form content, shading
    functions, patterns). Images, the embedded ICC profile and XMP are skipped:
    they are not vector colour operators."""
    for _num, (dictionary, body) in objects.items():
        if body is None:
            continue
        if re.search(r"/Subtype\s*/(Image|XML)\b", dictionary):
            continue
        if re.search(r"/N\s+4\b", dictionary):  # DestOutputProfile
            continue
        yield inflate(dictionary, body).decode("latin-1", "replace")


def stage_color(objects, scenario):
    """Only DeviceCMYK/DeviceGray/Separation colour reaches the file.

    There is no DeviceRGB operator, no DeviceRGB colour space, and no ICCBased
    colour space reachable from content or resources (capypdf's one dangling
    output-profile array is tolerated, see below).
    """
    dictionaries = object_dicts(objects)
    text = "\n".join(content_streams(objects))
    everything = "\n".join(dictionaries) + "\n" + text

    if re.search(r"(?<![A-Za-z])rg(?![A-Za-z])", text):
        fail("pdf: a DeviceRGB fill operator (rg) is present")
    if re.search(r"(?<![A-Za-z])RG(?![A-Za-z])", text):
        fail("pdf: a DeviceRGB stroke operator (RG) is present")
    if "/DeviceRGB" in everything:
        fail("pdf: a /DeviceRGB colour space is referenced")

    # capypdf always registers the output profile as a colour-space object
    # ("[ /ICCBased N 0 R ]") even though the OutputIntent references the
    # profile stream directly, so that object is left unreferenced. Only that
    # dangling artifact is allowed: nothing reachable from content or
    # resources may use an ICCBased colour space.
    icc_arrays = {num for num, (dictionary, _body) in objects.items()
                  if re.search(r"\[\s*/ICCBased\s+\d+\s+0\s+R\s*\]", dictionary)}
    for dictionary, body in objects.values():
        scope = dictionary
        if body is not None:
            scope += "\n" + inflate(dictionary, body).decode("latin-1", "replace")
        if re.search(r"/ColorSpace\s*\[\s*/ICCBased", scope):
            fail("pdf: an inline ICCBased colour space is referenced")
    # Any indirect reference to the array object counts, whatever its key:
    # resource dicts use "/ColorSpace << /CS0 N 0 R >>", groups "/CS N 0 R".
    # Content streams cannot reference objects, so dictionaries suffice
    # (ObjStm members are expanded into the map).
    referenced = {int(target) for dictionary in dictionaries
                  for target in re.findall(r"(\d+)\s+0\s+R\b", dictionary)}
    used = icc_arrays & referenced
    if used:
        fail(f"pdf: ICCBased colour space object(s) {sorted(used)} are referenced by content/resources")
    if len(icc_arrays) > 1:
        fail(f"pdf: {len(icc_arrays)} ICCBased colour-space objects; expected only capypdf's "
             f"unreferenced output-profile pair")

    if scenario.get("exact_cmyk"):
        found = len(re.findall(r"0\.100000 0\.200000 0\.300000 0\.400000 k", text))
        if found < 2:
            fail(f"pdf: the exact output-profile CMYK values appear {found}x, expected 2")

    if scenario.get("spot_separation"):
        if not any(re.search(r"/Separation\s+/\S+\s+/DeviceCMYK\b", dictionary)
                   for dictionary in dictionaries):
            fail("pdf: no /Separation with a /DeviceCMYK alternate was written for the spot")

    shadings = [dictionary for dictionary in dictionaries if "/ShadingType" in dictionary]
    if scenario.get("shading_cmyk"):
        if not shadings:
            fail("pdf: the fixture wrote no shading dictionary")
        for dictionary in shadings:
            if not re.search(r"/ColorSpace\s*/DeviceCMYK\b", dictionary):
                fail("pdf: a shading is not DeviceCMYK in the output intent")
    for dictionary in shadings:
        if re.search(r"/ColorSpace\s*/DeviceRGB\b", dictionary):
            fail("pdf: a shading is DeviceRGB")

    smasks = {int(num) for num in re.findall(r"/SMask\s+(\d+)\s+0\s+R", everything)}
    images = [(num, dictionary) for num, (dictionary, _body) in objects.items()
              if re.search(r"/Subtype\s*/Image\b", dictionary)]
    if scenario.get("cmyk_images"):
        if not images:
            fail("pdf: the fixture wrote no image XObject")
        for num, dictionary in images:
            if num in smasks:
                continue  # an SMask's alpha plane is DeviceGray by definition
            if not re.search(r"/ColorSpace\s*/DeviceCMYK\b", dictionary):
                fail(f"pdf: image object {num} is not a DeviceCMYK image")
            if not re.search(r"/BitsPerComponent\s+8\b", dictionary):
                fail(f"pdf: image object {num} does not use 8 bits per component")

    if scenario.get("preserved_transparency"):
        if "/SMask" not in everything and not re.search(r"/ca\s+0\.5", everything):
            fail("pdf: PDF/X-4 lost the document's transparency")
        for dictionary in dictionaries:
            if re.search(r"/S\s*/Transparency\b", dictionary) and \
                    re.search(r"/CS\s*/DeviceRGB\b", dictionary):
                fail("pdf: a transparency group is DeviceRGB")

    if scenario.get("version") == "X-1a":
        if "/SMask" in everything:
            fail("pdf: PDF/X-1a must not use a soft mask")
        for dictionary in dictionaries:
            for key in ("CA", "ca"):
                for value in re.findall(r"/" + key + r"\s+([\d.]+)", dictionary):
                    if float(value) < 1.0:
                        fail(f"pdf: PDF/X-1a sets /{key} {value} (transparency)")
        for mode in re.findall(r"/BM\s*/(\w+)", everything):
            if mode != "Normal":
                fail(f"pdf: PDF/X-1a uses the /{mode} blend mode")


# ---------------------------------------------------------------- pixels ----

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


def stage_pixels(binary, scenario, target, outdir, colors):
    """Re-import the PDF through Rosepen's own importer and sample the trim.

    The importer crops the page to its TrimBox (probe finding: 378x302 px for
    the 100x80 mm fixture at 96 dpi, i.e. no bleed), so samples are page
    relative millimetres inside the trim.
    """
    pixels = scenario.get("pixels") or {}
    tolerance = pixels.get("tolerance") or {}
    trim_w, trim_h = pixels.get("trim_mm") or [100.0, 80.0]
    points = pixels.get("sample_points") or {}
    if not (pixels.get("pages") or []):
        fail("pdf: the pixels stage has no pages")

    for entry in pixels["pages"]:
        page = entry["page"]
        path = os.path.join(outdir, f"pixels-p{page}.png")
        delete_stale(path)
        proc = run_rosepen(binary, [target, f"--pages={page}", "--export-type=png",
                                    "--export-dpi=96", "--export-background=#ffffff",
                                    "--export-background-opacity=1",
                                    f"--export-filename={path}"])
        check_stderr_fatal(proc.stderr or "")
        if proc.returncode != 0:
            tail = " ".join((proc.stderr or "").split())[-400:]
            fail(f"rosepen could not re-import page {page}: {tail}")
        if not os.path.isfile(path):
            fail(f"pdf: re-importing page {page} wrote no PNG")
        img = png_decode(path)
        width, height, _bpp, _raw = img
        for name, color_name in (entry.get("samples") or {}).items():
            if name not in points:
                fail(f"pdf: unknown pixel sample '{name}'")
            x_mm, y_mm = points[name]
            x = int(x_mm / trim_w * width)
            y = int(y_mm / trim_h * height)
            actual = png_pixel(img, x, y)
            wanted = colors.get(color_name)
            if wanted is None:
                fail(f"pdf: pixel sample '{name}' wants unknown colour '{color_name}'")
            eps = tolerance.get(name, tolerance.get("default", 24))
            if any(abs(g - w) > eps for g, w in zip(actual, wanted)):
                fail(f"pdf: page {page} sample {name} is {tuple(actual)}, expected "
                     f"{color_name} {tuple(wanted)} (±{eps})")


def run_stage(name, fn):
    try:
        fn()
    except StageFail as err:
        print(f"STAGE {name} FAIL: {err}")
        return False
    print(f"STAGE {name} OK")
    return True


# -------------------------------------------------------------------- main ---

def main(argv):
    if len(argv) != 6:
        print(f"usage: {os.path.basename(argv[0])} <rosepen> <fixtures-dir> "
              f"<expect.json> <outdir> <scenario>", file=sys.stderr)
        return 2
    binary, fixtures_dir, expect_path, outdir, scenario_name = argv[1:]

    with open(expect_path, encoding="utf-8") as f:
        document = json.load(f)
    scenario = (document.get("scenarios") or {}).get(scenario_name)
    if scenario is None:
        print(f"STAGE setup FAIL: scenario '{scenario_name}' is not in {expect_path}")
        return 1

    fixture = os.path.join(fixtures_dir, scenario["fixture"])
    target = os.path.join(outdir, scenario_name + ".pdf")
    delete_stale(target)
    seed_prefs(scenario.get("crop_marks", False))

    outcome = {}

    def run_export():
        outcome["stderr"] = stage_export(binary, scenario, fixture, target, document)

    if not run_stage("export", run_export):
        return 1
    if scenario.get("refused"):
        return 0

    stderr = outcome.get("stderr") or ""
    data, objects = load_pdf(target)
    colors = document.get("colors") or {}
    stages = scenario.get("stages") or []
    runners = {
        "header": lambda: stage_header(data, scenario),
        "intent": lambda: stage_intent(data, objects, scenario, fixtures_dir),
        "info": lambda: stage_info(data, objects, scenario),
        "boxes": lambda: stage_boxes(data, objects, scenario),
        "marks": lambda: stage_marks(objects, scenario),
        "structure": lambda: stage_structure(data, objects, scenario),
        "warnings": lambda: stage_warnings(stderr, scenario),
        "separations": lambda: stage_separations(objects, scenario),
        "color": lambda: stage_color(objects, scenario),
        "masters": lambda: stage_masters(objects, scenario),
        "pixels": lambda: stage_pixels(binary, scenario, target, outdir, colors),
    }
    for name in stages:
        if name == "export":
            continue
        if name not in runners:
            print(f"STAGE setup FAIL: unknown stage '{name}'")
            return 1
        if not run_stage(name, runners[name]):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
