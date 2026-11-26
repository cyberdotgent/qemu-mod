/*
 * AWS tape-image medium
 *
 * AWS records consist of one or more six-byte, little-endian chunk headers
 * followed by chunk data.  Keeping this parser separate from the 3590 CCW
 * implementation makes the guest-visible tape protocol independent of the
 * initial image format.
 *
 * A header holds the length of its chunk (16 bits), the length of the
 * chunk before it (16 bits) and two flag bytes; there are no absolute
 * offsets, so the format does not limit the size of an image.  See
 * tape-aws.h for the limits that do apply.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/s390x/tape-aws.h"
#include "block/block-common.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"

#define AWS_HEADER_SIZE 6
#define AWS_FLAG_NEW_RECORD 0x80
#define AWS_FLAG_TAPE_MARK  0x40
#define AWS_FLAG_END_RECORD 0x20
#define AWS_KNOWN_FLAGS (AWS_FLAG_NEW_RECORD | AWS_FLAG_TAPE_MARK | \
                         AWS_FLAG_END_RECORD)
#define AWS_MAX_CHUNK UINT16_MAX

/*
 * Header read-ahead: spacing, locating and mounting touch only the headers.
 * One page holds many headers of labels and tape marks, and costs little
 * more than six bytes when the blocks are large.
 */
#define AWS_WINDOW_SIZE (4 * KiB)

/*
 * Default distance of the early-warning point from the end of the volume.
 * After logical end of tape the system still writes what its access
 * method has queued (up to 255 buffers of at most 256 KiB) and the trailer
 * labels (tape mark, EOV1, EOV2, user trailer labels, two tape marks).
 */
#define AWS_DEFAULT_MARGIN        (128 * MiB)
#define AWS_DEFAULT_MARGIN_BLOCKS 8192

typedef struct QEMU_PACKED AwsHeader {
    uint16_t length;
    uint16_t previous_length;
    uint8_t flags1;
    uint8_t flags2;
} AwsHeader;

static void aws_window_drop(AwsTape *tape)
{
    tape->window_len = 0;
}

static int aws_pread_header(AwsTape *tape, int64_t offset, AwsHeader *header)
{
    int64_t len;
    int ret;

    if (offset < tape->window_start ||
        offset + AWS_HEADER_SIZE > tape->window_start + tape->window_len) {
        if (!tape->window) {
            tape->window = g_malloc(AWS_WINDOW_SIZE);
        }
        len = MIN(AWS_WINDOW_SIZE, tape->length - offset);
        ret = blk_pread(tape->blk, offset, len, tape->window, 0);
        if (ret < 0) {
            aws_window_drop(tape);
            return ret;
        }
        tape->window_start = offset;
        tape->window_len = len;
    }
    memcpy(header, tape->window + (offset - tape->window_start),
           sizeof(*header));
    return 0;
}

