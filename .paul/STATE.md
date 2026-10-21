---
description: "corel — current position and accumulated context"
type: ProjectState
about: "corel"
---

# Project State

## Project Reference

See: .paul/PROJECT.md (updated 2026-10-20)

**Core value:** CorelDRAW-class open source vector/print app on Linux, built on Inkscape.
**Current focus:** Phase 1 planning complete — PLAN 01-01 awaiting approval

## Current Position

Milestone: v0.1 Corel parity v1
Phase: 1 of 6 (Fork, build, rebrand) — Planning
Plan: 01-01 created, awaiting approval
Status: PLAN created, ready for APPLY
Last activity: 2026-10-21 — Created phases/01-fork-build-rebrand/01-01-PLAN.md

Progress:
- Milestone: [█░░░░░░░░░] 17%

## Loop Position

```
PLAN ──▶ APPLY ──▶ UNIFY
  ✓        ✓        ✓     [Phase 1 complete — next: Phase 2 PLAN]
```

## Accumulated Context

### Decisions
- Fork Inkscape, keep GTK4; Linux only
- Worker pipeline: GLM-5.3 subagents + command-code deepseek-v4.1-flash + antigravity gemini-3.8-flash (also stills/images) as workers; claude-opus-5-5 as reviewer/fixer and plan author — recorded in .paul/config.md
- Checkpoint resolved: product name "Penrose", binary `penrose`, app-id `io.github.peter.Penrose` (user decision, supersedes auto-proposed "Pressmark"); trademark/domain checks pending before public release

### Deferred Issues
- Pending before any public release: EUIPO/USPTO trademark + domain checks for "Penrose" (carried from name checkpoint).
- CI green confirmation: run https://github.com/toel1234/penrose/actions/runs/39529303947 was in_progress at transition; check result next session.
- `cli_tests` user-data-directory test expects profile dir `inkscape` (known failure, not weakened); poppler GPGME/NSS3 modules for the first real Flatpak build; real Flatpak build (flatpak-builder absent locally); AppImage full-stack bundling/signing; `share/inkscape` + `INKSCAPE_*` renames; msgid translations; `1.5-dev` version scheme.

### Blockers/Concerns
- None open. Sudo deps install completed by user (14 packages). Build race workaround and all deviations recorded in 01-01-SUMMARY.md.
- Pending before any public release: EUIPO/USPTO trademark + domain checks for "Penrose".

## Session Continuity

Last session: 2026-10-21 (Phase 1 complete, published)
Stopped at: Phase 1 transition done; repo live at github.com/toel1234/penrose
Next action: Review plan, run /paul:apply .paul/phases/01-fork-build-rebrand/01-01-PLAN.md
Resume file: .paul/phases/01-fork-build-rebrand/01-01-PLAN.md

---
*STATE.md — Updated after every significant action*
