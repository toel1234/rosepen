#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Re-run the whole Rosepen branding acceptance sweep against a local install.
#
# Run from the repository root after a clean reinstall:
#
#   rm -rf ./install
#   cmake -S . -B build -U 'FORK_*'
#   fork/scripts/build-local.sh
#   fork/scripts/verify-branding.sh
#
# Prints one line per check, then ALL_BRANDING_CHECKS_OK and exits 0 when
# everything passes; exits 1 otherwise.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

INSTALL="$PWD/install"
BIN="$INSTALL/bin"
fail=0

pass() { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
note() { printf 'skip  %s\n' "$1"; }

# ---------------------------------------------------------------------------
# Allow-list: every path this rebrand may touch.
# ---------------------------------------------------------------------------
ALLOW=(
    "CMakeLists.txt"
    "config.h.cmake"
    "man/CMakeLists.txt"
    "share/CMakeLists.txt"
    "share/branding/CMakeLists.txt"
    "share/icons/application/CMakeLists.txt"
    "share/screens/CMakeLists.txt"
    "share/ui/dialog-crash.glade"
    "share/ui/inkscape-about.glade"
    "share/ui/inkscape-splash.glade"
    "share/ui/inkscape-welcome.glade"
    "share/ui/menus.ui"
    "share/ui/statusbar.ui"
    "src/CMakeLists.txt"
    "src/inkscape.cpp"
    "src/extension/extension.cpp"
    "src/inkscape-application.cpp"
    "src/inkscape-main.cpp"
    "src/inkscape-version-info.cpp"
    "src/inkview-application.cpp"
    "src/io/recent-files.cpp"
    "src/io/resource.cpp"
    "src/path-prefix.cpp"
    "src/ui/dialog/command-palette.cpp"
    "src/ui/widget/desktop-widget.cpp"
    "src/colors/document-cms.h"
    "src/colors/document-cms.cpp"
    "src/colors/cms/system.h"
    "src/colors/cms/system.cpp"
    "src/colors/cms/transform.h"
    "src/colors/cms/transform-surface.h"
    "src/colors/spaces/cms.cpp"
    "src/colors/utils.h"
    "src/colors/utils.cpp"
    "src/colors/spot.h"
    "src/colors/spot.cpp"
    "src/colors/spot-swatch.h"
    "src/colors/spot-swatch.cpp"
    "src/colors/CMakeLists.txt"
    "src/colors/CMakeUnit.txt"
    "src/extension/internal/pdfoutput/build-document.h"
    "src/extension/internal/pdfoutput/build-patterns.cpp"
    "src/extension/internal/pdfoutput/build-styles.cpp"
    "src/object/filters/flood.cpp"
    "src/ui/dialog/color-item.h"
    "src/ui/dialog/color-item.cpp"
    "src/ui/dialog/object-attributes.cpp"
    "src/ui/gridview-utils.h"
    "src/ui/gridview-utils.cpp"
    "src/ui/widget/paint-attribute.cpp"
    "src/ui/widget/swatch-editor.h"
    "src/ui/widget/swatch-editor.cpp"
    "share/ui/swatch-editor.ui"
    "src/actions/actions-canvas-mode.cpp"
    "src/ui/widget/canvas.h"
    "src/ui/widget/canvas.cpp"
    "src/ui/widget/color-notebook.h"
    "src/ui/widget/color-notebook.cpp"
    "src/ui/dialog/document-properties.h"
    "src/ui/dialog/document-properties.cpp"
    "src/actions/actions-object.cpp"
    "src/preferences-skeleton.h"
    "testfiles/*"
    "fork/UPSTREAM_BASE"
    "fork/README.md"
    "fork/cmake/ForkBranding.cmake"
    "fork/scripts/*"
    "fork/man/*"
    "fork/branding/*"
    "fork/packaging/*"
    ".github/*"
    ".paul/*"
)

# ---------------------------------------------------------------------------
# AC-1: binaries, version, gettext, profile
# ---------------------------------------------------------------------------
if grep -q 'FORK_PRODUCT_NAME:STRING=Rosepen' build/CMakeCache.txt &&
   grep -q 'FORK_APP_ID:STRING=io.github.toel1234.Rosepen' build/CMakeCache.txt; then
    pass "CMake cache holds the Rosepen FORK_* values"
else
    bad "CMake cache holds the Rosepen FORK_* values"
fi

if [ -n "$(find build -maxdepth 3 -name config.h -exec grep -l 'GETTEXT_PACKAGE "rosepen"' {} +)" ] &&
   [ -n "$(find build -maxdepth 3 -name config.h -exec grep -l 'FORK_APP_ID "io.github.toel1234.Rosepen"' {} +)" ]; then
    pass "config.h has GETTEXT_PACKAGE and FORK_APP_ID"
else
    bad "config.h has GETTEXT_PACKAGE and FORK_APP_ID"
fi

if [ -x "$BIN/rosepen" ] && [ -x "$BIN/rosepen-view" ] &&
   [ ! -e "$BIN/inkscape" ] && [ ! -e "$BIN/inkview" ]; then
    pass "install/bin has rosepen and rosepen-view only"
else
    bad "install/bin has rosepen and rosepen-view only"
fi

if "$BIN/rosepen" --version | grep -cE '^Rosepen [0-9]' >/dev/null; then
    pass "rosepen --version prints the Rosepen product name"
else
    bad "rosepen --version prints the Rosepen product name"
fi

ldd_out="$(ldd "$BIN/rosepen" 2>&1 || true)"
if ! printf '%s\n' "$ldd_out" | grep -q 'not found'; then
    pass "ldd on rosepen reports no missing libraries"
else
    bad "ldd on rosepen reports no missing libraries"
fi

if [ -n "$(find "$INSTALL" -name rosepen.mo)" ] && [ -z "$(find "$INSTALL" -name inkscape.mo)" ]; then
    pass "rosepen.mo installed, no inkscape.mo"
else
    bad "rosepen.mo installed, no inkscape.mo"
fi

if locale -a 2>/dev/null | grep -qiE '^de_DE\.utf-?8$'; then
    if [ "$(LC_ALL=C "$BIN/rosepen" --help 2>&1 | md5sum)" != "$(LC_ALL=de_DE.UTF-8 "$BIN/rosepen" --help 2>&1 | md5sum)" ]; then
        pass "gettext catalog binds at runtime (de_DE)"
    else
        bad "gettext catalog binds at runtime (de_DE)"
    fi
else
    note "gettext runtime check (no de_DE.UTF-8 locale)"
fi

# ---------------------------------------------------------------------------
# AC-2: desktop integration, app-id, isolated profile
# ---------------------------------------------------------------------------
if desktop-file-validate "$INSTALL/share/applications/io.github.toel1234.Rosepen.desktop" &&
   desktop-file-validate "$INSTALL/share/applications/io.github.toel1234.RosepenView.desktop"; then
    pass "desktop-file-validate passes for both desktop files"
else
    bad "desktop-file-validate passes for both desktop files"
fi

if appstreamcli validate --no-net "$INSTALL/share/metainfo/io.github.toel1234.Rosepen.metainfo.xml"; then
    pass "appstreamcli validate passes for the metainfo file"
else
    bad "appstreamcli validate passes for the metainfo file"
fi

if [ -z "$(find "$INSTALL/share" -name 'org.inkscape.Ink*' 2>/dev/null)" ]; then
    pass "no org.inkscape.Ink* files installed under share/"
else
    bad "no org.inkscape.Ink* files installed under share/"
fi

if [ -z "$(git grep -nE 'org\.inkscape\.Ink(scape|view)' -- src 2>/dev/null || true)" ]; then
    pass "no org.inkscape.Ink(scape|view) app-id literals left in src/"
else
    bad "no org.inkscape.Ink(scape|view) app-id literals left in src/"
fi

if [ -z "$(git diff fork-base -U0 -- src share | grep -E '^-.*org\.inkscape\.[a-z]' || true)" ]; then
    pass "no org.inkscape.<type> extension ID line was removed"
else
    bad "no org.inkscape.<type> extension ID line was removed"
fi

if git grep -q 'INKSCAPE_COMMAND' -- src/inkscape-main.cpp; then
    pass "INKSCAPE_COMMAND is exported for inkex"
else
    bad "INKSCAPE_COMMAND is exported for inkex"
fi

tmp="$(mktemp -d)"
mkdir -p "$tmp/config" "$tmp/cache" "$tmp/work"
printf '<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect width="100" height="100" fill="red"/></svg>\n' > "$tmp/work/in.svg"
if env -u INKSCAPE_PROFILE_DIR XDG_CONFIG_HOME="$tmp/config" XDG_CACHE_HOME="$tmp/cache" \
       "$BIN/rosepen" --export-type=png --export-filename="$tmp/work/out.png" "$tmp/work/in.svg" >/dev/null 2>&1 &&
   [ -f "$tmp/work/out.png" ] &&
   [ -d "$tmp/config/rosepen" ] &&
   [ ! -e "$tmp/config/inkscape" ] && [ ! -e "$tmp/cache/inkscape" ]; then
    pass "headless run uses the rosepen profile dir only"
else
    bad "headless run uses the rosepen profile dir only"
fi
rm -rf "$tmp"

# ---------------------------------------------------------------------------
# AC-3: assets and user-visible strings
# ---------------------------------------------------------------------------
assets=0
about_png="$(mktemp -d)/about.png"
while IFS= read -r f; do
    [ -n "$f" ] || continue
    if ! grep -q 'SPDX-License-Identifier: GPL-2.0-or-later' "$f"; then
        bad "SPDX header missing: $f"; assets=1; continue
    fi
    if ! python3 -I -c 'import sys,xml.etree.ElementTree as E;E.parse(sys.argv[1])' "$f" 2>/dev/null; then
        bad "invalid XML: $f"; assets=1; continue
    fi
    if grep -qE '<image|xlink:href="(http|file)|<text' "$f"; then
        bad "forbidden element in: $f"; assets=1
    fi
    # AC-3: every SVG must render through our own binary
    if ! "$BIN/rosepen" --export-type=png --export-filename="$about_png" "$f" >/dev/null 2>&1; then
        bad "render failed: $f"; assets=1
    fi
done < <(find fork/branding -name '*.svg')
if [ "$assets" -eq 0 ]; then
    pass "all fork SVGs are SPDX-tagged, valid, self-contained and renderable"
fi

while IFS= read -r z; do
    [ -n "$z" ] || continue
    if ! zcat "$z" | grep -c 'SPDX-License-Identifier: GPL-2.0-or-later' >/dev/null; then
        bad "SPDX header missing: $z"; assets=1
    fi
    if ! "$BIN/rosepen" --export-type=png --export-filename="$about_png" "$z" >/dev/null 2>&1; then
        bad "render failed: $z"; assets=1
    fi
done < <(find fork/branding/screens/about -name '*.svgz')
if [ "$assets" -eq 0 ]; then
    pass "all fork SVGs are SPDX-tagged and the about screens render"
fi

if ! grep -rlqiE 'inkscape|corel' fork/branding/icons fork/branding/logo; then
    pass "no Inkscape/Corel names in icons or logo"
else
    bad "no Inkscape/Corel names in icons or logo"
fi

icons=0
for N in 16 22 24 32 48 96 128 256; do
    p="$INSTALL/share/icons/hicolor/${N}x${N}/apps/io.github.toel1234.Rosepen.png"
    if [ ! -f "$p" ] ||
       [ "$(python3 -I -c "import struct,sys;d=open(sys.argv[1],'rb').read(24);print(*struct.unpack('>II',d[16:24]))" "$p")" != "$N $N" ]; then
        bad "icon missing or wrong size: ${N}x${N}"; icons=1
    fi
done
if [ "$icons" -eq 0 ]; then
    pass "hicolor PNGs installed at 16/22/24/32/48/96/128/256"
fi

if [ -f "$INSTALL/share/icons/hicolor/scalable/apps/io.github.toel1234.Rosepen.svg" ] &&
   [ -f "$INSTALL/share/icons/hicolor/symbolic/apps/io.github.toel1234.Rosepen-symbolic.svg" ]; then
    pass "scalable and symbolic icons installed"
else
    bad "scalable and symbolic icons installed"
fi

if [ "$(ls "$INSTALL/share/inkscape/screens/about" 2>/dev/null)" = "$(ls fork/branding/screens/about | grep '\.svgz$')" ]; then
    pass "installed about screens are exactly the fork set"
else
    bad "installed about screens are exactly the fork set"
fi

screens=0
for n in start-splash start-welcome start-support start-support-money start-support-time; do
    a="$(python3 -I -c "import struct,sys;d=open(sys.argv[1],'rb').read(24);print(*struct.unpack('>II',d[16:24]))" "share/screens/$n.png")"
    b="$(python3 -I -c "import struct,sys;d=open(sys.argv[1],'rb').read(24);print(*struct.unpack('>II',d[16:24]))" "$INSTALL/share/inkscape/screens/$n.png")"
    if [ "$a" != "$b" ]; then
        bad "screen dimensions differ from upstream: $n ($a vs $b)"; screens=1
    fi
done
if [ "$screens" -eq 0 ]; then
    pass "start-* PNGs match the upstream dimensions"
fi

ui_out="$(grep -nE 'Inkscape' share/ui/menus.ui share/ui/inkscape-about.glade share/ui/inkscape-welcome.glade \
    share/ui/inkscape-splash.glade share/ui/dialog-crash.glade share/ui/statusbar.ui |
    grep 'translatable="yes"' |
    grep -vE 'Inkscape SVG|[Bb]ased on Inkscape|Inkscape Developers|inkscape\.org|gitlab\.com/inkscape' || true)"
