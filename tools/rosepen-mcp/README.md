# Rosepen MCP

A [Model Context Protocol](https://modelcontextprotocol.io) server that lets AI
agents paint in [Rosepen](https://github.com/toel1234/rosepen) documents.

Python stdlib only, no dependencies. The document **file is the state**: every
mutating tool edits the SVG directly (with an automatic timestamped backup) or
runs the Rosepen binary headlessly. Agents see what they painted via `render`,
which returns a real PNG rendered by Rosepen.

## Tools

| Tool | What it does |
|---|---|
| `doc_new` | New document (size, viewBox, background) |
| `doc_info` | Size, viewBox, object inventory (id, tag, label, style, transform) |
| `add_element` | Add rect/ellipse/circle/path/line/polyline/polygon/text/g/use/image on top of the top layer |
| `set_attrs` / `get_xml` / `delete` | Attribute edits, XML reading, deletion |
| `style` | fill / stroke / stroke-width / opacities / dasharray |
| `transform` | translate, scale, rotate (about a point), raw matrix — composed onto the existing transform |
| `action` | Run any of Rosepen's 1500+ headless actions (PowerClip, dimensions, pages, …), optionally saving the result |
| `export` | png / pdf / svg / ps / eps |
| `render` | Render (a page) to PNG, returned as an MCP image for visual inspection |
| `list_actions` | Search the action surface |
| `history` / `restore` | Automatic backups per document |

## Run

```sh
python3 rosepen_mcp.py
```

Environment: `ROSEPEN_BIN` (binary; default: `rosepen` on PATH, then the repo's
`build/bin/rosepen`), `ROSEPEN_ROOTS` (colon-separated roots agents may write
in; default: the server's working directory).

## Register

```sh
# Claude Code (user scope); run from the repo root
claude mcp add -s user rosepen -- python3 "$PWD/tools/rosepen-mcp/rosepen_mcp.py"

# CommandCode: ~/.commandcode/mcp.json (use the absolute path of your checkout)
"rosepen": {"transport": "stdio", "enabled": true,
            "command": "python3",
            "args": ["/absolute/path/to/repo/tools/rosepen-mcp/rosepen_mcp.py"]}
```

## Agent workflow that works

1. `doc_new` → `add_element` (shapes/text) → `style` / `transform`
2. `action` for Rosepen semantics, e.g.
   `select-by-id:content,frame;object-powerclip-place` or
   `dimension-add:horizontal,a@bbox:l,b@bbox:r,id=d1`
3. `render` to look at it, `export` for the artifact

Rosepen actions take effect only when the document is saved back — pass
`save_to` (or include `export-filename:<p>;export-do` in the action string,
since `--export-do` is an action, not a CLI flag).
