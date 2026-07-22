/*
 * AWS tape-image medium
 *
 * AWS records consist of one or more six-byte, little-endian chunk headers
 * followed by chunk data.  Keeping this parser separate from the 3590 CCW
 * implementation makes the guest-visible tape protocol independent of the
 * initial image format.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/s390x/tape-aws.h"
#include "qemu/bswap.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"

#define AWS_HEADER_SIZE 6
#define AWS_FLAG_NEW_RECORD 0x80
#define AWS_FLAG_TAPE_MARK  0x40
#define AWS_FLAG_END_RECORD 0x20
#define AWS_KNOWN_FLAGS (AWS_FLAG_NEW_RECORD | AWS_FLAG_TAPE_MARK | \
                         AWS_FLAG_END_RECORD)

typedef struct QEMU_PACKED AwsHeader {
    uint16_t length;
    uint16_t previous_length;
    uint8_t flags1;
    uint8_t flags2;
} AwsHeader;

static AwsTapeResult aws_read_header(AwsTape *tape, int64_t offset,
                                     AwsHeader *header)
{
    int ret;

    if (offset == tape->length) {
        return AWS_TAPE_EOT;
    }
    if (offset < 0 || offset > tape->length - AWS_HEADER_SIZE) {
        return AWS_TAPE_FORMAT_ERROR;
    }
    ret = blk_pread(tape->blk, offset, sizeof(*header), header, 0);
    if (ret < 0) {
        return AWS_TAPE_IO_ERROR;
    }
    header->length = le16_to_cpu(header->length);
    header->previous_length = le16_to_cpu(header->previous_length);
    if (header->flags2 || (header->flags1 & ~AWS_KNOWN_FLAGS) ||
        offset + AWS_HEADER_SIZE + header->length > tape->length) {
        return AWS_TAPE_FORMAT_ERROR;
    }
    return AWS_TAPE_OK;
}

void aws_tape_init(AwsTape *tape, BlockBackend *blk, Error **errp)
{
    int64_t length = blk_getlength(blk);
    int64_t cursor = 0;
    uint16_t previous_length = 0;
    bool first = true;

    if (length < 0) {
        error_setg_errno(errp, -length, "cannot determine AWS image size");
        return;
    }
    tape->blk = blk;
    tape->length = length;

    while (cursor < tape->length) {
        AwsHeader header;
        AwsTapeResult result = aws_read_header(tape, cursor, &header);

        /* A raw BlockBackend rounds a short file up to a 512-byte boundary. */
        if (result == AWS_TAPE_OK && !header.length &&
            !header.previous_length && !header.flags1 && !header.flags2 &&
            tape->length - cursor < 512) {
            tape->length = cursor;
            break;
        }
        if (result != AWS_TAPE_OK ||
            header.previous_length != previous_length) {
            error_setg(errp, "invalid AWS chunk header at offset 0x%" PRIx64,
                       cursor);
            return;
        }
        if (header.flags1 & AWS_FLAG_TAPE_MARK) {
            if (!first || header.length) {
                error_setg(errp, "invalid AWS tape mark at offset 0x%" PRIx64,
                           cursor);
                return;
            }
            first = true;
        } else {
            if (first != !!(header.flags1 & AWS_FLAG_NEW_RECORD)) {
                error_setg(errp,
                           "invalid AWS record boundary at offset 0x%" PRIx64,
                           cursor);
                return;
            }
            first = !!(header.flags1 & AWS_FLAG_END_RECORD);
        }
        previous_length = header.length;
        cursor += AWS_HEADER_SIZE + header.length;
    }
    if (!first) {
        error_setg(errp, "AWS image ends within a logical record");
        return;
    }
    aws_tape_rewind(tape);
}

void aws_tape_rewind(AwsTape *tape)
{
    tape->offset = 0;
    tape->block_id = 0;
}

