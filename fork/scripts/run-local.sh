#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the locally installed fork with an isolated user profile, so the dev
# build never reads or migrates ~/.config/inkscape.
# Run from the repository root.

set -euo pipefail

export INKSCAPE_PROFILE_DIR="$PWD/.profile"
exec "$PWD/install/bin/rosepen" "$@"
