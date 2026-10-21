<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# fork/

This repository is a fork of [Inkscape](https://gitlab.com/inkscape/inkscape),
licensed under **GPL-2.0-or-later**. All credit for the upstream code,
history and artwork belongs to the Inkscape project and its contributors.

## fork/ directory convention

All fork-only files live under `fork/` (scripts, CMake branding
(ForkBranding.cmake), branding assets, man pages, notes). Upstream files are
touched minimally: single-expression macro substitutions via `config.h` and
redirected install globs. The authoritative list of touched upstream files is
the allow-list in `fork/scripts/verify-branding.sh`. Keeping fork additions in
one place keeps future rebases onto upstream cheap and GPL provenance clear.

## Rebase procedure

To move onto a new upstream base:

```
git fetch upstream
git rebase --onto <new-base> fork-base main
```

After rebasing, move the `fork-base` tag to `<new-base>` and update
`fork/UPSTREAM_BASE` (sha, ref, gtk_major, fetched date).

## Pinned base

- Base: `master@2026-10-20` (commit recorded in `fork/UPSTREAM_BASE`), tag `fork-base`.
- Base choice followed the deterministic rule from the phase plan: the newest
  stable tag (`INKSCAPE_1_4_4`) still requires gtkmm-3.0, while `upstream/master`
  requires gtkmm-4.0; per the PROJECT.md GTK4 decision the current master was
  chosen to avoid porting later UI work from GTK3 to GTK4.

## Identity

| Item | Value |
|------|-------|
| Product | Rosepen |
| Editor binary | `rosepen` |
| Viewer product / binary | Rosepen Viewer / `rosepen-view` |
| Editor app-id | `io.github.toel1234.Rosepen` |
| Viewer app-id | `io.github.toel1234.RosepenView` |
| User config and cache dir | `rosepen` (`$XDG_CONFIG_HOME/rosepen`, `$XDG_CACHE_HOME/rosepen`) |
| Gettext domain | `rosepen` (`rosepen.mo`) |
| Homepage | `https://github.com/toel1234/rosepen` |
| Bug tracker | `https://github.com/toel1234/rosepen/issues` |

## Hosting

| Item | Value |
|------|-------|
| Repository | `https://github.com/toel1234/rosepen` |
| Issues | `https://github.com/toel1234/rosepen/issues` |
| CI | GitHub Actions, `.github/workflows/build.yml` |
| App-id namespace | `io.github.toel1234` (owned via the GitHub account `toel1234`) |

### Single source of truth

`fork/cmake/ForkBranding.cmake` defines `FORK_PRODUCT_NAME`,
`FORK_BINARY_NAME`, `FORK_APP_ID`, `FORK_CONFIG_DIR_NAME`,
`FORK_GETTEXT_DOMAIN`, `FORK_HOMEPAGE_URL`, `FORK_BUGTRACKER_URL` and the
viewer equivalents. The top-level `CMakeLists.txt` includes it right after
`project(rosepen)`, and `config.h.cmake` turns the values into `config.h`
macros (`FORK_PRODUCT_NAME`, `FORK_BINARY_NAME`, `FORK_APP_ID`,
`FORK_VIEWER_APP_ID`, `FORK_CONFIG_DIR_NAME`, `FORK_HOMEPAGE_URL`,
`FORK_BUGTRACKER_URL`, and `GETTEXT_PACKAGE` = `FORK_GETTEXT_DOMAIN`). C++
code and install rules read the macros; nothing should hardcode the strings.

The variables are CMake cache entries, so **changing a value requires
dropping the cache** before reconfiguring:

```sh
cmake -S . -B build -U 'FORK_*'
fork/scripts/build-local.sh
```

Without `-U 'FORK_*'` the old value stays in `build/CMakeCache.txt` and the
new name never reaches `config.h`. `project()` must stay equal to
`FORK_GETTEXT_DOMAIN`, because `po/` installs `${CMAKE_PROJECT_NAME}.mo`; the
guard in `ForkBranding.cmake` enforces this.

### Keep-list (not renamed)

The rebrand is deliberately shallow. These keep the upstream names:

- the `Inkscape::` C++ namespace, class and file names, and the CMake target
  names `inkscape`, `inkview` and `inkscape_base`;
- `org.inkscape.<type>.*` extension IDs (`.inx` files in the
  `share/extensions` submodule) and the `inkscape:` SVG XML namespace;
- the `share/inkscape` system data directory and `lib/inkscape` install dir;
- `INKSCAPE_*` environment variables (`INKSCAPE_PROFILE_DIR`,
  `INKSCAPE_DATADIR`, `INKSCAPE_LOCALEDIR`, `INKSCAPE_COMMAND`);
- the license (`GPL-2.0-or-later`), `COPYING`, `LICENSES/`, `AUTHORS`,
  `TRANSLATORS`, and the Inkscape attribution in the about dialog and man
  pages ("based on Inkscape", inkscape.org links).

Data-dir and environment-variable renames are deferred; keep them until a
later plan handles the migration.

## Verification

`fork/scripts/verify-branding.sh` re-runs the whole acceptance sweep after a
clean reinstall (`rm -rf ./install`, `cmake -S . -B build -U 'FORK_*'`,
`fork/scripts/build-local.sh`). It ends with `ALL_BRANDING_CHECKS_OK`.
