<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Rosepen branding assets

All artwork in this directory is the Rosepen project's own branding, licensed
**GPL-2.0-or-later** (same as the project).

## Identity

| Item | Value |
|------|-------|
| Product | Rosepen |
| Binaries | `rosepen`, `rosepen-view` |
| App-ids | `io.github.toel1234.Rosepen`, `io.github.toel1234.RosepenView` |
| Config and cache dir | `rosepen` |
| Gettext domain | `rosepen` |

These values come from `fork/cmake/ForkBranding.cmake`, which
`config.h.cmake` exports into `config.h`; desktop, metainfo, icon and
screenshot sources here only *use* them. The CMake variables are cache
entries, so after changing one run `cmake -S . -B build -U 'FORK_*'` before
building. See `fork/README.md` for the full description.

Keep-list: the `Inkscape::` namespace, `org.inkscape.<type>.*` extension IDs,
the `share/inkscape` data dir, `INKSCAPE_*` environment variables, and the
license and attribution stay as they are — the rebrand only changes
user-visible and system-integration identity.

## Provenance

- Generated **2026-10-21** by **gemini-3.8-flash** (antigravity/agy CLI) from a
  written design brief supplied by the 01-02 plan: Rosepen tribar motif in flat
  vector, palette deep indigo `#2B2D6E` / teal `#1FA59A` / amber `#F2A541`
  (light) on `#1C1D2B` (dark), Rosepen P3 rhombus tiling backgrounds.
- Reviewed by claude-opus-5-5 (trademark-similarity and 16px-legibility check on
  a rendered montage) before acceptance.
- The PNG rasters are **not** hand-authored: they are rendered deterministically
  from `icons/application/scalable/io.github.toel1234.Rosepen.svg` and the
  `screens/*.svg` sources by our own binary (see commands below).
- `screens/src/` holds the SVG sources for assets that ship only as rasters or
  gzipped SVG (`about/*.svgz`, `start-*.png`).

## Layout

| Path | Purpose |
|------|---------|
| `logo/rosepen-logo.svg`, `rosepen-logo-dark.svg`, `rosepen-logo-text.svg` | Logo marks (256², dark 256², wordmark 1024×256) |
| `icons/application/scalable/…Rosepen.svg` | App icon source (128²) |
| `icons/application/<N>x<N>/…Rosepen.png` | Rendered rasters, N ∈ {16,22,24,32,48,96,128,256} |
| `icons/application/symbolic/…Rosepen-symbolic.svg` | 16² symbolic icon (single `#2e3436` fill, GTK-recolorable) |
| `screens/about.svg`, `screens/about/*.svgz` | About dialog artwork (750×625) |
| `screens/start-*.png`, `screens/start-welcome-text.svg` | Startup dialog artwork (dimensions match upstream `share/screens/*`) |
| `io.github.toel1234.Rosepen*.desktop.template`, `…metainfo.xml.in` | Desktop/appstream metadata sources |
| `bash-completion/rosepen` | Shell completion for the `rosepen` binary |

## Regeneration

```sh
# Icon rasters (N in 16 22 24 32 48 96 128 256):
./install/bin/rosepen --export-type=png --export-width=N --export-height=N \
  --export-filename=fork/branding/icons/application/NxN/io.github.toel1234.Rosepen.png \
  fork/branding/icons/application/scalable/io.github.toel1234.Rosepen.svg

# Startup rasters (dimensions must match upstream share/screens/*.png):
./install/bin/rosepen --export-type=png --export-width=W --export-height=H \
  --export-filename=fork/branding/screens/<name>.png fork/branding/screens/src/<name>.svg

# About screens ship gzipped:
gzip -9n -c fork/branding/screens/src/<name>.svg > fork/branding/screens/about/<name>.svgz
```

## Rules for new assets

- SVG only, no `<text>` (convert lettering to `<path>`), no `<image>`, no
  external references, no fonts.
- Every file starts with the XML declaration and the
  `SPDX-License-Identifier: GPL-2.0-or-later` comment.
- Must not resemble the Inkscape logo, the CorelDRAW balloon or any Corel mark.
