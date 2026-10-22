---
description: "corel — current position and accumulated context"
type: ProjectState
about: "corel"
---

# Project State

## Project Reference

See: .paul/PROJECT.md (updated 2026-10-20)

**Core value:** CorelDRAW-class open source vector/print app on Linux, built on Inkscape.
**Current focus:** Phases 1-4 COMPLETE (GUI + shell + PDF/X); next Phase 5 (Corel tools) and Phase 6 (.cdr import)

## Current Position

Milestone: v0.1 Corel parity v1
Phase: 5 IN PROGRESS (05-01 ✓ 05-02 ✓ 05-03 ✓); Phases 1-4 COMPLETE
Plan: 05-03 Artistic Media applied + gated (opus PASS-WITH-FIXES, 9 fixes) + committed this session
Status: 03-01 ✓ 03-02 ✓ 03-03 ✓ 04-01 ✓ 04-02 ✓ 04-03 ✓ 04-04 ✓ 05-01 ✓ 05-02 ✓ 05-03 ✓ — next: 05-04 (Smart Fill) authoring via opus; also owed: Rosepen MCP interface delivered (tools/rosepen-mcp, agents paint headlessly); Phase 5 polish plan owes the container-move clone-compensation fix (05-01 gate finding #2)
Last activity: 2026-10-23 — 05-03 gated, summarized, committed (orchestrator session)

Progress:
- Milestone: [█████████░] 86% (18 of 21 plans)

## Loop Position

```
Phase 4: COMPLETE ✓   Phase 5: 05-01 ✓ 05-02 ✓ 05-03 ✓ | 05-04 05-05 pending   Phase 6: 06-01 06-02 pending
→ next: 05-04 (Smart Fill) authoring via opus; Phase 5 polish plan (container-move clone compensation)
```

## Accumulated Context

### Decisions
- Fork Inkscape, keep GTK4; Linux only
- Worker pipeline: GLM-5.3 subagents + command-code deepseek-v4.1-flash + antigravity gemini-3.8-flash (also stills/images) as workers; claude-opus-5-5 as reviewer/fixer and plan author — recorded in .paul/config.md
- Checkpoint resolved: product name "Penrose", binary `penrose`, app-id `io.github.peter.Penrose` (user decision, supersedes auto-proposed "Pressmark"); trademark/domain checks pending before public release

### Deferred Issues
- Pending before any public release: EUIPO/USPTO trademark + domain checks for "Penrose" (carried from name checkpoint).
- CI: dispatch runs SUCCESS for 02-03 through 04-03 (latest: 39721575324 on 55f40a4ec6 — check it plus 39721558880 next session; push-trigger duplicates get cancelled — always dispatch). CI does not run ctest — local ctest remains the real gate.
- Manual UI UATs owed (Xvfb absent; live GNOME not intruded): 02-03, 03-01, 03-02, 03-03, 04-01, 04-02, 04-03, 04-04-UAT.md.
- FIXED 2026-10-22: share/ui/style.css used :root/var() (newer-GTK CSS) — dozens of parser errors under host GTK 4.14 dropped the ruler/palette styling (big "doesn't look like Corel" factor). Values inlined (alpha/shade/@theme_* are supported); zero parser errors on relaunch; style.css allow-listed. NOTE for future upgrades: keep the inlined values in sync when restyling (comment in file). Stale Penrose-x86_64.AppImage (pre-rename build, no Corel menu) deleted — it was the likely cause of "menu doesn't look like Corel" reports; use build/appimage/Rosepen-x86_64.AppImage.
- Run CLI tests SERIALLY (`ctest` without -j races the session D-Bus app id io.github.toel1234.Rosepen); low-sev panel items (re-click no re-centre, full-list rebuild perf, ComboBoxText deprecated).
- From 03-02/03-03 gates: cairo/print/pixbuf masters page-1-only; export-dialog vector per-page path FIXED in 03-03; oversized-master-vs-bleed design decision before bleed work; spot ink named "All" collides with the registration-color cache entry; optional content + link annotations skipped on PDF/X; unrasterized filters dropped silently (default-path parity).
- From 04-04 gate: 04-02 preferences tooltip "CorelDRAW-style" violates the no-Corel-strings rule (rename in a follow-up plan); commands-bar tooltip still "Layers and Objects" (upstream, left); vertical palette pinned-well ~2 tiles wide (cosmetic, UAT 6); existing profiles switch to rosepen.xml keys + top command bar on first start after this change.
- TraceDraw (github.com/igtoth/tracedraw, MIT/Apache-2.0, clean-room Corel clone in Rust): used as the 04-04 reference with attribution; Phase 6 option — its `cdr` crate (reads AND writes .cdr) can be FFI-bridged (cdylib + cbindgen) as an alternative to libcdr. References in .paul/phases/04-corel-gui/references/ (local).
- Follow-ups from 02-04: sp-mesh-array.cpp locale imbue + callers; icc fallback siblings lighting-color (specular/diffuse) + dropshadow flood-color; CI ctest decision (all cli_* corpora local-only).
- `cli_tests` user-data-directory test expects profile dir `inkscape` (known failure, not weakened); poppler GPGME/NSS3 modules for the first real Flatpak build; real Flatpak build (flatpak-builder absent locally); AppImage full-stack bundling/signing; `share/inkscape` + `INKSCAPE_*` renames; msgid translations; `1.5-dev` version scheme.

### Blockers/Concerns
- Repo reconciliation RESOLVED 2026-10-21: origin = `toel1234/rosepen`, history + main live there (02-03 push `7673b78eb2` + CI dispatch succeeded); archived public `toel1234/penrose` keeps earlier history; rebrand commit 91f9c73e in history, sweep ALL_BRANDING_CHECKS_OK.
- None open. Sudo deps install completed by user (14 packages). Build race workaround and all deviations recorded in 01-01-SUMMARY.md.
- Pending before any public release: EUIPO/USPTO trademark + domain checks for "Penrose".

## Session Continuity

Last session: 2026-10-23 (05-03 Artistic Media applied, gated, summarized; MCP server delivered)
Stopped at: 05-03 committed; UATs 05-01/05-02/05-03 OWED
Next action: Author 05-04-PLAN.md (Smart Fill) via opus, then 05-05 (LiveSketch), 06-01/06-02 (.cdr); Phase 5 polish plan should absorb the PowerClip container-move clone-compensation fix (HIGH, structural)
Resume file: .paul/ROADMAP.md (Phase 5)

---
*STATE.md — Updated after every significant action*
