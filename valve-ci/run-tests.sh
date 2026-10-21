#!/bin/bash
# Build this tree with tests enabled and run `meson test`.
#
# holo's package build always passes --nocheck and configures with
# -Dtests=no, so the unit tests need their own build. Dependencies come from
# holo's recipe so this stays in step with what the package build installs.
#
# Usage: run-tests.sh <holo recipe dir> [build dir]

set -euo pipefail

recipe_dir="$1"
build_dir="${2:-build-tests}"

# Every depends/makedepends/checkdepends the recipe declares, including the
# split packages' runtime deps, with version constraints stripped. A split
# package that clears its deps shows up as an empty `depends = ` line; skip it.
mapfile -t deps < <(cd "$recipe_dir" && makepkg --printsrcinfo \
    | awk '$1 ~ /^(make|check)?depends(_aarch64)?$/ && $3 != "" {sub(/[<>=].*/, "", $3); print $3}' \
    | sort -u)
sudo pacman -S --noconfirm --needed --asdeps "${deps[@]}"

# Same feature set as the package (see build() in holo's recipe), minus docs
# and LTO to save time, plus the tests
meson setup "$build_dir" \
    -D tests=yes \
    -D docs=false \
    -D b_lto=false \
    -D session_tracking_consolekit=false \
    -D suspend_resume=systemd \
    -D modify_system=true \
    -D selinux=false \
    -D iwd=true \
    -D teamdctl=true \
    -D config_plugins_default=keyfile \
    -D ifupdown=false \
    -D netconfig=no \
    -D config_dns_rc_manager_default=symlink \
    -D vapi=true \
    -D qt=false
meson compile -C "$build_dir"

# check-local-gitlab-ci compares .gitlab-ci.yml against upstream's template,
# which we replaced with our own pipeline
export NMTST_SKIP_CHECK_GITLAB_CI=1
export PAGER=cat
meson test -C "$build_dir" --print-errorlogs
