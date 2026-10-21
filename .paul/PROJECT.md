---
description: "Designers get a CorelDRAW-class vector and print-design app on Linux that is open source, built on Inkscape."
type: Project
about: "corel"
---

# corel (working name: Rosepen)

## What This Is

A GPL fork of Inkscape (https://gitlab.com/inkscape/inkscape), extended with CorelDRAW-style GUI and functionality: CMYK/ICC color, multi-page layout, PowerClip, Corel-style UI and shortcuts, and .cdr import.

## Core Value

Designers get a CorelDRAW-class vector and print-design app on Linux that is open source, built on Inkscape.

## Current State

| Attribute | Value |
|-----------|-------|
| Type | Application |
| Version | 0.0.0 |
| Status | Phase 1 complete |
| Last Updated | 2026-10-21 |

## Requirements

### Core Features

- Color: CMYK / ICC / spot color, PDF/X export
- Pages: multi-page documents with master layer
- Corel-style GUI: workspace layout, property bar, dockers, shortcut scheme
- Corel tools: PowerClip, LiveSketch, Artistic Media, dimension tools, smart fill
- Files: .cdr import (libcdr); export best-effort only (proprietary format)

### Validated (Shipped)
None yet.

### Active (In Progress)
None yet.

### Planned (Next)
- Build and rebrand Inkscape fork
- CMYK/ICC color pipeline
- Multi-page + PDF/X
- Corel-style UI and shortcuts
- Corel-specific tools
- .cdr import

### Out of Scope
- Windows and macOS builds
- Photo-Paint and other bundled Corel apps (to confirm)
- Corel name, logo, icons, assets (trademark/copyright)
- Final app name and trademark clearance (working name Rosepen; rename before public release)

## Constraints

### Technical Constraints
- Base: Inkscape (C++, GTK4, CMake); keep toolkit, no Qt rewrite
- Target: Linux only; package as Flatpak/AppImage
- Libraries: lcms2 (ICC/CMYK), libcdr (.cdr import)

### Business Constraints
- Must stay GPL-2.0+ (Inkscape license)
- Clone layout, workflow, features only; no Corel branding or assets; fork needs its own name
- Inkscape's own name/logo/icons are trademarked — the fork must ship under its own branding (rebrand is Phase 1, plan 01-02)

## Key Decisions

| Decision | Rationale | Date | Status |
|----------|-----------|------|--------|
| Fork Inkscape, keep GTK4 | Qt rewrite is near-total UI rewrite | 2026-10-20 | Active |
| Linux only | User does not need Windows/macOS | 2026-10-20 | Active |
| Working name Rosepen, rename later | Penrose trademark crowded; Rosepen no hits found, not legally cleared | 2026-10-20 | Active |
| Product name "Rosepen" (binary `rosepen`; app-id `io.github.toel1234.Rosepen`, viewer `io.github.toel1234.RosepenView`) — RENAMED from "Penrose" 2026-10-21: name collision, user decision | User's choice; tiling motif fits vector/geometry; trademark/domain checks still pending before public release | 2026-10-21 | Active |
| Hosting: GitHub `toel1234/penrose` (public) + GitHub Actions CI; app-id namespace `io.github.toel1234` | User provided hosting and gh credentials 2026-10-21 | 2026-10-21 | Active |

## Success Metrics

| Metric | Target | Current | Status |
|--------|--------|---------|--------|
| v1 end-to-end | Open real .cdr, edit in CMYK, export print-ready PDF/X | - | Not started |
| Build | Builds and runs on Ubuntu (local + CI) | CI run 39529586443 success (deps→build→sweep→AppImage) | Achieved |

## Tech Stack / Tools

| Layer | Technology | Notes |
|-------|------------|-------|
| Language | C++ | Inkscape codebase |
| UI | GTK4 | Inherited |
| Build | CMake | Inherited |
| Color | lcms2 | ICC/CMYK |
| Import | libcdr | .cdr |
| Packaging | AppImage (CI-built, Ubuntu 24.04-class hosts) / Flatpak (manifest skeleton, unbuilt) | Linux |

## Links

| Resource | URL |
|----------|-----|
| Upstream | https://gitlab.com/inkscape/inkscape |
| Repository | https://github.com/toel1234/penrose |
| Issues | https://github.com/toel1234/penrose/issues |
| CI | https://github.com/toel1234/penrose/actions |

---
*PROJECT.md — Updated when requirements or context change*
*Last updated: 2026-10-20*
