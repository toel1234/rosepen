---
phase: 05-corel-tools
plan: 03
type: Summary
about: corel
---

# 05-03 SUMMARY — Artistic Media (live preset/brush/calligraphic strokes on an editable skeleton)

Status: **COMPLETE 2026-10-23** — applied by the worker pipeline (deepseek Tasks 1–2,
GLM subagent Task 3), gated by claude-opus-5-5: **PASS-WITH-FIXES** (9 defects, 3 HIGH),
all suites green, committed by the orchestrator.

## What was built
- Pure model `src/live_effects/artistic-media-geometry.{h,cpp}`: arc-length map-along
  (ONE full shape copy per skeleton subpath, u runs once along it; corners stitched into
  one closed ring with bevel/overlap joins), flat-nib sweep with PAGE-FIXED angle θ
  (mapped into path coordinates through inverse i2doc — an LPE path keeps its
  `transform` attribute, R2), length/area-fraction helpers.
- `artistic-media-library.{h,cpp}`: 11 built-in preset/brush rows (unit-space shape
  data + area fractions); `app.artistic-library` prints them.
- `LPEDimension`-style live LPE `lpe-artistic-media` (key `artistic_media`, after
  DIMENSION in the enum): skeleton = the path's own original-d; output = filled brush
  outline; width stored in PATH-LOCAL user units (deviation from the plan's "mm" —
  the actions convert; internally consistent, recorded); scale-with-object via
  delta-transform against the transform still on the item.
- Actions `app.artistic-add|apply|set|list|break|library` (6 registered): validate
  everything before any write (shared-effect fork happens only after validation),
  one undo per change, deterministic ids, errors prefixed `artistic-add/apply/set:`.
- Interactive Artistic Media tool (`/tools/artistic`, pref group `eventcontext
  id="artistic"`, icon `taper-stroke`, key `i` — a documented Spiral override in the
  rosepen golden table): press–drag capture ≥2 px, raw-polyline preview through the
  shared pure model, commit only via `ArtisticOps::create` into the current layer,
  click selects, Esc cancels; property-bar toolbar (mode/shape/width/angle/smoothing/
  scale) writes prefs AND applies to the selection via `ArtisticOps::apply/set`;
  flyout `[ArtisticMedia;Calligraphic;Spray]`; Tools menu + Object ▸ Break Artistic
  Media Apart.

## Verification (orchestrator, final tree after the gate)
- Build clean, no warnings.
- gtests: artistic-media-geometry 25 + artistic-media-lpe 26 cases (incl. one-ring
  closed-square, page-fixed nib mirror sampling, move-after-scale width,
  break-apart-after-move).
- `cli_artistic-media` 13/13; negative self-tests: wrong live-edit length → rc 1
  `STAGE list FAIL`; brush-bristle pixel → rc 1 `STAGE png FAIL`.
- `TOOLBOX_GROUPS_OK groups=14 tools=26`; `CHECK_COREL_MENUS_OK items=256 top=11`;
  `CHECK_COREL_KEYS_OK binds=77 overrides=40` (overrides documented);
  `ALL_BRANDING_CHECKS_OK`.
- Regression: toolbox/menus/keys/docker/property-bar/workspace 8/8, powerclip+dimension
  20/20, page-ops/master-layer/pdfx/color-corpus/fills CLI 49/49, `test_lpe` pass/fail
  set unchanged vs baseline. Uncommitted delta touches no protected upstream files.

## Opus gate (claude-opus-5-5, 2026-10-23) — 9 findings fixed
1. HIGH map_along copied the shape per EDGE (taper repeated per corner; tool preview
   = hundreds of lenses) → one arc-length skeleton per subpath, ring stitching.
2. HIGH tool captured px but `create` expects user units → mm documents were 3.78×
   off; converted with `c2p.inverse()` (dimension-tool idiom).
3. HIGH nib rotated with the object (LPE paths keep `transform`) → page-fixed θ via
   inverse i2doc + mirror-sampling gtest.
4. MEDIUM `transform_multiply` treated absolute handover as delta (width divided by
   scale on every move after a scale) → delta vs the item's current transform + gtest.
5. MEDIUM break_apart skeleton sibling lost the stroke's `transform` → copied + gtest.
6. MEDIUM `set` forked shared effects before validation (write with no undo on
   rejection) → validate first, refetch repr, byte-identical-XML test.
7. MEDIUM toolbar dropdown blank on first show/after mode switch; sensitivity used the
   wrong mode → rebuild-then-lookup + `_shown_mode`.
8. LOW width clamp 1000 cut large px-document widths → 1e9 (mm limit stays in actions).
9. LOW `apply` errors said `artistic-set:` and ignored `color=` → fixed.

## Recorded decisions / residual risks (UAT probes)
- R2: `doWriteTransform` keeps a `transform` attribute on LPE paths — the reason for
  fixes 3–5; nib page-fixedness is expressed in path coordinates.
- Closed skeletons keep a flat seam at the start point (gtest-locked); very sharp
  corners fold (recorded); parent-group transform changes do not re-run the effect
  until its next update; toolbar does not follow selection changes after first show
  (same gap as the dimension toolbar); `artistic-list` intentionally re-runs effects
  and may trigger the no-undo shape repair on load.
- Deferred per plan: sprite/scan strokes from user images, media library management,
  pressure input, Envelope/perspective distortions.

## UAT
`.paul/phases/05-corel-tools/05-03-UAT.md` — 16 items, **OWED** (no Xvfb; live GNOME
session not intruded).

## Handoff
05-04 Smart Fill and 05-05 LiveSketch can reuse the pure-model + single-LPE pattern;
LiveSketch strokes can be fed to `ArtisticOps::create`; 06-01 .cdr import can map CDR
artistic-media strokes onto `app.artistic-add` specs. MCP note: agents can create
artistic strokes headlessly via the `action` tool (`app.artistic-add`).