AwsTapeResult aws_tape_read(AwsTape *tape, uint8_t *buf, size_t capacity,
                            size_t *length)
{
    int64_t cursor = tape->offset;
    size_t total = 0;
    bool first = true;

    *length = 0;
    for (;;) {
        AwsHeader header;
        AwsTapeResult result = aws_read_header(tape, cursor, &header);

        if (result != AWS_TAPE_OK) {
            return result;
        }
        if (first && (header.flags1 & AWS_FLAG_TAPE_MARK)) {
            if (header.length) {
                return AWS_TAPE_FORMAT_ERROR;
            }
            tape->offset = cursor + AWS_HEADER_SIZE;
            tape->block_id++;
            return AWS_TAPE_MARK;
        }
        if (first != !!(header.flags1 & AWS_FLAG_NEW_RECORD)) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        if (header.flags1 & AWS_FLAG_TAPE_MARK) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        if (header.length > capacity - total) {
            return AWS_TAPE_TOO_LARGE;
        }
        if (header.length) {
            int ret = blk_pread(tape->blk, cursor + AWS_HEADER_SIZE,
                                header.length, buf + total, 0);

            if (ret < 0) {
                return AWS_TAPE_IO_ERROR;
            }
        }
        total += header.length;
        cursor += AWS_HEADER_SIZE + header.length;
        if (header.flags1 & AWS_FLAG_END_RECORD) {
            tape->offset = cursor;
            tape->block_id++;
            *length = total;
            return AWS_TAPE_OK;
        }
        first = false;
    }
}

/* Find the logical record or tape mark preceding the current position. */
static AwsTapeResult aws_previous_offset(AwsTape *tape, int64_t *result)
{
    int64_t cursor = tape->offset;
    AwsHeader current;
    AwsTapeResult rc;

    if (!cursor) {
        return AWS_TAPE_BOT;
    }

    rc = aws_read_header(tape, cursor, &current);
    if (rc == AWS_TAPE_EOT) {
        /*
         * EOF has no following header carrying a backward pointer.  Locate
         * the final chunk once, then follow its normal backward links.
         */
        int64_t scan = 0;

        while (scan < tape->length) {
            cursor = scan;
            rc = aws_read_header(tape, scan, &current);
            if (rc != AWS_TAPE_OK) {
                return rc;
            }
            scan += AWS_HEADER_SIZE + current.length;
        }
        if (scan != tape->length) {
            return AWS_TAPE_FORMAT_ERROR;
        }
    } else if (rc == AWS_TAPE_OK) {
        if (cursor < AWS_HEADER_SIZE + current.previous_length) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        cursor -= AWS_HEADER_SIZE + current.previous_length;
        rc = aws_read_header(tape, cursor, &current);
        if (rc != AWS_TAPE_OK) {
            return rc;
        }
    } else {
        return rc;
    }

    for (;;) {
        if (current.flags1 & (AWS_FLAG_NEW_RECORD | AWS_FLAG_TAPE_MARK)) {
            *result = cursor;
            return AWS_TAPE_OK;
        }
        if (cursor < AWS_HEADER_SIZE + current.previous_length) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        cursor -= AWS_HEADER_SIZE + current.previous_length;
        rc = aws_read_header(tape, cursor, &current);
        if (rc != AWS_TAPE_OK) {
            return rc;
        }
    }
}

AwsTapeResult aws_tape_backspace(AwsTape *tape)
{
    int64_t previous;
    AwsTapeResult result = aws_previous_offset(tape, &previous);

    if (result == AWS_TAPE_OK) {
        tape->offset = previous;
        tape->block_id--;
    }
    return result;
}

AwsTapeResult aws_tape_locate(AwsTape *tape, uint32_t block_id)
{
    uint8_t scratch[AWS_TAPE_MAX_RECORD];
    size_t length;

    aws_tape_rewind(tape);
    while (tape->block_id < block_id) {
        AwsTapeResult result = aws_tape_read(tape, scratch, sizeof(scratch),
                                             &length);
        if (result != AWS_TAPE_OK && result != AWS_TAPE_MARK) {
            return result;
        }
    }
    return AWS_TAPE_OK;
}
