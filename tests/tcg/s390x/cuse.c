#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CuseResult {
    const uint8_t *addr1;
    int64_t len1;
    const uint8_t *addr2;
    int64_t len2;
    unsigned int cc;
} CuseResult;

static CuseResult cuse(const void *addr1, int64_t len1,
                       const void *addr2, int64_t len2,
                       uint8_t substring_length, uint8_t pad)
{
    register uint64_t r0 asm("r0") = substring_length;
    register uint64_t r1 asm("r1") = pad;
    register const uint8_t *r2 asm("r2") = addr1;
    register int64_t r3 asm("r3") = len1;
    register const uint8_t *r4 asm("r4") = addr2;
    register int64_t r5 asm("r5") = len2;
    unsigned int cc;

    asm volatile(".insn rre,0xb2570000,%%r2,%%r4\n"
                 "ipm %[cc]\n"
                 "srl %[cc],28"
                 : [cc] "=d" (cc), "+d" (r0), "+d" (r1),
                   "+d" (r2), "+d" (r3), "+d" (r4), "+d" (r5)
                 :
                 : "cc", "memory");

    return (CuseResult) { r2, r3, r4, r5, cc };
}

static void check(const char *name, CuseResult result,
                  const uint8_t *addr1, int64_t len1,
                  const uint8_t *addr2, int64_t len2, unsigned int cc)
{
    if (result.addr1 != addr1 || result.len1 != len1 ||
        result.addr2 != addr2 || result.len2 != len2 || result.cc != cc) {
        fprintf(stderr, "%s: got {%td, %" PRId64 ", %td, %" PRId64
                ", cc%u}, expected {%td, %" PRId64 ", %td, %" PRId64
                ", cc%u}\n", name, result.addr1 - addr1, result.len1,
                result.addr2 - addr2, result.len2, result.cc,
                (ptrdiff_t)0, len1, (ptrdiff_t)0, len2, cc);
        exit(EXIT_FAILURE);
    }
}

int main(void)
{
    static const uint8_t a[] = "xxABC";
    static const uint8_t b[] = "yyABC";
    static const uint8_t short_a[] = "xxA";
    static const uint8_t long_a[] = "yyAAA";
    uint8_t *different1 = malloc(5000);
    uint8_t *different2 = malloc(5000);
    CuseResult result;

    check("found", cuse(a, 5, b, 5, 3, 0),
          a + 2, 3, b + 2, 3, 0);
    check("equal tail", cuse(a, 3, b, 3, 3, 0),
          a + 2, 1, b + 2, 1, 1);
    check("unequal tail", cuse(a, 2, b, 2, 3, 0),
          a + 2, 0, b + 2, 0, 2);
    check("zero substring", cuse(a, 5, b, 5, 0, 0),
          a, 5, b, 5, 0);
    check("padding", cuse(short_a, 3, long_a, 5, 3, 'A'),
          short_a + 2, 1, long_a + 2, 3, 0);
    check("negative length", cuse(a, -1, b, 0, 1, 0),
          a, -1, b, 0, 2);

    if (!different1 || !different2) {
        return EXIT_FAILURE;
    }
    memset(different1, 0x11, 5000);
    memset(different2, 0x22, 5000);
    result = cuse(different1, 5000, different2, 5000, 1, 0);
    check("interruptible", result, different1 + 4096, 904,
          different2 + 4096, 904, 3);

    free(different1);
    free(different2);
    return EXIT_SUCCESS;
}
