---
phase: 05-corel-tools
plan: 02
type: Summary
about: corel
---

# 05-02 SUMMARY — Live dimension tools (dimension LPE, actions, Dimension tool)

Status: **COMPLETE 2026-10-22** — applied in the parallel 05-01+05-02 working tree,
gated by claude-opus-5-5: **PASS-WITH-FIXES** (7 findings fixed), all suites green,
committed with 05-01 by the orchestrator.

## What was built
- Pure model `src/live_effects/dimension-geometry.{h,cpp}`
  (`Inkscape::LivePathEffect::Dimension`, lib2geom/glibmm only): `Kind`
  {aligned, horizontal, vertical, angular, segment}, anchor grammar
  `bbox:<tl..br>` / `node:<k>` / `rel:<u>/<v>` + free points, `build()` per kind
  (extension lines with gap/overshoot, dimension line/arc, closed filled arrowheads
  with outside-flip below 2.5×arrow length, upright label angle),
  `format_label` (locale-independent, '.' decimal, "-0.00"→"0.00"), `to_unit`.
- `LPEDimension` (`lpe-dimension.{h,cpp}`, key `dimension`, experimental/hidden):
  SatelliteParam refs `start_ref/end_ref/vertex_ref` + anchors, mm→uu via
  `doc->getDocumentScale()` with a Jacobian correction for non-uniform parent scale
  (`len_scale = doc_scale/√|det|`); points converted to document coords via
  `i2doc_affine()` before building; `last_points` freeze on deleted/invalid refs;
  label sibling found structurally by class (duplicate/copy never cross-wires);
  **parent `connectTransformed` signal** (lpe-dimension.cpp:557-572) keeps linked
  dims glued when an ancestor moves the group (gtest-proven need).
- Actions `src/actions/actions-dimension.{h,cpp}`: `DimensionOps::create/list/set` +
  `app.dimension-add|list|set` (string params, deterministic ids via `id=`,
  errors to stderr, one undo per step).
- Interactive `DimensionTool` (`/tools/dimension`, cursor measure.svg) with
  Auto/Parallel/Angular/Segment modes, snapping + anchor capture on document bounds,
  commit through the same `DimensionOps::create`; `DimensionToolbar` +
  `share/ui/toolbar-dimension.ui`; toolbox flyout `[group:dimension]
  Dimension;Measure;Connector` (face = Dimension); Tools ▸ Dimension Tool menu item;
  `TOOLS_DIMENSION` appended at the end of `tools_enum` (no renumbering).
- Tests: `dimension-geometry-test` (33 cases), `dimension-lpe-test` (14 cases incl.
  live move, freeze-on-delete, save/reload still live, undo, duplicate),
  `check_dimension.py` + 9 CLI scenarios (horizontal, vertical, aligned, segment,
  angular, live-move, precision-unit, freeze-on-delete, bad-input; svg/png[/pdf]).

## Decisions (as implemented, verified in code)
- Wrap-in-new-LPE (not a new SP object): plain path + text exports with zero
  pipeline change; label found structurally by `rosepen-dimension` class.
- Ancestor transforms: `connectTransformed` on the parent chain IS wired.
- Corpus moves measured objects via `object-set-attribute:x,120` —
  `transform-translate` is locale-px based (recorded in the fixture comment).
- `dimension-add` without a desktop parents the dimension to the measured item's
  parent (nearest group — a tspan parent climbs to its group).

## Verification (orchestrator, this tree)
- Build green, no warnings in the new files.
- `ctest`: **test_dimension-geometry 33 + test_dimension-lpe 14 + cli_dimension 9/9**,
  also green after the gate fixes; `test_lpe*` untouched (no live_effects file of
  upstream modified; measure-segments/measure-tool diff vs HEAD = empty).
- Negative self-test: live-move value 70.0→69.9 → rc 1 `STAGE list FAIL`.
- `--action-list` = 3 dimension-* actions. `check_toolbox_groups.py` →
  `TOOLBOX_GROUPS_OK groups=14 tools=25` (toolbox-groups-test 24→25 only).
  Menus `items=254`, keys `binds=77 overrides=40` (rosepen.xml untouched by 05-02).
- Branding sweep `ALL_BRANDING_CHECKS_OK`; allow-list block `# 05-02 dimension tools`
  added by orchestrator (6 flagged paths).

## Opus gate (claude-opus-5-5, 2026-10-22) — 7 findings fixed
1. HIGH angular arrows: end arrowhead used arc tangent at the START point → wrong
   direction on wide arcs; each arrow now follows the arc at its own end (+ hand-derived
   arrowhead bbox assertions in Angular90).
2. HIGH value under scaled parent: measured in path coordinates, converted with doc
   scale only → wrong number inside a scaled group; now converted to document units
   first (+ gtest `scaledParentDoesNotChangeTheValue`: ×2 group still "50.00 mm").
3. HIGH use-after-free: raw pointers to measured items survived a sibling's deletion;
   release signal now clears them, null entries skipped.
4. MEDIUM `dimension-set` atomicity: bad later option left earlier ones written outside
   undo; all options validated before any write (`set_option_attr`).
5. MEDIUM tool offset sign mirrored aligned lines / stale second point during drag /
   anchor capture used local bbox; rewritten per kind, defaults until both points set,
   document-bounds capture.
6. LOW create ignored `offset=` on first build; parent fallback could be `svg:text`.
7. LOW double `DocumentUndo::done` in the tool; toolbar unit dropdown fired
   `unit_changed` (undo step) on first switch — guarded.

## Residual (recorded, cosmetic/by-design)
Non-uniform scale label uses average scale factor (exact for uniform + rotation);
rotated parents measure horizontal/vertical along rotated axes; tool preview always
draws the label above the line (committed object honours the setting); CLI corpus
doesn't cover the three new gtest-only cases.

## Deferred (per plan scope limits)
Curve-length dims, 3-point callouts, baseline/chained dims, named dimension styles,
Object Properties dimension kind panel (shows as group), localized decimal separator,
label font picker beyond font-size, "dynamic dimensioning off" toggle.

## UAT
`.paul/phases/05-corel-tools/05-02-UAT.md` — 14 items, **OWED** (no Xvfb; live GNOME
session not intruded).

## Handoff
05-03 Artistic Media can reuse the LPE-group pattern; a dimension inside a PowerClip
works (any SPItem is content); Phase 6 .cdr import can map CDR dimension records onto
`app.dimension-add` specs; Object Properties kind panel for dimensions = follow-up plan.
