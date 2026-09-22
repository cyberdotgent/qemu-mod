#!/bin/bash
# Assemble a self-contained Windows distribution tree from a mingw cross build.
#
# The build tree's qemu-bundle/ directory is a farm of symlinks into the source
# and build trees, which is fine for running the emulators in place on the build
# host but does not survive being read from Windows (over \\wsl.localhost\, from
# a network share, or after copying the tree elsewhere).  This script produces a
# tree of real files instead, laid out the way get_relocated_path() expects:
#
#     win64-dist/qemu-system-x86_64.exe
#     win64-dist/share/bios-256k.bin
#     win64-dist/share/keymaps/...
#
# so the emulators find their firmware with no -L argument.  Any non-system DLLs
# the binaries import are resolved and copied in beside them.
#
# Usage: scripts/win64-dist.sh [-b BUILDDIR] [-o OUTDIR] [--no-strip] [--zip]
#
# This is a downstream (qemu-mod) helper; it is not part of upstream QEMU.

set -euo pipefail

die() { printf '%s: %s\n' "${0##*/}" "$*" >&2; exit 1; }
info() { printf '\033[1m==>\033[0m %s\n' "$*"; }

src_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

build_dir=
out_dir=
cross_prefix=x86_64-w64-mingw32-
strip=--strip
make_zip=false

while [ $# -gt 0 ]; do
    case $1 in
    -b|--build-dir)   build_dir=$2; shift 2 ;;
    -o|--out-dir)     out_dir=$2; shift 2 ;;
    --cross-prefix)   cross_prefix=$2; shift 2 ;;
    --no-strip)       strip=; shift ;;
    --zip)            make_zip=true; shift ;;
    -h|--help)        sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0 ;;
    *)                die "unknown argument '$1' (try --help)" ;;
    esac
done

: "${build_dir:=$src_dir/build-win64}"
[ -d "$build_dir" ] || die "build directory '$build_dir' does not exist"
build_dir=$(cd -- "$build_dir" && pwd)
: "${out_dir:=$build_dir/win64-dist}"

# The output lives inside the build tree by default so that it is reachable
# over the same UNC path as the build itself.

config_host=$build_dir/config-host.h
[ -f "$config_host" ] || die "'$build_dir' is not a configured QEMU build tree"
grep -q '^#define CONFIG_WIN32' "$config_host" ||
    die "'$build_dir' is not a Windows (mingw) build tree"

objdump=${cross_prefix}objdump
command -v "$objdump" >/dev/null || die "'$objdump' not found in PATH"

# meson lives in the build tree's own venv; its version must match the one that
# generated the build directory, so never fall back to a different meson silently.
meson=$build_dir/pyvenv/bin/meson
[ -x "$meson" ] || die "'$meson' not found -- was this tree configured by QEMU's configure?"

# CONFIG_PREFIX tells us where inside the DESTDIR the install tree will land.
prefix=$(sed -n 's/^#define CONFIG_PREFIX "\(.*\)"$/\1/p' "$config_host")
[ -n "$prefix" ] || die "could not read CONFIG_PREFIX from $config_host"

staging=$build_dir/.win64-dist-staging
trap 'rm -rf -- "$staging"' EXIT

info "Installing into a staging tree"
rm -rf -- "$staging"
# --no-rebuild: the default 'install' ninja target depends on 'all', which drags
# in test binaries that do not link under mingw.  Build what you need first.
"$meson" install -C "$build_dir" --destdir "$staging" --no-rebuild ${strip:+"$strip"} \
    >"$staging.log" 2>&1 ||
    { sed -n '$p' "$staging.log" >&2
      die "meson install failed (full log: $staging.log) -- build the emulators first, e.g.
    ninja -C $build_dir qemu-system-x86_64.exe qemu-system-ppc.exe"; }
rm -f -- "$staging.log"

installed=$staging/${prefix#/}
[ -d "$installed" ] || die "expected install tree at '$installed', not found"

info "Populating $out_dir"
rm -rf -- "$out_dir"
mkdir -p -- "$out_dir"
# mv rather than cp: the staging tree already holds real files, so this is a
# rename on the same filesystem and costs nothing.
find "$installed" -mindepth 1 -maxdepth 1 -exec mv -- {} "$out_dir/" \;

# Anything left as a symlink here would defeat the whole point of the exercise.
if [ -n "$(find "$out_dir" -type l -print -quit)" ]; then
    die "symlinks present in '$out_dir' -- refusing to ship a tree Windows cannot read"
fi

# Resolve imported DLLs.  Search paths cover the gcc runtime, the distro's mingw
# sysroot and the hand-built sysroot; DLLs that resolve nowhere are Windows's own
# (kernel32, user32, ...) and are correctly left alone.
: "${MINGW_PREFIX:=$HOME/mingw}"
gcc_lib_dir=$(${cross_prefix}gcc -print-search-dirs 2>/dev/null |
              sed -n 's/^install: //p') || true
search_dirs=(
    "$gcc_lib_dir"
    "/usr/${cross_prefix%-}/lib"
    "/usr/${cross_prefix%-}/bin"
    "$MINGW_PREFIX/bin"
    "$MINGW_PREFIX/lib"
)

find_dll() {
    local name=$1 dir
    for dir in "${search_dirs[@]}"; do
        [ -n "$dir" ] && [ -f "$dir/$name" ] && { printf '%s\n' "$dir/$name"; return 0; }
    done
    return 1
}

info "Resolving imported DLLs"
# Iterate to a fixed point: a copied DLL may itself import another one.
copied=0
while :; do
    new=0
    while IFS= read -r -d '' binary; do
        while read -r dll; do
            [ -n "$dll" ] || continue
            [ -e "$out_dir/$dll" ] && continue
            if path=$(find_dll "$dll"); then
                cp -L -- "$path" "$out_dir/$dll"
                printf '    %s <- %s\n' "$dll" "$path"
                new=$((new + 1))
            fi
        done < <("$objdump" -p "$binary" | sed -n 's/^\tDLL Name: //p')
    done < <(find "$out_dir" -maxdepth 1 \( -name '*.exe' -o -name '*.dll' \) -print0)
    copied=$((copied + new))
    [ "$new" -eq 0 ] && break
done
[ "$copied" -eq 0 ] && printf '    none needed (fully static)\n'

cat >"$out_dir/README.txt" <<EOF
QEMU for Windows (x86_64), built from qemu-mod.

Run an emulator directly from this directory, e.g.

    qemu-system-x86_64.exe -m 512

Firmware and keymaps are found automatically in share\\ relative to the
executable; there is no need to pass -L.  Use "-L help" to print the
directories that are actually searched.

Generated by scripts/win64-dist.sh on $(date -u '+%Y-%m-%d %H:%M:%S UTC').
EOF

if $make_zip; then
    info "Creating archive"
    command -v zip >/dev/null || die "'zip' not found in PATH"
    (cd -- "$(dirname -- "$out_dir")" && rm -f -- "$(basename -- "$out_dir").zip" &&
     zip -qr9 -- "$(basename -- "$out_dir").zip" "$(basename -- "$out_dir")")
    info "Archive: $out_dir.zip"
fi

info "Done: $out_dir ($(du -sh -- "$out_dir" | cut -f1), $(find "$out_dir" -type f | wc -l) files)"