if [ -z "$ui_out" ]; then
    pass "no rebrandable translatable strings still say Inkscape"
else
    bad "no rebrandable translatable strings still say Inkscape"
    printf '%s\n' "$ui_out"
fi

if ! grep -q '" - Inkscape"' src/ui/widget/desktop-widget.cpp &&
   grep -q '" - " FORK_PRODUCT_NAME' src/ui/widget/desktop-widget.cpp; then
    pass "window title suffix is the Rosepen product name"
else
    bad "window title suffix is the Rosepen product name"
fi

# ---------------------------------------------------------------------------
# AC-4: man pages, placeholders, boundaries, scope
# ---------------------------------------------------------------------------
man_ok=1
for m in rosepen rosepen-view; do
    [ -f "$INSTALL/share/man/man1/$m.1.gz" ] || { bad "missing man page: $m.1.gz"; man_ok=0; }
done
for m in inkscape inkview; do
    [ ! -e "$INSTALL/share/man/man1/$m.1.gz" ] || { bad "stale man page installed: $m.1.gz"; man_ok=0; }
done
if [ "$man_ok" -eq 1 ]; then
    pass "rosepen.1.gz and rosepen-view.1.gz installed, no upstream man pages"
fi

if zcat "$INSTALL/share/man/man1/rosepen.1.gz" | grep -c 'fork of Inkscape' >/dev/null; then
    pass "man page carries the fork attribution"
