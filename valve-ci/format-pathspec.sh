# Pathspec for the C sources we hold to .clang-format, shared by
# check-format.sh and hooks/pre-commit. Paths nm-code-format.sh excludes
# (vendored code) are skipped here too.
# shellcheck shell=bash disable=SC2034
format_pathspec=(
    -- '*.c' '*.h'
    ':!src/c-list' ':!src/c-rbtree' ':!src/c-siphash' ':!src/c-stdaux'
    ':!src/libnm-std-aux/unaligned-fundamental.h' ':!src/libnm-std-aux/unaligned.h'
    ':!src/libnm-systemd-core/src' ':!src/libnm-systemd-shared/src'
    ':!src/linux-headers' ':!src/n-acd' ':!src/n-dhcp4'
)
