#!/bin/bash
# Check that the C changes we carry on top of upstream follow .clang-format.
#
# Only lines changed since the upstream release tag are checked, so the
# result doesn't depend on our clang-format version matching upstream's.
#
# Usage: check-format.sh [base]   (default: nearest upstream release tag)

set -euo pipefail

# shellcheck source=valve-ci/format-pathspec.sh
source "$(dirname "$0")/format-pathspec.sh"

base="${1:-$(git describe --tags --abbrev=0 --match '[0-9]*.[0-9]*.[0-9]*' --exclude '*-*' HEAD)}"
echo "Checking C changes since ${base} with $(clang-format --version)"

if ! git clang-format --diff "$base" HEAD "${format_pathspec[@]}"; then
    echo
    echo "Formatting differs. Fix with: git clang-format ${base}"
    exit 1
fi
