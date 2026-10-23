<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- The "rosepen-cdr-export/1" model and the data-rp-* import attributes. -->
<!-- Copyright (C) 2026 Authors -->
<!-- Released under GNU GPL v2+, read the file 'COPYING' for more information. -->
<!-- The linked TraceDraw crates are MIT OR Apache-2.0 (see THIRD-PARTY.md). -->

# The Rosepen bridge contract

Two interfaces live here, both Rosepen's own (not TraceDraw's internal
schema, which is unversioned and private):

- **Import direction**: annotated page SVG — the serializer's page SVG plus
  the `data-rp-*` attributes and layer markup described below.
- **Export direction**: the `rosepen-cdr-export/1` JSON model, which the Rust
  builder (`src/export.rs`) turns into a document and writes out.

## Import: annotated page SVG

`rp_cdr_page_svg` returns, for one page, the serializer's SVG (millimetres,
Y down, one group per visible layer) with these changes:

1. **Ids.** Every bare integer id becomes `td<digits>` (`id="7"` →
   `id="td7"`). The serializer's own `grad<N>`, `clip<N>`, `mask<N>` and
   `maskinv<N>` ids and the `url(#...)` references to them are untouched,
   since a reference never names a bare shape id. Elements of one object
   written under the same id (an object's fill and outline parts, a table's
   cells) keep sharing that one `td<N>`; a caller must not assume ids are
   unique on a page.
2. **Layers.** The root element declares
   `xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"` and every
   top-level layer group gains
   `inkscape:groupmode="layer" inkscape:label="<source layer name>"` (the
   name is XML-escaped).
3. **Hidden layers.** A layer hidden in the file is still written (its
   shapes are present) and its group carries `style="display:none"`.
4. **Source colours.** An element whose own `fill` equals the sRGB preview
   of a CMYK or spot paint gets `data-rp-fill`; likewise `data-rp-stroke`
   for an outline painted with its own colour. The attribute means "the
   file's numbers for this paint", and the element's `fill`/`stroke` keeps
   that same sRGB preview. Other colour models (RGB, Gray, Lab, …) and
   Registration are not annotated.

### `data-rp-fill` / `data-rp-stroke` grammar

```
value  = "cmyk:" num "," num "," num "," num
       / "spot:" num "," num "," num "," num "," num "," name
num    = decimal number, 0..1, up to six decimals, trailing zeros dropped ("0.1", "1")
name   = percent-encoded ink name (see below)
```

For `spot`, the numbers are `tint,c,m,y,k` — the tint (0..1) first, then the
ink's process stand-in at full strength.

Ink names are percent-encoded: every byte that is not an ASCII letter,
digit, `-`, `_`, `.`, `~` or part of a UTF-8 multibyte sequence becomes
`%XX` (uppercase hex). So a space is `%20`, a comma `%2C`, `%` itself `%25`.
`src/annotate.rs::percent_decode` is the decoder.

## Export: `rosepen-cdr-export/1`

A single JSON object (unknown members are ignored; a different `schema`
string is an error, reported as `RP_ERR_JSON`):

```json
{
  "schema": "rosepen-cdr-export/1",
  "title": "Drawing",
  "inks": [{"name": "Pantone 123", "cmyk": [0.0, 0.5, 1.0, 0.0]}],
  "pages": [
    {
      "name": "Page 1",
      "width_mm": 210.0,
      "height_mm": 297.0,
      "layers": [
        {"name": "Artwork", "visible": true, "shapes": [SHAPE, ...]}
      ]
    }
  ],
  "master": [LAYER, ...]
}
```

- `inks` are the spot inks the shapes refer to by index; each carries its
  process stand-in as CMYK 0..1.
- `pages` must not be empty. `layers` is in bottom-to-top order, `shapes`
  too.
- `master` layers are drawn under every page.

`LAYER` = `{"name": S, "visible": B (default true), "shapes": [SHAPE]}`.

`SHAPE` (every variant also takes `name` (optional string), `opacity`
(default 1, 0..1), `fill` (PAINT or null) and `stroke` (STROKE or null)):

| `kind` | Fields |
| --- | --- |
| `rect` | `x`, `y`, `w`, `h`, `rx` (corner radius, default 0) |
| `ellipse` | `cx`, `cy`, `rx`, `ry` |
| `path` | `d` — absolute SVG path data, page millimetres, Y down |
| `text` | `x`, `y` (first baseline), `align` (`left`/`center`/`right`), `spans` |
| `group` | `children` (array of SHAPE) |

`SPAN` = `{"text": S, "family": S, "size_pt": F, "bold": B, "italic": B}`.

`PAINT` = `{"model": "rgb", "r", "g", "b"}` |
`{"model": "cmyk", "c", "m", "y", "k"}` |
`{"model": "spot", "ink": INDEX, "tint": F}` — all components 0..1.

`STROKE` = `{"paint": PAINT, "width_mm": F, "cap": "butt"|"round"|"square",
"join": "miter"|"round"|"bevel", "miter": F (ratio, default 4),
"dash_mm": [F, ...]}`.

Coordinates are in page millimetres with **Y down** from the page's top-left
corner. The builder flips them (`y' = page_h - y`) into the document model's
bottom-left, Y-up space.

### What the writer cannot carry (each becomes a warning, never an error)

The format's writer states one page size for the whole file and has no ink
table; these are FINDINGS against the vendored writer, recorded here so the
Rosepen side can compensate:

1. **One page size.** Pages of different sizes are written as the first
   page's size (content stays at the same distance from its top-left corner)
   and a warning says so.
2. **Spot inks.** A spot colour is written as its process stand-in (tint
   applied), so the ink name and the fact that it was a spot do not survive
   an export → import round trip.
3. **Master layers.** `master` shapes are copied into a bottom layer named
   "Master" on every page (the writer has no master-layer concept), with a
   warning.
4. **Group transparency.** Only an object's own `opacity` is written, and it
   travels in the style string, which the writer emits from version 16;
   versions 7–15 lose it.
5. **Text baseline.** Before version 16 the file keeps a text object's place
   in the object's transform, not in the text argument; the placed position
   is right, the field is not.
6. **Colour precision.** CMYK is stored as integer percentages, so values
   are quantised to 1 % steps.
