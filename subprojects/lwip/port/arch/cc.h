/*
 * lwIP port for QEMU: compiler and platform
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QEMU_LWIP_ARCH_CC_H
#define QEMU_LWIP_ARCH_CC_H

#include <endian.h>     /* BYTE_ORDER, BIG_ENDIAN, LITTLE_ENDIAN */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define LWIP_ERRNO_STDINCLUDE   1
#define LWIP_TIMEVAL_PRIVATE    0

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
        fprintf(stderr, "lwIP assertion \"%s\" failed at line %d in %s\n", \
                x, __LINE__, __FILE__); \
        abort(); \
    } while (0)

/* provided by the user of the stack (QEMU) */
uint32_t qemu_lwip_rand(void);

#endif