static AwsTapeResult aws_read_header(AwsTape *tape, int64_t offset,
                                     AwsHeader *header)
{
    if (offset == tape->length) {
        return AWS_TAPE_EOT;
    }
    if (offset < 0 || offset > tape->length - AWS_HEADER_SIZE) {
        return AWS_TAPE_FORMAT_ERROR;
    }
    if (aws_pread_header(tape, offset, header) < 0) {
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

void aws_tape_set_limits(AwsTape *tape, int64_t capacity, uint32_t max_blocks,
                         int64_t margin, uint32_t margin_blocks)
{
    tape->capacity = capacity;
    tape->max_blocks = max_blocks;
    if (!margin_blocks) {
        margin_blocks = MIN(AWS_DEFAULT_MARGIN_BLOCKS, max_blocks / 8);
    }
    tape->warn_block = max_blocks - MIN(margin_blocks, max_blocks);
    if (capacity) {
        if (!margin) {
            margin = MIN(AWS_DEFAULT_MARGIN, capacity / 8);
        }
        tape->warn_offset = capacity - MIN(margin, capacity);
    } else {
        tape->warn_offset = INT64_MAX;
    }
}

bool aws_tape_past_warning(const AwsTape *tape)
{
    return tape->offset >= tape->warn_offset ||
           tape->block_id >= tape->warn_block;
}

/*
 * A raw BlockBackend reports the length of a file rounded up to a whole
 * 512-byte sector.  Bytes past the last chunk that are all zero and do not
 * reach the next sector boundary are that padding.
 */
static bool aws_is_padding(AwsTape *tape, int64_t cursor)
{
    uint8_t tail[BDRV_SECTOR_SIZE];
    int64_t len = tape->length - cursor;

    if (len <= 0 || len >= BDRV_SECTOR_SIZE ||
        QEMU_ALIGN_UP(cursor, BDRV_SECTOR_SIZE) != tape->length) {
        return false;
    }
    if (blk_pread(tape->blk, cursor, len, tail, 0) < 0) {
        return false;
    }
    return buffer_is_zero(tail, len);
}

void aws_tape_init(AwsTape *tape, BlockBackend *blk, Error **errp)
{
    int64_t length = blk_getlength(blk);
    int64_t cursor = 0;
    uint16_t previous_length = 0;
    uint64_t blocks = 0;
    bool first = true;

    if (length < 0) {
        error_setg_errno(errp, -length, "cannot determine AWS image size");
        return;
    }
    tape->blk = blk;
    tape->length = length;
    tape->fenced = false;
    aws_window_drop(tape);

    while (cursor < tape->length) {
        AwsHeader header;
        AwsTapeResult result;

        if (aws_is_padding(tape, cursor)) {
            tape->length = cursor;
            break;
        }
        result = aws_read_header(tape, cursor, &header);
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
            blocks++;
        } else {
            if (first != !!(header.flags1 & AWS_FLAG_NEW_RECORD)) {
                error_setg(errp,
                           "invalid AWS record boundary at offset 0x%" PRIx64,
                           cursor);
                return;
            }
            first = !!(header.flags1 & AWS_FLAG_END_RECORD);
            if (first) {
                blocks++;
            }
        }
        previous_length = header.length;
        cursor += AWS_HEADER_SIZE + header.length;
    }
    if (!first) {
        error_setg(errp, "AWS image ends within a logical record");
        return;
    }
    if (blocks > UINT32_MAX) {
        error_setg(errp, "AWS image holds %" PRIu64 " blocks, more than a "
                   "32-bit block ID can address", blocks);
        return;
    }
    aws_tape_rewind(tape);
}

void aws_tape_cleanup(AwsTape *tape)
{
    g_clear_pointer(&tape->window, g_free);
    aws_window_drop(tape);
}

bool aws_tape_restore(AwsTape *tape, BlockBackend *blk, Error **errp)
{
    int64_t backend_length = blk_getlength(blk);
    AwsHeader header;
    AwsTapeResult result;

    if (backend_length < 0) {
        error_setg_errno(errp, -backend_length,
                         "cannot determine AWS image size");
        return false;
    }
    tape->blk = blk;
    aws_window_drop(tape);
    if (tape->length < 0 || tape->length > backend_length ||
        tape->offset < 0 || tape->offset > tape->length) {
        error_setg(errp, "AWS position 0x%" PRIx64 " of 0x%" PRIx64
                   " does not fit the %" PRId64 "-byte medium",
                   tape->offset, tape->length, backend_length);
        return false;
    }
    if (tape->offset == tape->length) {
        return true;
    }

    /*
     * The position must be a block boundary of this medium: a header
     * starting a record or holding a tape mark, linked back to the block
     * the source passed last.
     */
    result = aws_read_header(tape, tape->offset, &header);
    if (result != AWS_TAPE_OK ||
        !(header.flags1 & (AWS_FLAG_NEW_RECORD | AWS_FLAG_TAPE_MARK)) ||
        header.previous_length != tape->previous_length) {
        error_setg(errp, "AWS position 0x%" PRIx64
                   " is not a block boundary of the mounted medium",
                   tape->offset);
        return false;
    }
    return true;
}

void aws_tape_rewind(AwsTape *tape)
{
    tape->offset = 0;
    tape->block_id = 0;
    tape->previous_length = 0;
}

/*
 * Move forward over one record or tape mark, copying the record into @buf
 * unless @buf is NULL.
 */
static AwsTapeResult aws_tape_forward(AwsTape *tape, uint8_t *buf,
                                      size_t capacity, size_t *length)
{
    int64_t cursor = tape->offset;
    size_t total = 0;
    bool first = true;

    *length = 0;
    if (tape->block_id == UINT32_MAX) {
        /* Cannot happen on a medium aws_tape_init() accepted. */
        return AWS_TAPE_FORMAT_ERROR;
    }
    for (;;) {
        AwsHeader header;
        AwsTapeResult result = aws_read_header(tape, cursor, &header);

        if (result == AWS_TAPE_EOT && !first) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        if (result != AWS_TAPE_OK) {
            return result;
        }
        if (first && (header.flags1 & AWS_FLAG_TAPE_MARK)) {
            if (header.length) {
                return AWS_TAPE_FORMAT_ERROR;
            }
            tape->offset = cursor + AWS_HEADER_SIZE;
            tape->block_id++;
            tape->previous_length = 0;
            return AWS_TAPE_MARK;
        }
        if (first != !!(header.flags1 & AWS_FLAG_NEW_RECORD)) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        if (header.flags1 & AWS_FLAG_TAPE_MARK) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        if (total + header.length > AWS_TAPE_MAX_RECORD) {
            return AWS_TAPE_TOO_LARGE;
        }
        if (buf && header.length) {
            int ret;

            if (header.length > capacity - total) {
                return AWS_TAPE_TOO_LARGE;
            }
            ret = blk_pread(tape->blk, cursor + AWS_HEADER_SIZE,
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
            tape->previous_length = header.length;
            *length = total;
            return AWS_TAPE_OK;
        }
        first = false;
    }
}

AwsTapeResult aws_tape_read(AwsTape *tape, uint8_t *buf, size_t capacity,
                            size_t *length)
{
    return aws_tape_forward(tape, buf, capacity, length);
}

AwsTapeResult aws_tape_skip(AwsTape *tape)
{
    size_t length;

    return aws_tape_forward(tape, NULL, 0, &length);
}

/* Find the logical record or tape mark preceding the current position. */
static AwsTapeResult aws_previous_offset(AwsTape *tape, int64_t *result,
                                         AwsHeader *found)
{
    int64_t cursor = tape->offset;
    uint16_t previous_length = tape->previous_length;
    AwsHeader current;
    AwsTapeResult rc;

    if (!cursor) {
        return AWS_TAPE_BOT;
    }

    /*
     * The position carries the length of the chunk before it, also at the
     * end of the data, where there is no following header to read it from.
     */
    for (;;) {
        if (cursor < AWS_HEADER_SIZE + previous_length) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        cursor -= AWS_HEADER_SIZE + previous_length;
        rc = aws_read_header(tape, cursor, &current);
        if (rc != AWS_TAPE_OK) {
            return rc == AWS_TAPE_EOT ? AWS_TAPE_FORMAT_ERROR : rc;
        }
        if (current.length != previous_length) {
            return AWS_TAPE_FORMAT_ERROR;
        }
        if (current.flags1 & (AWS_FLAG_NEW_RECORD | AWS_FLAG_TAPE_MARK)) {
            *result = cursor;
            *found = current;
            return AWS_TAPE_OK;
        }
        previous_length = current.previous_length;
    }
}

AwsTapeResult aws_tape_backspace(AwsTape *tape)
{
    int64_t previous;
    AwsHeader header;
    AwsTapeResult result;

    if (!tape->block_id) {
        return AWS_TAPE_BOT;
    }
    result = aws_previous_offset(tape, &previous, &header);
    if (result != AWS_TAPE_OK) {
        return result;
    }
    tape->offset = previous;
    tape->block_id--;
    tape->previous_length = header.previous_length;
    return header.flags1 & AWS_FLAG_TAPE_MARK ? AWS_TAPE_MARK : AWS_TAPE_OK;
}

AwsTapeResult aws_tape_locate(AwsTape *tape, uint32_t block_id)
{
    int64_t offset = tape->offset;
    uint32_t current = tape->block_id;
    uint16_t previous_length = tape->previous_length;

    if (block_id < tape->block_id) {
        aws_tape_rewind(tape);
    }
    while (tape->block_id < block_id) {
        AwsTapeResult result = aws_tape_skip(tape);

        if (result != AWS_TAPE_OK && result != AWS_TAPE_MARK) {
            tape->offset = offset;
            tape->block_id = current;
            tape->previous_length = previous_length;
            return result == AWS_TAPE_EOT ? AWS_TAPE_NOT_FOUND : result;
        }
    }
    return AWS_TAPE_OK;
}

static AwsTapeResult aws_space_error(int ret)
{
    switch (ret) {
    case -ENOSPC:
    case -EFBIG:
    case -EDQUOT:
        return AWS_TAPE_NO_SPACE;
    default:
        return AWS_TAPE_IO_ERROR;
    }
}

/*
 * Cut the medium back to @end after a failed write.  If even that fails,
 * the content of the medium is unknown: fence it.
 */
static AwsTapeResult aws_tape_rollback(AwsTape *tape, int64_t end, int err)
{
    Error *local_err = NULL;
    AwsTapeResult result = aws_space_error(err);

    aws_window_drop(tape);
    if (result == AWS_TAPE_NO_SPACE &&
        blk_truncate(tape->blk, end, false, PREALLOC_MODE_OFF, 0,
                     &local_err) >= 0) {
        tape->length = end;
        return result;
    }
    error_free(local_err);
    tape->fenced = true;
    return result;
}

static AwsTapeResult aws_tape_commit(AwsTape *tape, const uint8_t *data,
                                     size_t length, uint16_t final_length)
{
    int64_t new_end = tape->offset + length;
    Error *local_err = NULL;
    int ret;

    if (tape->fenced) {
        return AWS_TAPE_FENCED;
    }
    if (!blk_is_writable(tape->blk)) {
        return AWS_TAPE_WRITE_PROTECTED;
    }
    /* Physical end of the volume: the block is not written. */
    if (tape->block_id >= tape->max_blocks ||
        (tape->capacity && new_end > tape->capacity)) {
        return AWS_TAPE_NO_SPACE;
    }

    /*
     * Block backends reject writes beyond their current length.  Extend
     * before writing, but defer shrinking an overwritten tail until after
     * the complete serialized record is safely present.  A write ends the
     * data on the medium, so a failure leaves it ending at the position.
     */
    aws_window_drop(tape);
    if (new_end > tape->length) {
        ret = blk_truncate(tape->blk, new_end, false, PREALLOC_MODE_OFF, 0,
                           &local_err);
        if (ret < 0) {
            error_free(local_err);
            return aws_tape_rollback(tape, tape->offset, ret);
        }
    }
    ret = blk_pwrite(tape->blk, tape->offset, length, data, 0);
    if (ret < 0) {
        return aws_tape_rollback(tape, tape->offset, ret);
    }
    /*
     * Growing an empty file, the file-posix driver writes a whole zeroed
     * sector to speed up later alignment probing, so the file ends up with
     * zeros behind a short first write.  Cut them off: other AWS readers
     * would take them for a chunk header.
     */
    if (new_end < tape->length ||
        (!tape->length && new_end < BDRV_SECTOR_SIZE)) {
        ret = blk_truncate(tape->blk, new_end, false, PREALLOC_MODE_OFF, 0,
                           &local_err);
        if (ret < 0) {
            error_free(local_err);
            tape->fenced = true;
            return AWS_TAPE_IO_ERROR;
        }
    }

    tape->offset = new_end;
    tape->length = new_end;
    tape->block_id++;
    tape->previous_length = final_length;
    return AWS_TAPE_OK;
}

AwsTapeResult aws_tape_write(AwsTape *tape, const uint8_t *buf, size_t length)
{
    size_t chunks;
    size_t serialized_length;
    g_autofree uint8_t *serialized = NULL;
    size_t input_offset = 0;
    size_t output_offset = 0;
    uint16_t previous_length = tape->previous_length;
    size_t i;

    if (!length || length > AWS_TAPE_MAX_RECORD) {
        return AWS_TAPE_TOO_LARGE;
    }

    chunks = DIV_ROUND_UP(length, AWS_MAX_CHUNK);
    serialized_length = length + chunks * AWS_HEADER_SIZE;
    serialized = g_malloc(serialized_length);
    for (i = 0; i < chunks; i++) {
        size_t chunk_length = MIN(length - input_offset,
                                  (size_t)AWS_MAX_CHUNK);
        AwsHeader header = {
            .length = cpu_to_le16(chunk_length),
            .previous_length = cpu_to_le16(previous_length),
            .flags1 = (i == 0 ? AWS_FLAG_NEW_RECORD : 0) |
                      (i + 1 == chunks ? AWS_FLAG_END_RECORD : 0),
        };

        memcpy(serialized + output_offset, &header, sizeof(header));
        memcpy(serialized + output_offset + sizeof(header),
               buf + input_offset, chunk_length);
        output_offset += sizeof(header) + chunk_length;
        input_offset += chunk_length;
        previous_length = chunk_length;
    }

    return aws_tape_commit(tape, serialized, serialized_length,
                           previous_length);
}

AwsTapeResult aws_tape_write_mark(AwsTape *tape)
{
    AwsHeader header = {
        .previous_length = cpu_to_le16(tape->previous_length),
        .flags1 = AWS_FLAG_TAPE_MARK,
    };

    return aws_tape_commit(tape, (uint8_t *)&header, sizeof(header), 0);
}

AwsTapeResult aws_tape_erase_gap(AwsTape *tape)
{
    if (tape->fenced) {
        return AWS_TAPE_FENCED;
    }
    if (!blk_is_writable(tape->blk)) {
        return AWS_TAPE_WRITE_PROTECTED;
    }
    return AWS_TAPE_OK;
}

AwsTapeResult aws_tape_sync(AwsTape *tape)
{
    int ret;

    if (tape->fenced) {
        return AWS_TAPE_FENCED;
    }
    if (!blk_is_writable(tape->blk)) {
        return AWS_TAPE_WRITE_PROTECTED;
    }
    ret = blk_flush(tape->blk);
    if (ret < 0) {
        tape->fenced = true;
        return aws_space_error(ret);
    }
    return AWS_TAPE_OK;
}
