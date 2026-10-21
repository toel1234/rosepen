#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# build-appimage.sh — turn ./install into a self-contained Rosepen AppImage.
#
# Run from the repository root:
#   fork/packaging/build-appimage.sh
#
# Inputs (read-only, never modified in place):
#   ./install                install tree from the normal build
#   ./build/deps/lib         gtkmm 4.14 ExternalProject
#   ./build/deps-cairo/lib   cairo 1.18.6 ExternalProject
#   ./fork/packaging/appimage/AppRun
# Output:
#   build/appimage/Rosepen-x86_64.AppImage (plus the kept build/appimage/AppDir)
#
# Approach: manual AppDir assembly + patchelf ($ORIGIN RUNPATHs) +
# appimagetool type-2. Rationale: fork/packaging/README.md ("Why a manual
# AppDir"). Scope: skeleton for Ubuntu 24.04-class hosts — everything that is
# NOT a system package is bundled; glibc/libstdc++/GTK 4 & co. come from the
# host.

set -euo pipefail

REPO_ROOT=$PWD
INSTALL=$REPO_ROOT/install
APPDIR=$REPO_ROOT/build/appimage/AppDir
TOOLS=$REPO_ROOT/build/appimage/tools

# --- Pinned build tools (TOFU: URLs + hashes resolved 2026-10-21; the script
# verifies the hashes on every run and refuses to proceed on mismatch) ---
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
PATCHELF_VERSION=0.19.2
PATCHELF_URL="https://github.com/NixOS/patchelf/releases/download/${PATCHELF_VERSION}/patchelf-${PATCHELF_VERSION}-x86_64.tar.gz"
PATCHELF_SHA256=2abdc34fbe949c995a1baa4ec310896a085588a45a99cd0d726851bdfac2a4bb
# appimagetool otherwise downloads an unpinned 'continuous' runtime at build
# time; pinning the type-2 runtime keeps image inputs fully hash-verified.
RUNTIME_VERSION=20251108
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/${RUNTIME_VERSION}/runtime-x86_64"
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

log() { printf '== %s\n' "$*"; }
die() { printf 'build-appimage: ERROR: %s\n' "$*" >&2; exit 1; }

# --- 1. Preconditions -------------------------------------------------------
[ -f "$INSTALL/bin/rosepen" ] || die "install/bin/rosepen missing — run fork/scripts/build-local.sh first"
[ -f "$REPO_ROOT/build/CMakeCache.txt" ] || die "build/CMakeCache.txt missing — run from the repository root"
FORK_APP_ID=$(sed -n 's/^FORK_APP_ID:STRING=//p' "$REPO_ROOT/build/CMakeCache.txt" | head -n1)
[ -n "$FORK_APP_ID" ] || die "FORK_APP_ID not found in build/CMakeCache.txt"
FORK_PRODUCT=$(sed -n 's/^FORK_PRODUCT_NAME:STRING=//p' "$REPO_ROOT/build/CMakeCache.txt" | head -n1)
[ -n "$FORK_PRODUCT" ] || die "FORK_PRODUCT_NAME not found in build/CMakeCache.txt"
OUT=$REPO_ROOT/build/appimage/${FORK_PRODUCT}-x86_64.AppImage
log "FORK_APP_ID=$FORK_APP_ID"
log "FORK_PRODUCT_NAME=$FORK_PRODUCT"

# --- 2. Pinned tools: download if absent, verify hashes unconditionally -----
mkdir -p "$TOOLS"
fetch_tool() { # url sha256 filename
    local url=$1 sha=$2 fname=$3
    if [ ! -f "$TOOLS/$fname" ]; then
        log "downloading $fname"
        curl -fsSL --retry 3 -o "$TOOLS/$fname.part" "$url"
        mv "$TOOLS/$fname.part" "$TOOLS/$fname"
    fi
    echo "$sha  $fname" | (cd "$TOOLS" && sha256sum -c -) \
        || die "sha256 mismatch for $fname (pinned $sha)"
}
fetch_tool "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256" "appimagetool-x86_64.AppImage"
fetch_tool "$PATCHELF_URL" "$PATCHELF_SHA256" "patchelf-${PATCHELF_VERSION}-x86_64.tar.gz"
fetch_tool "$RUNTIME_URL" "$RUNTIME_SHA256" "runtime-x86_64"

chmod +x "$TOOLS/appimagetool-x86_64.AppImage"
if [ ! -x "$TOOLS/bin/patchelf" ]; then
    tar -xzf "$TOOLS/patchelf-${PATCHELF_VERSION}-x86_64.tar.gz" -C "$TOOLS"
fi
PATCHELF=$TOOLS/bin/patchelf
"$PATCHELF" --version >/dev/null || die "downloaded patchelf does not run"

# --- 3. AppDir assembly -----------------------------------------------------
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr"

cp -a "$INSTALL/bin" "$INSTALL/share" "$APPDIR/usr/"
mkdir -p "$APPDIR/usr/lib"
cp -a "$INSTALL/lib/inkscape" "$INSTALL/lib"/lib2geom.so* "$APPDIR/usr/lib/"
cp -a "$REPO_ROOT/build/deps/lib"/libgtkmm-4.0.so* "$APPDIR/usr/lib/"
cp -a "$REPO_ROOT/build/deps-cairo/lib"/libcairo.so* \
      "$REPO_ROOT/build/deps-cairo/lib"/libcairo-gobject.so* "$APPDIR/usr/lib/"

