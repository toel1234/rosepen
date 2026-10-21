<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Packaging

Packaging support for Rosepen. The AppImage path, the Flatpak manifest and the
CI workflow each have their own section here.

## AppImage

`build-appimage.sh` turns a finished `./install` tree into a self-contained
type-2 AppImage:

```sh
fork/scripts/build-local.sh          # produces ./install
fork/packaging/build-appimage.sh     # produces build/appimage/Rosepen-x86_64.AppImage
```

### Why a manual AppDir

The obvious alternative, linuxdeploy, walks `ldd` and copies a large,
host-dependent set of system libraries, steered by an excludelist we would
have to audit; it also rewrites RPATHs in ways that are hard to verify. This
fork has exactly two non-system libraries — gtkmm 4.14
(`build/deps/lib`) and cairo 1.18.6 (`build/deps-cairo/lib`) — plus
`lib2geom` and `libinkscape_base`, so an explicit copy list is deterministic
and reviewable. On top of the explicit list the script runs `ldd` over every
ELF in the AppDir until a pass finds nothing new, so anything else that still
resolves into `build/` or `install/` is bundled on evidence, not assumption.
All bundled ELF files are stripped (`--strip-unneeded`) and get `$ORIGIN`-relative
RUNPATHs via patchelf; the script asserts that no RUNPATH contains an absolute
path. Neither `install/` nor `build/deps*` is modified — only the AppDir copies.

### What is bundled vs. taken from the host

Bundled: `install/bin` + `install/share` in full, `lib2geom`,
`libinkscape_base`, gtkmm 4.14, cairo 1.18.6 (`libcairo.so*`,
`libcairo-gobject.so*`; the cairo script-interpreter copies that ship inside
`install/lib/inkscape` come along). Development leftovers (`pkgconfig`,
`cmake`) are removed from the image.

Taken from the host: glibc, libstdc++, GTK 4, glibmm/cairomm/pangomm/sigc++3,
poppler, libwpg/libvisio/libcdr/librevenge, potrace, gsl, ImageMagick — i.e.
everything installable as an Ubuntu 24.04 system package. Nothing from the
host is copied into the image.

`AppRun` sets `INKSCAPE_DATADIR=<image>/usr/share` and
`INKSCAPE_LOCALEDIR=<image>/usr/share/locale`. Per `src/path-prefix.cpp`,
`INKSCAPE_DATADIR` is the directory *containing* the `inkscape/` resource
directory (i.e. `<prefix>/share`), not `<prefix>/share/inkscape` — the same
semantics as the compile-time default in `CMakeScripts/ConfigPaths.cmake`. It
also prepends `<image>/usr/share` to `XDG_DATA_DIRS` and deliberately sets no
`LD_LIBRARY_PATH`: the `$ORIGIN` RUNPATHs suffice, and `LD_LIBRARY_PATH`
would leak into extension subprocesses (python).

### Target hosts

Ubuntu 24.04-class systems providing the system GTK 4 ≥ 4.14 stack. Older
distros are unsupported by this skeleton (full-stack bundling is deferred).

### Running

With FUSE available the AppImage runs directly. Without it, use the
extraction runtime:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 Rosepen-x86_64.AppImage
```

### Tool pinning

The script pins the exact release binaries it builds with (TOFU: resolved
once at execution time; the script verifies the hashes with `sha256sum -c`
on every run and refuses to continue on mismatch). Downloads land in the
dedicated `build/appimage/tools/` directory:

- appimagetool 1.9.1 (`appimagetool-x86_64.AppImage`),
  sha256 `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0`
- patchelf 0.19.2 (static `patchelf-0.19.2-x86_64.tar.gz`),
  sha256 `2abdc34fbe949c995a1baa4ec310896a085588a45a99cd0d726851bdfac2a4bb`
- type-2 runtime 20251108 (`runtime-x86_64`, passed to appimagetool via
  `--runtime-file`),
  sha256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`

Without the runtime pin, appimagetool downloads an unpinned `continuous`
runtime at build time; passing the pinned file keeps every image input
hash-verified.

The Flatpak manifest under `flatpak/` is an unbuilt skeleton — see the
Flatpak section below.

### Deferred

Full GTK-stack bundling (older-distro support), zsync update information,
image signing, and a release workflow are out of scope for this skeleton.

## Flatpak

The manifest is `flatpak/io.github.toel1234.Rosepen.yml`. It is a **skeleton**:
it has not been built, and the first real build is a later plan. The
development machine has no `flatpak-builder`, so the manifest is validated
statically only:

```sh
python3 -I fork/packaging/lint-packaging.py
```

- App-id `io.github.toel1234.Rosepen`, command `rosepen`.
- Runtime `org.gnome.Platform` 51, SDK `org.gnome.Sdk` 51. The version was
  resolved from the highest `gnome-NN` release branch in
  `git ls-remote --heads https://gitlab.gnome.org/GNOME/gnome-build-meta`.
- The module list is adapted from Flathub's `org.inkscape.Inkscape` manifest,
  moved from the GTK3 stack to GTK4: cairo, libsigc++-3, glibmm-2.68,
  cairomm-1.16, pangomm-2.48, gtkmm-4.0, boost (filesystem + stacktrace),
  double-conversion, gsl, bdwgc (with libatomic_ops), potrace, librevenge,
  libwpd, libwpg, libvisio, libcdr, poppler-data, poppler, then Rosepen.
- Providing gtkmm and cairo in `/app` makes the Rosepen CMake build skip its
  `ExternalProject` downloads, which cannot work without network access.
- Every archive source carries a real `sha256` computed from the downloaded
  tarball. There are no placeholder hashes.
- `-DWITH_IMAGE_MAGICK=OFF` keeps the skeleton small; ImageMagick is deferred.
- The Rosepen module uses `type: dir` at the repository root. Release builds
  will switch to `type: git` against `https://github.com/toel1234/rosepen`.

## CI

The workflow is `.github/workflows/build.yml` ("Build"). It runs one
`ubuntu-24.04` job with a 150-minute timeout on pushes to `main` and on
`workflow_dispatch`.

Steps: free disk space, checkout with recursive submodules, fetch the
`fork-base` tag (needed by the branding sweep), restore the ccache, install the
build dependencies, build with `fork/scripts/build-local.sh`, run
`fork/scripts/verify-branding.sh`, build the AppImage with
`fork/packaging/build-appimage.sh`, and upload
`build/appimage/Rosepen-x86_64.AppImage` as the artifact
`Rosepen-x86_64-<sha>` for 14 days.

The workflow uses read-only repository permissions, cancels superseded runs on
the same ref, and has no matrix, release job, scheduled run or secrets.