else
    bad "man page carries the fork attribution"
fi

if [ -f "$INSTALL/share/bash-completion/completions/rosepen" ] &&
   grep -q 'complete -F _rosepen' "$INSTALL/share/bash-completion/completions/rosepen"; then
    pass "rosepen bash completion installed"
else
    bad "rosepen bash completion installed"
fi

if [ -z "$(git grep -niE 'forknametbd' -- . ':!.paul' ':!fork/scripts/verify-branding.sh' 2>/dev/null || true)" ] &&
   [ -z "$(grep -rniE 'forknametbd' fork/ --exclude='verify-branding.sh' 2>/dev/null || true)" ]; then
    pass "no ForkNameTBD placeholders remain"
else
    bad "no ForkNameTBD placeholders remain"
fi

if [ -z "$(git grep -niE 'penrose' -- . ':!.paul' ':!fork/scripts/verify-branding.sh' 2>/dev/null || true)" ]; then
    pass "no old Penrose name remains anywhere"
else
    bad "no old Penrose name remains anywhere"
    git grep -niE 'penrose' -- . ':!.paul' ':!fork/scripts/verify-branding.sh' 2>/dev/null || true
fi

old_appid="$(git grep -nE 'io[.]github[.]peter' -- . ':!.paul' 2>/dev/null || true)"
old_appid+="$(grep -rnE 'io[.]github[.]peter' fork/ .github/ --exclude=verify-branding.sh 2>/dev/null || true)"
old_files="$(find fork install -name '*io[.]github[.]peter*' 2>/dev/null || true)"
if [ -z "$old_appid" ] && [ -z "$old_files" ]; then
    pass "no old io.github.p* app-id or file name remains"
