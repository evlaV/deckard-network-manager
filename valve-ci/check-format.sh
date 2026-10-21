#!/bin/bash
# Check that the C changes we carry on top of upstream follow .clang-format.
#
# Only lines changed since the upstream release tag are checked, so the
# result doesn't depend on our clang-format version matching upstream's.
# Paths nm-code-format.sh excludes (vendored code) are skipped here too.
#
# Usage: check-format.sh [base]   (default: nearest upstream release tag)

set -euo pipefail

base="${1:-$(git describe --tags --abbrev=0 --match '[0-9]*.[0-9]*.[0-9]*' --exclude '*-*' HEAD)}"
echo "Checking C changes since ${base} with $(clang-format --version)"

if ! git clang-format --diff "$base" HEAD -- '*.c' '*.h' \
        ':!src/c-list' ':!src/c-rbtree' ':!src/c-siphash' ':!src/c-stdaux' \
        ':!src/libnm-std-aux/unaligned-fundamental.h' ':!src/libnm-std-aux/unaligned.h' \
        ':!src/libnm-systemd-core/src' ':!src/libnm-systemd-shared/src' \
        ':!src/linux-headers' ':!src/n-acd' ':!src/n-dhcp4'; then
    echo
    echo "Formatting differs. Fix with: git clang-format ${base}"
    exit 1
fi