# no development leftovers in the image (lib/inkscape/pkgconfig rides along above)
rm -rf "$APPDIR/usr/lib/pkgconfig" "$APPDIR/usr/lib/inkscape/pkgconfig" \
       "$APPDIR/usr/lib/cmake" "$APPDIR/usr/lib/include"

# --- 4. Discovery loop ------------------------------------------------------
# The AppDir copies still carry the install tree's RUNPATHs here, so ldd shows
# exactly which libraries still resolve into build/ or install/. Copy them in
# (symlink chains intact) and loop until a pass finds nothing new, so the
# bundle list follows the evidence instead of assumptions.
# ELF detection must not depend on the local `file` output column layout:
# match "ELF" anywhere and take the path before the first colon.
elf_files() {
    find "$APPDIR" -type f -exec file {} + | grep 'ELF' | cut -d: -f1
}
require_elves() { # $1.. = array name
    local -n arr=$1
    [ "${#arr[@]}" -gt 0 ] || die "no ELF files found in $APPDIR — file(1)/grep detection broken"
}

bundle_one() { # resolved source path
    local src=$1 real
    cp -a "$src" "$APPDIR/usr/lib/"
    real=$(readlink -f "$src")
    if [ "$real" != "$src" ] && [ ! -e "$APPDIR/usr/lib/$(basename "$real")" ]; then
        cp -a "$real" "$APPDIR/usr/lib/"   # keep the SONAME symlink chain resolvable
    fi
    printf '   bundled %s (from %s)\n' "$(basename "$src")" "$src"
}

log "discovering libraries that resolve into build/ or install/"
for pass in 1 2 3 4 5 6; do
    found=0
    mapfile -t elves < <(elf_files)
    for elf in "${elves[@]}"; do
        while IFS= read -r lib; do
            case $lib in
                "$APPDIR"/*) continue ;;                            # already bundled
                "$REPO_ROOT/build/"*|"$REPO_ROOT/install/"*) ;;
                *) continue ;;                                      # host system library
            esac
            [ -e "$APPDIR/usr/lib/$(basename "$lib")" ] && continue
            bundle_one "$lib"
            found=1
        done < <(ldd "$elf" 2>/dev/null | awk '$2 == "=>" {print $3}' | sort -u)
    done
    [ "$found" -eq 0 ] && break
    [ "$pass" -eq 6 ] && die "library discovery did not stabilize"
done

# --- 5. Strip and set $ORIGIN RUNPATHs (AppDir copies only) -----------------
rpath_for() { # file under $APPDIR -> $ORIGIN-based RUNPATH reaching usr/lib
    local rel=${1#"$APPDIR"/} dir n up=''
    dir=${rel%/*}
    case $dir in
        usr/bin)          printf '$ORIGIN/../lib:$ORIGIN/../lib/inkscape' ;;
        usr/lib)          printf '$ORIGIN:$ORIGIN/inkscape' ;;
        usr/lib/inkscape) printf '$ORIGIN:$ORIGIN/..' ;;
        *)
            n=$(awk -F/ '{print NF}' <<<"$dir")   # hops from $dir up to usr/
            while (( n-- > 1 )); do up+='../'; done
            printf '$ORIGIN/%slib:$ORIGIN/%slib/inkscape' "$up" "$up"
            ;;
    esac
}

log "stripping and setting \$ORIGIN RUNPATHs"
mapfile -t elves < <(elf_files)
require_elves elves
for elf in "${elves[@]}"; do
    strip --strip-unneeded "$elf"
    "$PATCHELF" --set-rpath "$(rpath_for "$elf")" "$elf"
done

log "asserting \$ORIGIN-only RUNPATHs and complete resolution"
for elf in "${elves[@]}"; do
    while IFS= read -r line; do
        rp=${line#*\[}; rp=${rp%]*}
        if [ -n "$rp" ] && printf '%s' "$rp" | grep -qE '(^|:)/'; then
            die "absolute RUNPATH left in $elf: $rp"
        fi
    done < <(readelf -d "$elf" 2>/dev/null | grep -E 'R(UN)?PATH')
    if missing=$(ldd "$elf" 2>/dev/null | grep 'not found'); then
        die "unresolved libraries for $elf:
$missing"
    fi
done

# --- 6. Root files ----------------------------------------------------------
log "installing AppDir root files"
cp -a "$APPDIR/usr/share/applications/$FORK_APP_ID.desktop" "$APPDIR/"
cp -a "$APPDIR/usr/share/icons/hicolor/256x256/apps/$FORK_APP_ID.png" "$APPDIR/"
ln -sfn "$FORK_APP_ID.png" "$APPDIR/.DirIcon"
install -m 0755 "$REPO_ROOT/fork/packaging/appimage/AppRun" "$APPDIR/AppRun"

# --- 7. Build the type-2 AppImage -------------------------------------------
log "building the AppImage"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
ARCH=x86_64 \
SOURCE_DATE_EPOCH=$(git -C "$REPO_ROOT" log -1 --format=%ct) \
APPIMAGE_EXTRACT_AND_RUN=1 \
    "$TOOLS/appimagetool-x86_64.AppImage" --runtime-file "$TOOLS/runtime-x86_64" "$APPDIR" "$OUT"

# --- 8. Smoke test in a clean environment -----------------------------------
log "smoke test: --version in an empty environment"
SMOKE=$(mktemp -d)
trap 'rm -rf "$SMOKE"' EXIT
env -i HOME="$SMOKE" PATH=/usr/bin:/bin APPIMAGE_EXTRACT_AND_RUN=1 "$OUT" --version

log "done: $OUT ($(du -h "$OUT" | cut -f1))"
