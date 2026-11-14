# Building and transferring

## Revalidate prerequisites

```
ssh -o BatchMode=yes -o ConnectTimeout=10 friedkiwi@chungus.lan.cyber.gent 'uname -m; free -h; df -h /tmp; ps -eo pid,rss,args --sort=-rss | head -12'
file build-s390x-kvm/qemu-system-s390x
command -v s390x-linux-gnu-gcc
```

An old Python scan exhausted roughly187GiB RAM plus8GiB swap on this192GiB LPAR and starved SSH. Use mmap/chunked scans, bounded debugger reads/logs, modest build concurrency (start with -j4), and inspect memory before adding guests. Do not kill unrelated processes or all QEMU/s3270 processes. Stop only validated task-owned PIDs.

## Local cross build

Known existing build directory is `build-s390x-kvm`; compiler is `s390x-linux-gnu-gcc`, sysroot `/opt/qemu-s390x-sysroot`, and pkg-config metadata is under `usr/lib/s390x-linux-gnu/pkgconfig` inside it. Inspect `config.log`, `config-meson.cross`, and `meson-logs/meson-setup.txt` before reusing a build. Directory names containing tcg are not suitable evidence of a KVM-only configuration.

For an already configured coherent tree:

```
ninja -C build-s390x-kvm -j4 qemu-system-s390x
file build-s390x-kvm/qemu-system-s390x
s390x-linux-gnu-readelf -h build-s390x-kvm/qemu-system-s390x
s390x-linux-gnu-readelf -d build-s390x-kvm/qemu-system-s390x
```

For a fresh directory, this is a starting configuration, not a guarantee that the sysroot is complete:

```
mkdir build-chungus-kvm
cd build-chungus-kvm
env PKG_CONFIG_SYSROOT_DIR=/opt/qemu-s390x-sysroot \
    PKG_CONFIG_LIBDIR=/opt/qemu-s390x-sysroot/usr/lib/s390x-linux-gnu/pkgconfig:/opt/qemu-s390x-sysroot/usr/share/pkgconfig \
    ../configure --target-list=s390x-softmmu \
      --cross-prefix=s390x-linux-gnu- --host-cc=gcc \
      --extra-cflags=--sysroot=/opt/qemu-s390x-sysroot \
      --extra-ldflags=--sysroot=/opt/qemu-s390x-sysroot \
      --enable-kvm --disable-tcg --disable-docs --disable-guest-agent \
      --disable-tools --disable-modules --disable-gtk --disable-sdl
ninja -j4 qemu-system-s390x
```

Explicitly scope pkg-config to s390x libraries; do not let amd64 library paths leak into the cross build. Previous full cross builds encountered incomplete GnuTLS/dependency headers and mixed libc headers, although individual objects and guest tests compiled. Inspect the first actual error. Do not claim successful full build based on object compilation or stale output binaries. Do not repair a shared sysroot by replacing broad directory trees without understanding provenance.

## Native fallback on CHUNGUS

When the cross sysroot blocks a build, use an isolated native tree. Transfer a known committed source tree (plus explicitly named patches only if needed). `git archive` excludes submodule contents; ROM/submodule build dependencies must be supplied separately or resolved deliberately. A scoped `git diff --binary` patch can update a compatible existing tree; first inspect and use `git apply --check`.

Native configure uses `--target-list=s390x-softmmu --enable-kvm --disable-tcg` without `--cross-prefix` or local sysroot settings. Build out of tree with `ninja -j4 qemu-system-s390x`. Verify configure says TCG support NO. Do not silently copy all remote dirty changes into an isolated tree; include only necessary reviewed overlays and record their diff/hash.

## Stage and test with scp

Choose a unique task directory (the following commands create one):

```
ssh friedkiwi@chungus.lan.cyber.gent 'mktemp -d /tmp/qemu-chungus.XXXXXX'
# Use the exact returned directory below, replacing REMOTE_STAGE.
scp build-s390x-kvm/qemu-system-s390x friedkiwi@chungus.lan.cyber.gent:REMOTE_STAGE/qemu-system-s390x
sha256sum build-s390x-kvm/qemu-system-s390x
ssh friedkiwi@chungus.lan.cyber.gent 'chmod 0755 REMOTE_STAGE/qemu-system-s390x; sha256sum REMOTE_STAGE/qemu-system-s390x; file REMOTE_STAGE/qemu-system-s390x; REMOTE_STAGE/qemu-system-s390x --version'
```

Replace placeholders before running. Test the staged binary with KVM and required firmware (`-L /home/friedkiwi/qemu-mod/share/qemu`). A binary copied without matching firmware/data files may start but fail later. Check shared-library compatibility on CHUNGUS. The local amd64 host cannot validate KVM execution of this s390x binary.

Tests live under `tests/tcg/s390x` for historical naming; select explicit KVM wrappers/arguments. Current compatibility fixture is `stsi-zos-compat.c` on `mtm-3907`, plus generic-machine disabled fixture. Build them as bare-metal guests and launch via `-kernel`. `mtm-3907` validates host3907 and requires a memory size aligned to the host storage increment; existing tests use8192M, not128M. Inspect current meson entries and wrappers rather than guessing commands.

## Install only when deployment is authorized

The expected installed path is `/home/friedkiwi/qemu-mod/bin/qemu-system-s390x`. Record source hash/provenance and tests first. On the remote host use explicit validated paths, make a uniquely named backup of the current file, install a staged replacement beside the destination, and rename it over the destination atomically. Do not truncate/overwrite the running executable in place.

Example remote shell fragment, after replacing STAGED_BINARY and UNIQUE_BACKUP:

```
set -eu
src=STAGED_BINARY
dst=/home/friedkiwi/qemu-mod/bin/qemu-system-s390x
backup=UNIQUE_BACKUP
test -f "$src"
test ! -e "$backup"
cp -p "$dst" "$backup"
install -m 0755 "$src" "$dst.new-chungus"
mv -f "$dst.new-chungus" "$dst"
sha256sum "$src" "$dst"
"$dst" --version
```

Choose a unique temporary destination if another deployment could overlap. Installing does not restart existing guests: `/proc/PID/exe` may display `(deleted)` after the backing pathname is replaced; the process retains its old image. Report that distinction. To roll back, stage the validated backup and rename it in the same fashion; do not restart guests unless requested.
