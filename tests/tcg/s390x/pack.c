#include <stdint.h>
#include <unistd.h>

static int test_pack_overlap(void)
{
    uint8_t data[] = {0xaa, 0xaa, 0xf1, 0xf2, 0xf3, 0xc4, 0xaa, 0xaa};
    const uint8_t exp[] = {
        0xaa, 0xaa, 0x00, 0x01, 0x23, 0x4c, 0xaa, 0xaa,
    };
    int i;

    asm volatile(
        "    pack 2(4,%[data]),2(4,%[data])\n"
        :
        : [data] "a" (data)
        : "memory");
    for (i = 0; i < sizeof(data); i++) {
        if (data[i] != exp[i]) {
            return 1;
        }
    }
    return 0;
}

static int test_unpack_full_source(void)
{
    uint8_t dest[5];
    const uint8_t src[] = {0x0f, 0xf0, 0x0f};
    const uint8_t exp[] = {0xf0, 0xff, 0xff, 0xf0, 0xf0};
    int i;

    asm volatile(
        "    unpk 0(5,%[dest]),0(3,%[src])\n"
        :
        : [dest] "a" (dest), [src] "a" (src)
        : "memory");
    for (i = 0; i < sizeof(dest); i++) {
        if (dest[i] != exp[i]) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    if (test_pack_overlap() || test_unpack_full_source()) {
        write(STDOUT_FILENO, "bad data\n", 9);
        return 1;
    }
    return 0;
}
