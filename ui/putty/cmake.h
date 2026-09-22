/*
 * cmake.h for the PuTTY terminal emulator as vendored into QEMU-mod.
 *
 * SPDX-License-Identifier: MIT
 *
 * This file is NOT part of the PuTTY distribution: upstream generates it
 * from cmake/cmake.h.in with the results of its own configure-time probes.
 * We do not run PuTTY's CMake, so the handful of feature macros that the
 * subset of PuTTY we build actually looks at are hardcoded here for the
 * x86_64-w64-mingw32 toolchain.
 */

#ifndef PUTTY_QEMU_CMAKE_H
#define PUTTY_QEMU_CMAKE_H

/* mingw-w64's inttypes.h declares strtoumax(), so platform.h must not. */
#define HAVE_STRTOUMAX 1

/* _countof() is an MSVC extension; mingw-w64's stdlib.h does have it, but
 * PuTTY's fallback is harmless and we do not rely on the real one. */
#define HAVE_COUNTOF 0

/* We do have <stdint.h>. */
#define HAVE_NO_STDINT_H 0

/* Only used by PuTTY's own window.c/Windows front end, which we do not
 * build; defined so that no #if picks up an undefined identifier. */
#define HAVE_GCP_RESULTSW 1
#define HAVE_DWMAPI_H 0
#define HAVE_AFUNIX_H 0
#define HAVE_ADDDLLDIRECTORY 1
#define HAVE_SETDEFAULTDLLDIRECTORIES 1
#define HAVE_GETNAMEDPIPECLIENTPROCESSID 1

/* No Arm here. */
#define HAVE_ARM_DIT 0

#endif /* PUTTY_QEMU_CMAKE_H */
