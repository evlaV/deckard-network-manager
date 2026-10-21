#!/bin/bash
# Carry downloaded build dependencies between CI jobs.
#
# GitLab can only cache paths inside the project directory, while pacman
# downloads into /var/cache/pacman/pkg. `restore` seeds pacman's cache from
# the saved copy so unchanged packages aren't downloaded again; `save` writes
# back just the package versions that are installed now, so versions the
# repositories have since replaced drop out instead of piling up.
#
# Usage: pacman-cache.sh restore|save <saved cache dir>

set -euo pipefail

action="$1"
saved_dir="$2"
pacman_dir=/var/cache/pacman/pkg

case "$action" in
restore)
    mkdir -p "$saved_dir"
    shopt -s nullglob
    saved=( "$saved_dir"/*.pkg.tar* )
    echo "Restoring ${#saved[@]} cached packages"
    if (( ${#saved[@]} )); then
        sudo mkdir -p "$pacman_dir"
        sudo cp --update=none -t "$pacman_dir" "${saved[@]}"
    fi
    ;;
save)
    rm -rf "$saved_dir"
    mkdir -p "$saved_dir"
    shopt -s nullglob
    while read -r name version; do
        # <name>-<version>-<arch>.pkg.tar.zst, plus any .sig
        files=( "$pacman_dir/$name-$version"-*.pkg.tar* )
        if (( ${#files[@]} )); then
            cp -t "$saved_dir" "${files[@]}"
        fi
    done < <(pacman -Q)
    echo "Saved $(find "$saved_dir" -name '*.pkg.tar*' ! -name '*.sig' | wc -l) packages" \
        "($(du -sh "$saved_dir" | cut -f1))"
    ;;
*)
    echo "Usage: $0 restore|save <saved cache dir>" >&2
    exit 1
    ;;
esac
