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
Phases: 1 of 6 complete

## Phases

| Phase | Name | Plans | Status | Completed |
|-------|------|-------|--------|-----------|
| 1 | Fork, build, rebrand | 3 | Complete | 2026-10-21 |
| 2 | CMYK/ICC color | 4 | In progress | - |
| 3 | Multi-page and PDF/X | 3 | Not started | - |
| 4 | Corel-style GUI | 4 | Not started | - |
| 5 | Corel tools | 5 | Not started | - |
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
- [~] 02-01: Document CMYK core: output profile in DocumentCMS, convert-to-output-profile action, ICC/CMYK pages in Fill&Stroke, new-doc default pref, gtests (research folded in — upstream Colors:: settles the storage model)
- [ ] 02-02: Soft-proofing and gamut: wire softproof prefs into display transform, unstub canvas CMS, gamut/over-ink indicators, icc opacity policy
- [ ] 02-03: Spot/named colors and swatches
- [ ] 02-04: Round-trip corpus and hardening

### Phase 3: Multi-page and PDF/X
**Goal:** Multi-page documents with master layer and print-ready PDF/X export.
**Depends on:** Phase 2 (CMYK output)
**Research:** Likely (Inkscape 1.2+ page support; PDF/X via cairo or own writer)
**Plans:**
- [ ] 03-01: Page manager panel, page ops, per-page size
- [ ] 03-02: Master layer shown on all pages
- [ ] 03-03: PDF/X-1a and PDF/X-4 export with CMYK, bleed, crop marks

### Phase 4: Corel-style GUI
**Goal:** Workspace layout, property bar, dockers and shortcut scheme in the style of CorelDRAW, using original assets.
**Depends on:** Phase 1 (parallel with 2 and 3)
**Research:** Unlikely
**Plans:**
- [ ] 04-01: Toolbox and flyout groups layout
- [ ] 04-02: Context-sensitive property bar
- [ ] 04-03: Docker panel system, Object Manager, Object Properties
- [ ] 04-04: Corel-style shortcut scheme and menus

### Phase 5: Corel tools
**Goal:** PowerClip, LiveSketch, Artistic Media, dimension tools, smart fill.
**Depends on:** Phases 2 and 4
**Research:** Likely (behavior must be re-implemented from documented user behavior)
**Plans:**
- [ ] 05-01: PowerClip (clip contents editing)
- [ ] 05-02: Dimension tools
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
