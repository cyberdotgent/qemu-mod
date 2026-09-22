#!/usr/bin/env python3
#
# Copyright (C) 2020 Red Hat, Inc.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import glob
import os
import shutil
import subprocess
import tempfile


def signcode(path):
    cmd = os.environ.get("SIGNCODE")
    if not cmd:
        return
    subprocess.run([cmd, path])

def find_deps(exe_or_dll, search_path, analyzed_deps):
    deps = [exe_or_dll]
    output = subprocess.check_output(["objdump", "-p", exe_or_dll], text=True)
    output = output.split("\n")
    for line in output:
        if not line.lstrip().startswith("DLL Name: "):
            continue

        dep = line.split("DLL Name: ")[1].strip()
        if dep in analyzed_deps:
            continue

        dll = None
        for directory in search_path:
            candidate = os.path.join(directory, dep)
            if os.path.exists(candidate):
                dll = candidate
                break
        if dll is None:
            # assume it's a Windows provided dll, skip it
            continue

        analyzed_deps.add(dep)
        # locate the dll dependencies recursively
        analyzed_deps, rdeps = find_deps(dll, search_path, analyzed_deps)
        deps.extend(rdeps)

    return analyzed_deps, deps


def read_runtime_dll_list(path):
    """Names of DLLs that are LoadLibrary()d instead of imported.

    find_deps() discovers DLLs by walking the PE import table, which by
    construction cannot see a library that is only ever opened at run time --
    libepoxy dispatches EGL through LoadLibrary("libEGL.dll"), so ANGLE appears
    in no import table at all.  Those names are declared in a shared list file
    (scripts/runtime-loaded-dlls.txt) that scripts/win64-dist.sh reads too.
    """
    if not path or not os.path.exists(path):
        return []

    names = []
    with open(path) as f:
        for line in f:
            line = line.split("#")[0].strip()
            if line:
                names.append(line)
    return names


def main():
    parser = argparse.ArgumentParser(description="QEMU NSIS build helper.")
    parser.add_argument(
        "--meson",
        action="append",
        default=[],
        help="the meson command to install the build tree with "
        "(repeat for a multi-word command); defaults to 'meson'",
    )
    parser.add_argument(
        "--dll-search-dir",
        action="append",
        default=[],
        dest="dll_search_dirs",
        help="extra directory to look for imported DLLs in "
        "(repeatable, searched after the positional dlldir)",
    )
    parser.add_argument(
        "--runtime-dll-list",
        default=None,
        help="file listing DLLs that are loaded with LoadLibrary() instead of "
        "being imported, and so cannot be found by walking import tables",
    )
    parser.add_argument("outfile")
    parser.add_argument("prefix")
    parser.add_argument("srcdir")
    parser.add_argument("dlldir")
    parser.add_argument("cpu")
    parser.add_argument("nsisargs", nargs="*")
    args = parser.parse_args()

    # canonicalize the Windows native prefix path
    prefix = os.path.splitdrive(args.prefix)[1]
    destdir = tempfile.mkdtemp()
    try:
        # 'make install' (and ninja's 'install' target) depends on 'all', which
        # can drag in targets that are not part of the distribution and that may
        # not even build in a given configuration (e.g. a couple of qtest
        # binaries do not link under mingw).  Install what has already been
        # built instead, and fail loudly if something is missing from it.
        meson = args.meson or ["meson"]
        subprocess.run(
            meson + ["install", "--no-rebuild", "--destdir", destdir],
            check=True,
        )
        with open(
            os.path.join(destdir + prefix, "system-emulations.nsh"), "w"
        ) as nsh, open(
            os.path.join(destdir + prefix, "system-mui-text.nsh"), "w"
        ) as muinsh:
            for exe in sorted(glob.glob(
                os.path.join(destdir + prefix, "qemu-system-*.exe")
            )):
                exe = os.path.basename(exe)
                arch = exe[12:-4]
                nsh.write(
                    """
                Section "{0}" Section_{0}
                SetOutPath "$INSTDIR"
                File "${{BINDIR}}\\{1}"
                SectionEnd
                """.format(
                        arch, exe
                    )
                )
                if arch.endswith('w'):
                    desc = arch[:-1] + " emulation (GUI)."
                else:
                    desc = arch + " emulation."

                muinsh.write(
                    """
                !insertmacro MUI_DESCRIPTION_TEXT ${{Section_{0}}} "{1}"
                """.format(arch, desc))

        # The DLLs the binaries import do not all live in one place: with a
        # static dependency sysroot none of them do, and the only ones left are
        # the compiler's own runtime libraries.  Search every directory we were
        # given, in order.
        search_path = [
            d
            for d in [args.dlldir] + args.dll_search_dirs
            if d and os.path.isdir(d)
        ]
        print("Searching %s for the dependent dlls ..." % ", ".join(
            "'%s'" % d for d in search_path))
        dlldir = os.path.join(destdir + prefix, "dll")
        os.mkdir(dlldir)

        analyzed_deps = set()

        def copy_deps(deps):
            for dep in deps:
                dllfile = os.path.join(dlldir, os.path.basename(dep))
                print("Copying '%s' to '%s'" % (dep, dllfile))
                shutil.copy(dep, dllfile)

        for exe in glob.glob(os.path.join(destdir + prefix, "*.exe")):
            signcode(exe)

            # find all dll dependencies
            analyzed_deps, deps = find_deps(exe, search_path, analyzed_deps)
            deps = set(deps)
            deps.remove(exe)

            # copy all dlls to the DLLDIR
            copy_deps(deps)

        # Runtime-loaded DLLs are invisible to the import-table walk above, so
        # they have to be named explicitly; what *they* import is then resolved
        # the ordinary way.  A name that is nowhere to be found is skipped with
        # a warning rather than being fatal: a sysroot without ANGLE is a
        # legitimate build, it just has no GL display path.
        for name in read_runtime_dll_list(args.runtime_dll_list):
            if name in analyzed_deps:
                continue
            for directory in search_path:
                candidate = os.path.join(directory, name)
                if os.path.exists(candidate):
                    analyzed_deps.add(name)
                    analyzed_deps, deps = find_deps(
                        candidate, search_path, analyzed_deps
                    )
                    copy_deps(set(deps))
                    break
            else:
                print(
                    "Warning: runtime-loaded DLL '%s' not found in %s -- "
                    "not packaged" % (name, ", ".join(search_path))
                )

        makensis = [
            "makensis",
            "-V2",
            "-NOCD",
            "-DSRCDIR=" + args.srcdir,
            "-DBINDIR=" + destdir + prefix,
        ]
        if args.cpu == "aarch64" or args.cpu == "x86_64":
            makensis += ["-DW64"]
        # qemu.nsi's "Libraries (DLL)" section does File "${DLLDIR}\*.dll",
        # which makensis rejects outright when the directory is empty.  A fully
        # statically linked build legitimately needs no DLL at all, so in that
        # case leave DLLDIR undefined and let the .nsi drop the section.
        if os.listdir(dlldir):
            makensis += ["-DDLLDIR=" + dlldir]
        else:
            print("No dependent dlls found, omitting the DLL section")

        makensis += ["-DOUTFILE=" + args.outfile] + args.nsisargs
        subprocess.run(makensis, check=True)
        signcode(args.outfile)
    finally:
        shutil.rmtree(destdir)


if __name__ == "__main__":
    main()