else
    bad "old io.github.p* app-id or file name remains:"
    printf '%s\n' "$old_appid" "$old_files"
fi

cache_home="$(sed -n 's/^FORK_HOMEPAGE_URL:STRING=//p' build/CMakeCache.txt | head -1)"
cache_bug="$(sed -n 's/^FORK_BUGTRACKER_URL:STRING=//p' build/CMakeCache.txt | head -1)"
if [ -n "$cache_home" ] && [ -n "$cache_bug" ] &&
   grep -qF "$cache_home" "$INSTALL/share/metainfo/io.github.toel1234.Rosepen.metainfo.xml" &&
   grep -qF "$cache_bug" "$INSTALL/share/metainfo/io.github.toel1234.Rosepen.metainfo.xml" &&
   grep -qF "$cache_bug" share/ui/dialog-crash.glade; then
    pass "public homepage and bug tracker URLs match the CMake cache"
else
    bad "public homepage and bug tracker URLs match the CMake cache"
fi

if ! grep -q 'inkscape\.org/report' share/ui/dialog-crash.glade src/inkscape.cpp; then
    pass "crash dialog and stderr no longer point at the upstream tracker"
else
    bad "crash dialog and stderr no longer point at the upstream tracker"
fi

if [ -z "$(git diff fork-base -- COPYING LICENSES AUTHORS TRANSLATORS)" ]; then
    pass "COPYING, LICENSES, AUTHORS and TRANSLATORS are unchanged"
