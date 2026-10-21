#!/bin/bash
# Point holo's networkmanager recipe at a commit of this repository.
#
# The holo recipe may still fetch upstream NetworkManager and patch it in
# prepare(); those patches are commits here, so both the source list and
# prepare() are replaced. Works whether the recipe sources upstream or us.
#
# build() expects the checkout in NetworkManager/, hence the rename.
#
# Usage: use-this-commit.sh <PKGBUILD> <commit>

set -euo pipefail

pkgbuild=$1
commit=$2
repo=https://gitlab.steamos.cloud/deckard/deckardos/network-manager.git

tmp=$(mktemp)
awk -v src="NetworkManager::git+${repo}#commit=${commit}" '
    # Arrays end on the first line closing the parenthesis, which may be the
    # line that opens them.
    skip == "array" { if ($0 ~ /\)[[:space:]]*$/) skip = ""; next }
    skip == "func"  { if ($0 ~ /^}/) skip = ""; next }
    /^source=\(/ {
        print "source=(\"" src "\")"
        if ($0 !~ /\)[[:space:]]*$/) skip = "array"
        next
    }
    /^(b2|sha256|sha512|md5)sums=\(/ {
        print "b2sums=(\x27SKIP\x27)"
        if ($0 !~ /\)[[:space:]]*$/) skip = "array"
        next
    }
    /^prepare\(\)/ { print "prepare() {\n  :\n}"; skip = "func"; next }
    { print }
' "$pkgbuild" > "$tmp"

grep -qxF "source=(\"NetworkManager::git+${repo}#commit=${commit}\")" "$tmp" \
    || { echo "ERROR: no source=() array in $pkgbuild" >&2; exit 1; }
# Write through the file, not over it: in the holo repo, the recipe
# directory deckard/networkmanager may be a symlink to holo/networkmanager.
cat "$tmp" > "$(realpath "$pkgbuild")"
rm -f "$tmp"
