/*
 * AWS tape-image medium
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_TAPE_AWS_H
#define HW_S390X_TAPE_AWS_H

#include "qapi/error.h"

typedef struct BlockBackend BlockBackend;

/*
 * Largest logical block: 256 KiB, the largest block of the 3590 (and of
 * the 3490E with its 256 KiB buffer).  An AWS record of that size takes
 * five chunks, since a chunk holds at most 65535 bytes.
 */
#define AWS_TAPE_MAX_RECORD (256 * 1024)

/*
 * Limits of the medium
 *
 * The AWS format itself has no volume limit: a chunk header holds only
 * 16-bit lengths of its own and of the previous chunk, no offsets.  Our
 * position is a 64-bit byte offset and a 32-bit block number, so what
 * bounds a volume is
 *  - the block ID the drive reports and locates by (Read Block ID, Locate
 *    Block): 32 bits of logical block number on a 3590, 22 bits (bits
 *    10-31 of the block ID, under the wrap, segment and format bits) on
 *    the 3480/3490 family.  Every block and every tape mark takes one
 *    number, and the position after the last one must be expressible too,
 *    so a volume holds at most 2^32 - 1 or 2^22 - 1 of them;
 *  - optionally a capacity in bytes chosen by the user;
 *  - the host: a full file system, a quota or the file-size limit.
 * The first two are known in advance: writing past them is refused with
 * physical end of tape, and an early-warning point ahead of them makes the
 * drive report logical end of tape (unit exception on write-type commands)
 * while there is still room for the trailer labels.  The host limits are
 * not: they also end the write with physical end of tape, and the medium
 * keeps everything written before.
 */
#define AWS_TAPE_MAX_BLOCKS_32 UINT32_MAX
#define AWS_TAPE_MAX_BLOCKS_22 ((1u << 22) - 1)

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
    AWS_TAPE_NOT_FOUND,
} AwsTapeResult;

typedef struct AwsTape {
    BlockBackend *blk;
    /* Position and length: migrated */
    int64_t offset;
    int64_t length;
    uint32_t block_id;
    uint16_t previous_length;
    bool fenced;

    /* Configuration: not migrated */
    int64_t capacity;           /* bytes of AWS data, 0 = no byte limit */
    uint32_t max_blocks;        /* blocks and tape marks per volume */
    int64_t warn_offset;        /* early warning at this byte offset ... */
    uint32_t warn_block;        /* ... or at this block number */

    /* Read-ahead window for chunk headers: a cache, not migrated */
    uint8_t *window;
    int64_t window_start;
    int64_t window_len;
} AwsTape;

/*
 * Set the limits: @capacity bytes (0 = none), @max_blocks, and the
 * early-warning margins in front of them (0 = automatic).
 */
void aws_tape_set_limits(AwsTape *tape, int64_t capacity, uint32_t max_blocks,
                         int64_t margin, uint32_t margin_blocks);
void aws_tape_init(AwsTape *tape, BlockBackend *blk, Error **errp);
void aws_tape_cleanup(AwsTape *tape);
/*
 * Take over a medium position restored by migration: @tape holds the
 * migrated offset, length, block ID and previous chunk length, @blk the
 * medium mounted on the destination.  Fails unless the position is a
 * block boundary of that medium.
 */
bool aws_tape_restore(AwsTape *tape, BlockBackend *blk, Error **errp);
void aws_tape_rewind(AwsTape *tape);
AwsTapeResult aws_tape_read(AwsTape *tape, uint8_t *buf, size_t capacity,
                            size_t *length);
/* Move over the next block (OK) or tape mark (MARK) without reading it. */
AwsTapeResult aws_tape_skip(AwsTape *tape);
/* Move back over the previous block (OK) or tape mark (MARK). */
AwsTapeResult aws_tape_backspace(AwsTape *tape);
/* NOT_FOUND if the medium ends before @block_id; position unchanged then */
AwsTapeResult aws_tape_locate(AwsTape *tape, uint32_t block_id);
AwsTapeResult aws_tape_write(AwsTape *tape, const uint8_t *buf, size_t length);
AwsTapeResult aws_tape_write_mark(AwsTape *tape);
AwsTapeResult aws_tape_erase_gap(AwsTape *tape);
AwsTapeResult aws_tape_sync(AwsTape *tape);
/* The position is at or past the early-warning point (logical EOT). */
bool aws_tape_past_warning(const AwsTape *tape);

#endif