else
    bad "COPYING, LICENSES, AUTHORS and TRANSLATORS are unchanged"
fi

# Modified files only: new fork files (e.g. src/colors/spot*.cpp) legitimately
# live in the upstream Inkscape:: namespace.
if [ -z "$(git diff fork-base -U0 --diff-filter=M -- src | grep -E '^[-+].*namespace Inkscape' || true)" ]; then
    pass "no diff line adds or removes 'namespace Inkscape'"
else
    bad "no diff line adds or removes 'namespace Inkscape'"
fi

if [ -z "$(git submodule status | grep '^+' || true)" ]; then
    pass "no submodule is at a different commit than recorded"
else
    bad "no submodule is at a different commit than recorded"
fi

sm=0
for sub in po share/extensions share/themes src/3rdparty/2geom; do
    if [ -n "$(git -C "$sub" status --porcelain)" ]; then
        bad "submodule has local changes: $sub"; sm=1
    fi
done
if [ "$sm" -eq 0 ]; then
    pass "po, share/extensions, share/themes and 2geom are clean"
fi

scope=0
while IFS= read -r p; do
    [ -n "$p" ] || continue
    matched=0
    for pat in "${ALLOW[@]}"; do
        if [[ "$p" == $pat ]]; then matched=1; break; fi
    done
    if [ "$matched" -eq 0 ]; then
        bad "out-of-scope path: $p"; scope=1
    fi
done < <(git diff --name-only fork-base; git ls-files --others --exclude-standard fork/ .github/)
if [ "$scope" -eq 0 ]; then
    pass "every changed or new path is inside the allow-list"
fi

spdx_missing="$(grep -rL 'SPDX-License-Identifier: GPL-2.0-or-later' fork/ \
    --include='*.sh' --include='*.cmake' --include='*.md' --include='*.svg' \
    --include='*.in' --include='*.template' --include='rosepen' \
    --include='*.yml' --include='*.py' --include='AppRun' || true)"
if [ -z "$spdx_missing" ]; then
    pass "every fork script, cmake, doc, SVG, template, yml and py carries SPDX"
else
    bad "every fork script, cmake, doc, SVG, template, yml and py carries SPDX"
    printf '%s\n' "$spdx_missing"
fi

if [ -f .github/workflows/build.yml ]; then
    if head -3 .github/workflows/build.yml | grep -q 'SPDX-License-Identifier: GPL-2.0-or-later'; then
        pass "GitHub Actions workflow carries SPDX"
    else
        bad "GitHub Actions workflow carries SPDX"
    fi
else
    note "GitHub Actions workflow SPDX (workflow not created yet)"
fi

if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    rc=0
    timeout 20 ./fork/scripts/run-local.sh >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 124 ]; then
        pass "GUI stays alive for 20s"
    else
        bad "GUI stays alive for 20s (exit $rc)"
    fi
else
    note "GUI liveness check (no DISPLAY/WAYLAND_DISPLAY)"
fi

# ---------------------------------------------------------------------------
if [ "$fail" -ne 0 ]; then
    printf 'BRANDING_CHECKS_FAILED\n'
    exit 1
fi

echo ALL_BRANDING_CHECKS_OK
