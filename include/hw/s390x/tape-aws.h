/*
 * AWS tape-image medium
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_TAPE_AWS_H
#define HW_S390X_TAPE_AWS_H

#include "qapi/error.h"

typedef struct BlockBackend BlockBackend;

#define AWS_TAPE_MAX_RECORD (256 * 1024)

typedef enum AwsTapeResult {
    AWS_TAPE_OK,
    AWS_TAPE_MARK,
    AWS_TAPE_EOT,
    AWS_TAPE_BOT,
    AWS_TAPE_IO_ERROR,
    AWS_TAPE_FORMAT_ERROR,
    AWS_TAPE_TOO_LARGE,
    AWS_TAPE_WRITE_PROTECTED,
    AWS_TAPE_NO_SPACE,
    AWS_TAPE_FENCED,
} AwsTapeResult;

typedef struct AwsTape {
    BlockBackend *blk;
    int64_t offset;
    int64_t length;
    uint32_t block_id;
    uint16_t previous_length;
    bool fenced;
} AwsTape;

void aws_tape_init(AwsTape *tape, BlockBackend *blk, Error **errp);
void aws_tape_rewind(AwsTape *tape);
AwsTapeResult aws_tape_read(AwsTape *tape, uint8_t *buf, size_t capacity,
                            size_t *length);
AwsTapeResult aws_tape_backspace(AwsTape *tape);
AwsTapeResult aws_tape_locate(AwsTape *tape, uint32_t block_id);
AwsTapeResult aws_tape_write(AwsTape *tape, const uint8_t *buf, size_t length);
AwsTapeResult aws_tape_write_mark(AwsTape *tape);
AwsTapeResult aws_tape_erase_gap(AwsTape *tape);
AwsTapeResult aws_tape_sync(AwsTape *tape);

#endif
