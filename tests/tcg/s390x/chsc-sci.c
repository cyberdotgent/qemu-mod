/*
 * Verify CHSC Store Configuration Information in basic mode.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <string.h>

typedef struct ChscRequest {
    uint16_t length;
    uint16_t command;
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
} __attribute__((packed)) ChscRequest;

typedef struct ChscResponse {
    uint16_t length;
    uint16_t code;
    uint32_t param;
    uint8_t data[];
} __attribute__((packed)) ChscResponse;

static uint8_t page[4096] __attribute__((aligned(4096)));

static int chsc(void *area)
{
    typedef struct { uint8_t bytes[4096]; } Page;
    int cc;

    asm volatile(
        ".insn rre,0xb25f0000,%2,0\n"
        "ipm %0\n"
        "srl %0,28\n"
        : "=d" (cc), "=m" (*(Page *)area)
        : "d" (area), "m" (*(Page *)area)
        : "cc");
    return cc;
}

static ChscResponse *run(uint16_t length, uint32_t param0)
{
    ChscRequest *request = (ChscRequest *)page;

    memset(page, 0xa5, sizeof(page));
    memset(request, 0, sizeof(*request));
    request->length = length;
    request->command = 0x0012;
    request->param0 = param0;

    if (chsc(page)) {
        return NULL;
    }
    return (ChscResponse *)(page + length);
}

int main(void)
{
    ChscResponse *response;

    response = run(sizeof(ChscRequest), 0);
    if (!response || response->length != 0x0138 ||
        response->code != 0x0001 || response->param ||
        response->data[2] != 0x01) {
        return 1;
    }
    for (unsigned int i = 0; i < 0x0138 - sizeof(*response); i++) {
        if (i != 2 && response->data[i]) {
            return 2;
        }
    }

    response = run(sizeof(ChscRequest) + 8, 0);
    if (!response || response->length != sizeof(*response) ||
        response->code != 0x0003) {
        return 3;
    }

    response = run(sizeof(ChscRequest), 0x10000000);
    if (!response || response->length != sizeof(*response) ||
        response->code != 0x0003) {
        return 4;
    }

    response = run(sizeof(ChscRequest), 0x01000000);
    if (!response || response->length != sizeof(*response) ||
        response->code != 0x0007) {
        return 5;
    }

    response = run(sizeof(ChscRequest), 0x20000000);
    if (!response || response->length != sizeof(*response) ||
        response->code != 0x0003) {
        return 6;
    }
    return 0;
}
