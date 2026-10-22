---
phase: 05-corel-tools
plan: 01
type: Summary
about: corel
---

# 05-01 SUMMARY — PowerClip (place inside frame, edit contents in place, extract)

Status: **COMPLETE 2026-10-22** — applied in the parallel 05-01+05-02 working tree,
gated by claude-opus-5-5: **PASS-WITH-FIXES** (1 defect fixed, 1 HIGH deferred as
structural), all suites green, committed with 05-02 by the orchestrator.

## What was built
- `Inkscape::PowerClip` Gtk-free/desktop-free document model (`src/object/powerclip.{h,cpp}`):
  role attribute `inkscape:powerclip` = frame|container|contents; frame = one SVG group
  holding container + contents group; live clip = `clipPath` in defs containing one
  `svg:use` of the container (live reshape follows for free).
  Queries (`is_frame`, `frame_of`, `container_of`, `contents_of`, `content_items`,
  `frames`), `Refusal` + stable `refusal_message`, `place_inside` (new frame and
  add-to-frame, target = last selected, contents keep absolute page position),
  `extract` (dissolves frame, keeps visual positions, selects contents),
  `normalize` (repairs non-identity contents transform and stale/shared clips after
  duplicate), `edit_target`/`finish_target`.
- Re-parenting idiom: `reparent_keep_visual(doc, repr, old_i2doc, new_parent, after)` —
  GC-anchored move keeping the id, then `doWriteTransform(old_i2doc * parent->i2doc_affine().inverse())`
  (powerclip.cpp:50-74). Shared by place/extract/normalize.
- Five headless-safe actions (`src/actions/actions-powerclip.{h,cpp}`):
  `app.object-powerclip-place|extract|edit|finish|list` (`list` = stdout model oracle,
  `powerclip-count <n>`). Exactly one `DocumentUndo::done` per successful change; none
  on refusal. Headless `show_output` text goes to **stderr** (recorded in the corpus).
- GUI entry points: context menu block ("Clipping and Masking"), Object ▸ Power_Clip
  submenu in menus-corel.ui, `Ctrl+Alt+P` in rosepen.xml (free trigger; overrides stay 40).
- Tests: `testfiles/src/powerclip-test.cpp` (22 cases), `testfiles/cli_tests/check_powerclip.py`
  + 8-scenario corpus (place, transform-follow, container-move, extract, add-to-frame,
  duplicate-move, master, refuse-single) with model/svg/png stages.

## Verification (orchestrator, this tree)
- `cmake --build build -j` green, no warnings in any new file.
- `ctest -R "test_powerclip|cli_powerclip|..."`: **test_powerclip 22 cases** +
  **cli_powerclip 8/8** (also green after the gate fixes).
- Negative self-tests: flipped expected colour → rc 1 `STAGE png FAIL`; frame count 2
  for `place` → rc 1 `STAGE model FAIL`.
- `--action-list` = 5 object-powerclip-* actions. Gtk-free grep clean. No
  `namespace Inkscape` line added/removed. Zero protected upstream files touched.
- `check_corel_menus.py` → `CHECK_COREL_MENUS_OK items=254 top=11` (249 + 4 PowerClip
  + 1 dimension tool). `check_corel_keys.py` → `CHECK_COREL_KEYS_OK binds=77 overrides=40`
  (75 + 2 aliases). `fork/scripts/verify-branding.sh` → `ALL_BRANDING_CHECKS_OK`
  (allow-list block `# 05-01 PowerClip` present; `# 05-02 dimension tools` added by
  orchestrator for the 6 flagged 05-02 paths).

## Opus gate (claude-opus-5-5, 2026-10-22)
- **Fixed, HIGH** — `extract` deleted the frame group's extra children (anything drawn
  inside the frame after Enter Group, e.g. a dimension). Extract now moves every child
  out with its visual position; new gtest `extractKeepsObjectsDrawnInsideTheFrameGroup`.
- **Open, HIGH (deferred, structural)** — with the default clone-compensation pref,
  moving the CONTAINER ALONE with the Select tool counter-moves the clip's `svg:use`
  (upstream `SPUse::move_compensate`, sp-use.cpp:496), so the clip stays behind until
  Edit/Finish Editing (normalize re-creates the stale clip, one "Repair PowerClip"
  undo step). Reshape (Node tool), whole-frame moves and non-translation resizes are
  unaffected. `cli_powerclip-container-move` therefore drives the container via
  `object-set-attribute:transform` instead of `transform-translate`. A real fix needs a
  wrapper group between clip use and container or a hook in protected sp-use/selection
  code → **Phase 5 polish plan**. Recorded in 05-01-UAT.md item 5.

## Known differences from CorelDRAW (design notes, per plan)
Container outline paints under the contents (inner stroke half covered); contents stay
clipped while editing; double-click enters the frame group first; Ungroup breaks the
frame (contents keep their clip); extract dissolves the frame. Deferred: lock-contents
toggle, empty frames, auto-centre, outline-over-contents, unclipped edit preview,
single double-click into contents, Ungroup=extract, Object Manager type labels,
classic menus.ui entry, container-move clone-compensation fix (gate finding #2).

## Integration points (orchestrator)
Shared files (src/CMakeLists.txt, src/inkscape-application.cpp, src/ui/contextmenu.cpp,
share/ui/menus-corel.ui, share/keys/rosepen.xml, testfiles/*/CMakeLists.txt,
check_corel_menus.py, verify-branding.sh) carry both plans' append-only blocks and are
committed together with 05-02 by the orchestrator, then re-verified.

## UAT
`.paul/phases/05-corel-tools/05-01-UAT.md` — 13 items, **OWED** (no Xvfb; live GNOME
session not intruded). Includes the clone-compensation divergence probe (item 5).

## Handoff
Any SPItem can be PowerClip content (dimension objects included); `inkscape:powerclip`
roles are reserved. 05-03 Artistic Media / 05-04 Smart Fill / 05-05 LiveSketch next;
06-01 .cdr import can map CDR PowerClip records onto `place_inside`.
