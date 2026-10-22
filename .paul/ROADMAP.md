---
description: "corel — milestone and phase structure"
type: Roadmap
about: "corel"
---

# Roadmap: corel

## Overview
GPL fork of Inkscape extended with CorelDRAW-style GUI and functionality. Linux only, GTK4 kept. Name: **Rosepen** (renamed from Penrose 2026-10-21 — trademark collision; repo renamed with redirect).

## Current Milestone
**v0.1 Corel parity v1** (v0.1.0)
Status: In progress
Phases: 4 of 6 complete

## Phases

| Phase | Name | Plans | Status | Completed |
|-------|------|-------|--------|-----------|
| 1 | Fork, build, rebrand | 3 | Complete | 2026-10-21 |
| 2 | CMYK/ICC color | 4 | Complete | 2026-10-21 |
| 3 | Multi-page and PDF/X | 3 | Complete | 2026-10-22 |
| 4 | Corel-style GUI | 4 | Complete | 2026-10-22 |
| 5 | Corel tools | 5 | In progress | 05-01, 05-02: 2026-10-22 |
| 6 | .cdr import | 2 | Not started | - |

## Phase Details

### Phase 1: Fork, build, rebrand — COMPLETE
**Goal:** Pinned Inkscape fork builds and runs on Ubuntu under a new name, with packaging and CI.
**Result:** 01-01 pinned master `3d6e954a` (GTK4), full build + smoke; 01-02 rebrand (interim Penrose → final Rosepen, app-id `io.github.toel1234.Rosepen`); 01-03 50M AppImage + Flatpak skeleton + GitHub Actions. Published: https://github.com/toel1234/penrose (renaming to `/rosepen`); CI green (run 39529586443).

### Phase 2: CMYK/ICC color
**Goal:** Documents can be CMYK end to end with ICC profiles and spot colors.
**Depends on:** Phase 1
**Research:** DONE (scout map + opus anchors 2026-10-21: Colors:: system has CMS profiles, document spaces, proofing transforms, device-cmyk; gaps = first-class CMYK UX, soft-proof consumers, spot colors)
**Plans:**
- [x] 02-01: Document CMYK core: output profile in DocumentCMS, convert-to-output-profile action, ICC/CMYK pages in Fill&Stroke, new-doc default pref, gtests (research folded in — upstream Colors:: settles the storage model)
- [x] 02-02: Soft-proofing and gamut: wire softproof prefs into display transform, unstub canvas CMS, gamut/over-ink indicators, icc opacity policy (policy: opacity stays out of icc-color, travels in *-opacity)
- [x] 02-03: Spot/named colors and swatches (pushed 7673b78eb2; manual UI UAT owed — see 02-03-UAT.md)
- [x] 02-04: Round-trip corpus and hardening (pushed 4d91aaf7c2; corpus locks svg/plain/pdf/png; icc export abort + feFlood icc + mesh/alpha locale defects fixed)

### Phase 3: Multi-page and PDF/X
**Goal:** Multi-page documents with master layer and print-ready PDF/X export.
**Depends on:** Phase 2 (CMYK output)
**Research:** Likely (Inkscape 1.2+ page support; PDF/X via cairo or own writer)
**Plans:**
- [x] 03-01: Page manager panel, page ops, per-page size (pushed be7323192c; Pages docker + 9 app.page-* actions + cli_page-ops corpus; UAT owed)
- [x] 03-02: Master layer shown on all pages (pushed b3f63124e9; canvas/PNG/capypdf render + page-op integration + docker section; UAT owed)
- [x] 03-03: PDF/X-1a and PDF/X-4 export with CMYK, bleed, crop marks (pushed 9091735e23; OutputIntent + boxes + marks + preflight; UAT owed)

**Phase 3 COMPLETE 2026-10-22.** Master-layer limits recorded for PDF/X polish: cairo/print/pixbuf masters page-1-only; oversized-master-vs-bleed design decision outstanding.

### Phase 4: Corel-style GUI
**Goal:** Workspace layout, property bar, dockers and shortcut scheme in the style of CorelDRAW, using original assets.
**Depends on:** Phase 1 (parallel with 2 and 3)
**Research:** Unlikely
**Plans:**
- [x] 04-01: Toolbox and flyout groups layout (pushed 0311aa1319; flyouts by default, classic grid one pref away; UAT owed)
- [x] 04-02: Context-sensitive property bar (pushed 4de2caee0b; selection-driven object sections, pref-gated; UAT owed)
- [x] 04-03: Docker panel system, Object Manager, Object Properties (pushed 55f40a4ec6; page filter + master rows + kind-keyed panels + Corel docker defaults; UAT owed)
- [x] 04-04: Corel-style shortcut scheme and menus (pushed e1adb56ed7; rosepen.xml keys, Corel menubar, vertical right palette, workspace presets; UAT owed)

**Phase 4 COMPLETE 2026-10-22.** Reference: TraceDraw (MIT/Apache-2.0, clean-room Corel clone) — attribution in 04-04-SUMMARY. Follow-ups: 04-02 tooltip "CorelDRAW-style" rename; commands-bar tooltip still "Layers and Objects" (upstream); palette pinned-well width cosmetic.

### Phase 5: Corel tools
**Goal:** PowerClip, LiveSketch, Artistic Media, dimension tools, smart fill.
**Depends on:** Phases 2 and 4
**Research:** Likely (behavior must be re-implemented from documented user behavior)
**Plans:**
- [x] 05-01: PowerClip (place inside frame, edit contents in place, extract, normalize repair; Gtk-free model + 5 actions + 22-case gtest + 8-scenario corpus; opus gate PASS-WITH-FIXES — extract extra-children fix, container-move clone-compensation HIGH deferred to polish plan; UAT owed)
- [x] 05-02: Dimension tools (live dimension LPE + pure geometry model 33+14 gtests, app.dimension-add/list/set, Dimension tool as flyout face, 9-scenario corpus incl. live-move/freeze; opus gate PASS-WITH-FIXES — 7 fixes; UAT owed)
- [ ] 05-03: Artistic Media brushes
- [ ] 05-04: Smart Fill
- [ ] 05-05: LiveSketch

### Phase 6: .cdr import
**Goal:** Open real .cdr files, edit in CMYK, export PDF/X (the v1 success test).
**Depends on:** Phases 2 and 3
**Research:** Likely (libcdr coverage, color mapping)
**Plans:**
- [ ] 06-01: libcdr integration and color mapping
- [ ] 06-02: Corpus of real .cdr files, fixes, end-to-end acceptance test

---
*Roadmap created: 2026-10-20*
*Last updated: 2026-10-21*
