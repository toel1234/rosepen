#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Rosepen MCP server — lets MCP agents paint in Rosepen documents.

Stdio MCP server (JSON-RPC 2.0, protocol 2024-11-05), Python stdlib only.

Model: the document FILE is the state. Every mutating tool edits the SVG
directly (with an automatic timestamped backup) or runs the Rosepen binary
headlessly. Agents get real rendering feedback via `render` (returns a PNG
image) and the full 1500+ headless action surface via `action`.

Run:  python3 rosepen_mcp.py
Env:  ROSEPEN_BIN   binary to use (default: rosepen on PATH, then build/bin/rosepen)
      ROSEPEN_ROOTS colon-separated allowed roots for document paths
      (default: the server's working directory)
"""
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "rosepen"
SERVER_VERSION = "0.1.0"

SVG_NS = "http://www.w3.org/2000/svg"
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
SODIPODI_NS = "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
XLINK_NS = "http://www.w3.org/1999/xlink"
for prefix, uri in (("", SVG_NS), ("inkscape", INKSCAPE_NS),
                    ("sodipodi", SODIPODI_NS), ("xlink", XLINK_NS)):
    ET.register_namespace(prefix, uri)

NS = {"svg": SVG_NS, "inkscape": INKSCAPE_NS, "sodipodi": SODIPODI_NS, "xlink": XLINK_NS}


def rosepen_bin():
    env = os.environ.get("ROSEPEN_BIN")
    if env:
        return env
    found = shutil.which("rosepen")
    if found:
        return found
    local = Path(__file__).resolve().parents[2] / "build" / "bin" / "rosepen"
    if local.exists():
        return str(local)
    return "rosepen"


def allowed_roots():
    roots = os.environ.get("ROSEPEN_ROOTS")
    if roots:
        return [Path(r).resolve() for r in roots.split(":") if r]
    return [Path.cwd().resolve()]


def check_path(path):
    p = Path(path).expanduser().resolve()
    if not any(p == r or r in p.parents for r in allowed_roots()):
        raise ValueError(f"path {p} is outside the allowed roots {allowed_roots()}")
    return p


def run_rosepen(args, timeout_s=60):
    cmd = [rosepen_bin()] + [str(a) for a in args]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return 124, "", f"rosepen timed out after {timeout_s}s: {' '.join(cmd)}"


def parse_doc(path):
    tree = ET.parse(path)
    return tree, tree.getroot()


def save_doc(tree, path):
    tree.write(path, xml_declaration=True, encoding="utf-8")


def backup(path):
    hist = Path(str(path) + ".mcp-history")
    hist.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S") + f"-{int(time.time() * 1000) % 1000:03d}"
    shutil.copy2(path, hist / f"{stamp}.svg")
    return str(hist / f"{stamp}.svg")


def q(tag):
    return f"{{{SVG_NS}}}{tag}"


def top_layer(root, create=True):
    """Last (topmost) layer group among root children, else create one."""
    layers = [c for c in root if c.tag == q("g") and c.get(f"{{{INKSCAPE_NS}}}groupmode") == "layer"]
    if layers:
        return layers[-1]
    if not create:
        return None
    layer = ET.SubElement(root, q("g"))
    layer.set("id", unique_id(root, "layer"))
    layer.set(f"{{{INKSCAPE_NS}}}groupmode", "layer")
    layer.set(f"{{{INKSCAPE_NS}}}label", "Layer 1")
    return layer


def unique_id(root, base):
    ids = {e.get("id") for e in root.iter() if e.get("id")}
    if base not in ids:
        return base
    n = 2
    while f"{base}-{n}" in ids:
        n += 1
    return f"{base}-{n}"


def find_by_id(root, obj_id):
    for e in root.iter():
        if e.get("id") == obj_id:
            return e
    raise ValueError(f"no object with id '{obj_id}'")


def merge_style(elem, updates):
    style = {}
    for part in (elem.get("style") or "").split(";"):
        if ":" in part:
            k, v = part.split(":", 1)
            style[k.strip()] = v.strip()
    style.update({k: str(v) for k, v in updates.items() if v is not None})
    elem.set("style", ";".join(f"{k}:{v}" for k, v in style.items()))


def _as_dict(v):
    if v is None:
        return {}
    if isinstance(v, str):
        v = json.loads(v)
    if not isinstance(v, dict):
        raise ValueError("expected a JSON object")
    return v


def _as_list(v):
    if isinstance(v, str):
        v = json.loads(v)
    if not isinstance(v, list):
        raise ValueError("expected a JSON array")
    return v


def mat_mul(m2, m1):
    """Return m = m2 * m1 (apply m1 first), [a,b,c,d,e,f]."""
    a1, b1, c1, d1, e1, f1 = m1
    a2, b2, c2, d2, e2, f2 = m2
    return [a2 * a1 + c2 * b1,
            b2 * a1 + d2 * b1,
            a2 * c1 + c2 * d1,
            b2 * c1 + d2 * d1,
            a2 * e1 + c2 * f1 + e2,
            b2 * e1 + d2 * f1 + f2]


def parse_transform(elem):
    import re
    t = elem.get("transform") or ""
    m = re.search(r"matrix\(([^)]*)\)", t)
    if m:
        return [float(x) for x in m.group(1).split(",")]
    return [1, 0, 0, 1, 0, 0]


def set_transform_matrix(elem, m):
    elem.set("transform", "matrix(" + ",".join(f"{x:.6g}" for x in m) + ")")


# --------------------------------------------------------------------------- tools

def tool_doc_new(args):
    p = check_path(args["path"])
    if p.exists() and not args.get("overwrite"):
        raise ValueError(f"{p} exists; pass overwrite=true to replace it")
    w, h = args.get("width", 800), args.get("height", 600)
    vb = args.get("viewBox") or f"0 0 {w} {h}"
    bg = args.get("background", "#ffffff")
    svg = f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="{SVG_NS}" xmlns:inkscape="{INKSCAPE_NS}" xmlns:sodipodi="{SODIPODI_NS}" xmlns:xlink="{XLINK_NS}"
     width="{w}" height="{h}" viewBox="{vb}" version="1.1" id="svg1">
  <sodipodi:namedview id="namedview1" inkscape:document-units="{args.get('units', 'px')}" pagecolor="{bg}" borderopacity="1"/>
  <g inkscape:groupmode="layer" inkscape:label="Layer 1" id="layer1"/>
</svg>
'''
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(svg, encoding="utf-8")
    return {"path": str(p), "size": [w, h], "viewBox": vb}


def tool_doc_info(args):
    p = check_path(args["path"])
    root = parse_doc(p)[1]
    items = []
    for e in root.iter():
        if e.get("id") and e.tag in (q(t) for t in ("rect", "ellipse", "circle", "path", "text", "g", "use", "image", "line", "polyline", "polygon")):
            items.append({"id": e.get("id"), "tag": e.tag.split("}")[1],
                          "label": e.get(f"{{{INKSCAPE_NS}}}label"),
                          "style": e.get("style"),
                          "transform": e.get("transform")})
    pages = [e for e in root.iter() if e.get(f"{{{INKSCAPE_NS}}}type") == "page" or e.get("inkscape:page")]
    return {"path": str(p), "width": root.get("width"), "height": root.get("height"),
            "viewBox": root.get("viewBox"), "objects": items, "page_rects": len(pages)}


def tool_add_element(args):
    p = check_path(args["path"])
    tree, root = parse_doc(p)
    tag = args["tag"]
    allowed = {"rect", "ellipse", "circle", "path", "line", "polyline", "polygon", "text", "g", "use", "image", "tspan"}
    if tag not in allowed:
        raise ValueError(f"tag must be one of {sorted(allowed)}")
    obj_id = args.get("id") or unique_id(root, tag)
    elem = ET.Element(q(tag), {"id": obj_id})
    for k, v in _as_dict(args.get("attrs")).items():
        if k.startswith("inkscape:"):
            elem.set(f"{{{INKSCAPE_NS}}}{k.split(':', 1)[1]}", str(v))
        elif k.startswith("xlink:"):
            elem.set(f"{{{XLINK_NS}}}{k.split(':', 1)[1]}", str(v))
        else:
            elem.set(k, str(v))
    if args.get("style"):
        merge_style(elem, _parse_style_str(args["style"]))
    if args.get("fill") or args.get("stroke") or args.get("stroke_width") or args.get("opacity"):
        merge_style(elem, {"fill": args.get("fill"), "stroke": args.get("stroke"),
                           "stroke-width": args.get("stroke_width"), "opacity": args.get("opacity")})
    if args.get("content") is not None:
        elem.text = str(args["content"])
    parent = find_by_id(root, args["parent_id"]) if args.get("parent_id") else top_layer(root)
    if args.get("parent_id") and parent.tag != q("g"):
        raise ValueError("parent_id must reference a group")
    parent.append(elem)
    backup(p)
    save_doc(tree, p)
    return {"id": obj_id, "path": str(p), "parent": parent.get("id")}


def _parse_style_str(s):
    out = {}
    for part in s.split(";"):
        if ":" in part:
            k, v = part.split(":", 1)
            out[k.strip()] = v.strip()
    return out


def tool_set_attrs(args):
    p = check_path(args["path"])
    tree, root = parse_doc(p)
    elem = find_by_id(root, args["id"])
    for k, v in _as_dict(args.get("attrs")).items():
        if k.startswith("inkscape:"):
            elem.set(f"{{{INKSCAPE_NS}}}{k.split(':', 1)[1]}", str(v))
        elif k.startswith("xlink:"):
            elem.set(f"{{{XLINK_NS}}}{k.split(':', 1)[1]}", str(v))
        else:
            elem.set(k, str(v))
    if args.get("style"):
        merge_style(elem, _parse_style_str(args["style"]))
    backup(p)
    save_doc(tree, p)
    return {"id": args["id"], "updated": list((args.get("attrs") or {}).keys()) + (["style"] if args.get("style") else [])}


def tool_get_xml(args):
    p = check_path(args["path"])
    root = parse_doc(p)[1]
    elem = find_by_id(root, args["id"]) if args.get("id") else root
    return {"xml": ET.tostring(elem, encoding="unicode")}


def tool_delete(args):
    p = check_path(args["path"])
    tree, root = parse_doc(p)
    elem = find_by_id(root, args["id"])
    parent = next(e for e in root.iter() if elem in list(e))
    parent.remove(elem)
    backup(p)
    save_doc(tree, p)
    return {"deleted": args["id"]}


def tool_style(args):
    p = check_path(args["path"])
    tree, root = parse_doc(p)
    elem = find_by_id(root, args["id"])
    updates = {"fill": args.get("fill"), "stroke": args.get("stroke"),
               "stroke-width": args.get("stroke_width"),
               "fill-opacity": args.get("fill_opacity"), "stroke-opacity": args.get("stroke_opacity"),
               "stroke-dasharray": args.get("stroke_dasharray"), "opacity": args.get("opacity")}
    merge_style(elem, updates)
    backup(p)
    save_doc(tree, p)
    return {"id": args["id"], "style": elem.get("style")}


def tool_transform(args):
    p = check_path(args["path"])
    tree, root = parse_doc(p)
    elem = find_by_id(root, args["id"])
    cur = parse_transform(elem)
    m = cur
    if args.get("translate"):
        tx, ty = _as_list(args["translate"])
        m = mat_mul([1, 0, 0, 1, tx, ty], m)
    if args.get("scale"):
        s = args["scale"]
        sx, sy = _as_list(s) if not isinstance(s, (int, float)) else [s, s]
        m = mat_mul([sx, 0, 0, sy, 0, 0], m)
    if args.get("rotate_deg") is not None:
        import math
        cx, cy = _as_list(args.get("about", [0, 0]))
        a = math.radians(args["rotate_deg"])
        ca, sa = math.cos(a), math.sin(a)
        rot = [ca, sa, -sa, ca,
               cx - ca * cx + sa * cy, cy - sa * cx - ca * cy]
        m = mat_mul(rot, m)
    if args.get("matrix"):
        m = mat_mul(_as_list(args["matrix"]), m)
    set_transform_matrix(elem, m)
    backup(p)
    save_doc(tree, p)
    return {"id": args["id"], "transform": elem.get("transform")}


def tool_action(args):
    p = check_path(args["path"])
    actions = args["actions"]
    export_to = args.get("save_to")
    if export_to:
        out = check_path(export_to)
        if ";" in str(out):
            raise ValueError("save_to must not contain ';'")
        # export is an ACTION in the Rosepen CLI, not a flag
        actions = f"{actions};export-filename:{out};export-do"
    rc, out, err = run_rosepen([p, f"--actions={actions}"], timeout_s=args.get("timeout_s", 120))
    return {"rc": rc, "stdout": out[-8000:], "stderr": err[-8000:],
            "saved_to": str(export_to) if export_to else None}


def tool_export(args):
    p = check_path(args["path"])
    out = check_path(args["out"])
    fmt = args.get("fmt", Path(out).suffix.lstrip(".") or "png")
    cmd_args = [p, f"--export-type={fmt}", f"--export-filename={out}"]
    if args.get("page"):
        cmd_args.append(f"--export-page={args['page']}")
    if args.get("dpi"):
        cmd_args.append(f"--export-dpi={args['dpi']}")
    if args.get("width"):
        cmd_args.append(f"--export-width={args['width']}")
    if args.get("height"):
        cmd_args.append(f"--export-height={args['height']}")
    if fmt == "png":
        cmd_args += [f"--export-background={args.get('background', '#ffffff')}",
                     "--export-background-opacity=1"]
    rc, stdout, err = run_rosepen(cmd_args, timeout_s=args.get("timeout_s", 120))
    ok = out.exists() and out.stat().st_size > 0
    return {"rc": rc, "out": str(out), "bytes": out.stat().st_size if ok else 0,
            "stderr": err[-2000:] if rc else None}


def tool_render(args):
    p = check_path(args["path"])
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "render.png"
        cmd_args = [p, "--export-type=png", f"--export-filename={out}",
                    f"--export-dpi={args.get('dpi', 96)}",
                    f"--export-background={args.get('background', '#ffffff')}",
                    "--export-background-opacity=1"]
        if args.get("page"):
            cmd_args.append(f"--export-page={args['page']}")
        rc, _, err = run_rosepen(cmd_args, timeout_s=args.get("timeout_s", 120))
        if rc or not out.exists():
            return {"type": "text", "text": f"render failed rc={rc}: {err[-1500:]}"}
        data = base64.b64encode(out.read_bytes()).decode()
    return {"type": "image", "data": data, "mimeType": "image/png"}


def tool_list_actions(args):
    rc, out, _ = run_rosepen(["--action-list"], timeout_s=60)
    lines = out.splitlines()
    if args.get("filter"):
        fl = args["filter"].lower()
        lines = [ln for ln in lines if fl in ln.lower()]
    return {"rc": rc, "count": len(lines), "actions": [ln.split(":")[0].strip() for ln in lines[:400]]}


def tool_history(args):
    p = check_path(args["path"])
    hist = Path(str(p) + ".mcp-history")
    snaps = sorted(x.name for x in hist.glob("*.svg")) if hist.exists() else []
    return {"path": str(p), "snapshots": snaps}


def tool_restore(args):
    p = check_path(args["path"])
    snap = Path(str(p) + ".mcp-history") / args["snapshot"]
    if not snap.exists():
        raise ValueError(f"no such snapshot: {snap}")
    backup(p)
    shutil.copy2(snap, p)
    return {"restored": str(snap), "path": str(p)}


TOOLS = {
    "doc_new": (tool_doc_new, "Create a new Rosepen SVG document.", {
        "path": {"type": "string", "description": "absolute path of the .svg to create"},
        "width": {"type": "integer", "description": "canvas width (default 800)"},
        "height": {"type": "integer", "description": "canvas height (default 600)"},
        "viewBox": {"type": "string"}, "background": {"type": "string"},
        "overwrite": {"type": "boolean", "description": "replace an existing file (default false)"},
    }),
    "doc_info": (tool_doc_info, "Inventory a document: size, viewBox, object list (id, tag, label, style, transform).", {
        "path": {"type": "string"},
    }),
    "add_element": (tool_add_element, "Add an SVG element on top of the top layer (or inside parent_id).", {
        "path": {"type": "string"},
        "tag": {"type": "string", "enum": ["rect", "ellipse", "circle", "path", "line", "polyline", "polygon", "text", "g", "use", "image", "tspan"]},
        "attrs": {"type": "object", "description": "geometry attrs, e.g. {\"x\":10,\"y\":20,\"width\":40,\"height\":30} or {\"d\":\"M ...\"} or {\"cx\":..,\"cy\":..,\"r\":..}; inkscape:/xlink: prefixed keys are mapped to those namespaces"},
        "id": {"type": "string", "description": "optional id, auto-generated otherwise"},
        "style": {"type": "string", "description": "CSS style string, e.g. 'fill:#ff0000;stroke:none'"},
        "fill": {"type": "string"}, "stroke": {"type": "string"}, "stroke_width": {"type": "number"},
        "content": {"type": "string", "description": "text content (for tag=text)"},
        "parent_id": {"type": "string", "description": "id of a group to append to instead of the top layer"},
    }),
    "set_attrs": (tool_set_attrs, "Set attributes (and optionally a style string) on an existing object.", {
        "path": {"type": "string"}, "id": {"type": "string"},
        "attrs": {"type": "object"}, "style": {"type": "string"},
    }),
    "get_xml": (tool_get_xml, "Read the XML of one object (id) or the whole document.", {
        "path": {"type": "string"}, "id": {"type": "string"},
    }),
    "delete": (tool_delete, "Delete an object.", {"path": {"type": "string"}, "id": {"type": "string"}}),
    "style": (tool_style, "Set paint on an object.", {
        "path": {"type": "string"}, "id": {"type": "string"},
        "fill": {"type": "string", "description": "#rrggbb or none"},
        "stroke": {"type": "string"}, "stroke_width": {"type": "number"},
        "fill_opacity": {"type": "number"}, "stroke_opacity": {"type": "number"},
        "stroke_dasharray": {"type": "string"}, "opacity": {"type": "number"},
    }),
    "transform": (tool_transform, "Transform an object, composed onto its existing matrix.", {
        "path": {"type": "string"}, "id": {"type": "string"},
        "translate": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2},
        "scale": {"type": ["number", "array"], "description": "number or [sx,sy]"},
        "rotate_deg": {"type": "number"},
        "about": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2, "description": "rotation centre, default [0,0]"},
        "matrix": {"type": "array", "items": {"type": "number"}, "minItems": 6, "maxItems": 6},
    }),
    "action": (tool_action, "Run Rosepen headless actions on a document, e.g. 'select-by-id:content,frame;object-powerclip-place' or 'dimension-add:horizontal,a@bbox:l,b@bbox:r,id=d1'.", {
        "path": {"type": "string"},
        "actions": {"type": "string", "description": "semicolon-separated actions; single action parameter follows the first ':'"},
        "save_to": {"type": "string", "description": "export the result as SVG to this path"},
        "timeout_s": {"type": "integer"},
    }),
    "export": (tool_export, "Export the document: png|pdf|svg|ps|eps.", {
        "path": {"type": "string"}, "out": {"type": "string"},
        "fmt": {"type": "string", "enum": ["png", "pdf", "svg", "ps", "eps"]},
        "page": {"type": "integer"}, "dpi": {"type": "integer"},
        "width": {"type": "integer"}, "height": {"type": "integer"},
        "background": {"type": "string", "description": "png only, default #ffffff"},
    }),
    "render": (tool_render, "Render the document (or one page) to a PNG and return it as an image for visual inspection.", {
        "path": {"type": "string"}, "page": {"type": "integer"},
        "dpi": {"type": "integer"}, "background": {"type": "string"},
    }),
    "list_actions": (tool_list_actions, "List available Rosepen actions (filter substring).", {
        "filter": {"type": "string"},
    }),
    "history": (tool_history, "List automatic backups of a document.", {"path": {"type": "string"}}),
    "restore": (tool_restore, "Restore a backup snapshot.", {
        "path": {"type": "string"}, "snapshot": {"type": "string"},
    }),
}


# --------------------------------------------------------------------------- MCP plumbing

def handle(req):
    method = req.get("method")
    rid = req.get("id")
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION}}}
    if method == "notifications/initialized":
        return None
    if method == "ping":
        return {"jsonrpc": "2.0", "id": rid, "result": {}}
    if method == "tools/list":
        required = {
            "doc_new": ["path"], "add_element": ["path", "tag"],
            "set_attrs": ["path", "id"], "delete": ["path", "id"],
            "style": ["path", "id"], "transform": ["path", "id"],
            "action": ["path", "actions"], "export": ["path", "out"],
            "restore": ["path", "snapshot"], "get_xml": ["path"],
            "doc_info": ["path"], "history": ["path"],
        }
        return {"jsonrpc": "2.0", "id": rid, "result": {"tools": [
            {"name": name, "description": desc,
             "inputSchema": {"type": "object",
                             "properties": schema,
                             "required": required.get(name, [])}}
            for name, (_, desc, schema) in TOOLS.items()]}}
    if method == "tools/call":
        name = req["params"]["name"]
        args = req["params"].get("arguments") or {}
        if name not in TOOLS:
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": f"unknown tool {name}"}], "isError": True}}
        try:
            res = TOOLS[name][0](args)
        except Exception as exc:  # noqa: BLE001 — tool errors go back to the agent
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": f"{type(exc).__name__}: {exc}"}], "isError": True}}
        if isinstance(res, dict) and res.get("type") == "image":
            payload = {"content": [{"type": "image", "data": res["data"], "mimeType": res["mimeType"]}]}
        else:
            payload = {"content": [{"type": "text", "text": json.dumps(res, ensure_ascii=False, default=str)}]}
        return {"jsonrpc": "2.0", "id": rid, "result": payload}
    if method and method.startswith("resources/"):
        return {"jsonrpc": "2.0", "id": rid, "result": {} if method.endswith("list") else {"contents": []}}
    return {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": f"method not found: {method}"}}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            continue
        resp = handle(req)
        if resp is not None:
            sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
