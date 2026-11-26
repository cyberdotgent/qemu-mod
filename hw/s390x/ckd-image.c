/*
 * Plain CKD/CKD64 image access
 *
 * A plain image is a 512-byte device header followed by fixed-size track
 * slots.  The outer BlockBackend may itself be raw, qcow2, or another QEMU
 * block graph; the logical byte stream remains CKD_P370 or CKD_P064.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/s390x/ckd-image.h"
#include "block/accounting.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/memalign.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"

#define CKD_TRACK_HEADER_SIZE 5
#define CKD_RECORD_HEADER_SIZE 8
#define CKD_END_TRACK_SIZE 8
#define CKD_R0_DATA_SIZE 8
#define CKD_HEADS 15
#define CKD_MAX_CYLINDERS 65520

/*
 * The track formats of an image.  The track slot holds the track header,
 * a standard R0, the largest record the device takes and the end-of-track
 * marker, rounded up to 512 bytes: 3390 tracks take a record of 56664
 * bytes, 3380 tracks one of 47476 bytes.  The header names the device by
 * the low byte of its type.
 */
typedef struct CkdTrackFormat {
    uint8_t header_type;
    uint16_t device_type;
    uint32_t track_size;
} CkdTrackFormat;

static const CkdTrackFormat ckd_track_formats[] = {
    { 0x90, 0x3390, 56832 },
    { 0x80, 0x3380, 47616 },
};

typedef struct QEMU_PACKED CkdDeviceHeader {
    uint8_t identifier[8];
    uint32_t heads;
    uint32_t track_size;
    uint8_t device_type;
    uint8_t file_sequence;
    uint16_t high_cylinder;
    uint8_t serial[12];
    uint8_t reserved[480];
} CkdDeviceHeader;

static bool ckd_is_eot(const uint8_t *p)
{
    unsigned int i;

    for (i = 0; i < CKD_END_TRACK_SIZE; i++) {
        if (p[i] != 0xff) {
            return false;
        }
    }
    return true;
}

/*
 * Build an empty track in place of an all-zero slot: home address, a
 * standard record zero (key length 0, data length 8, zero data) and the
 * end-of-track marker.  This is the track ICKDSF leaves after initialising
 * it, the track eckd-ccw writes for Write Home Address/Erase, and Hercules'
 * CCKD null track format 1 (CKD_NULLTRK_FMT1, cckd_null_trk()).  Hercules'
 * default null format 0 also has an end-of-file R1, which an unformatted
 * track does not have.
 */
static void ckd_image_build_empty_track(CkdImage *image, uint16_t cylinder,
                                        uint16_t head)
{
    uint8_t *p = image->track;

    p[0] = 0;
    stw_be_p(p + 1, cylinder);
    stw_be_p(p + 3, head);
    p += CKD_TRACK_HEADER_SIZE;
    stw_be_p(p, cylinder);
    stw_be_p(p + 2, head);
    p[4] = 0;
    p[5] = 0;
    stw_be_p(p + 6, CKD_R0_DATA_SIZE);
    p += CKD_RECORD_HEADER_SIZE + CKD_R0_DATA_SIZE;
    memset(p, 0xff, CKD_END_TRACK_SIZE);
}

static int ckd_image_parse_track(CkdImage *image, uint16_t cylinder,
                                 uint16_t head)
{
    uint8_t *track = image->track;
    uint32_t offset = CKD_TRACK_HEADER_SIZE;
    int previous_record = -1;

    image->record_count = 0;
    image->used_length = 0;
    if (track[0] != 0 || lduw_be_p(track + 1) != cylinder ||
        lduw_be_p(track + 3) != head) {
        return -EINVAL;
    }

    while (offset <= image->track_size - CKD_END_TRACK_SIZE) {
        CkdImageRecord *record;
        uint64_t end;

        if (ckd_is_eot(track + offset)) {
            image->used_length = offset + CKD_END_TRACK_SIZE;
            return 0;
        }
        if (offset > image->track_size - CKD_RECORD_HEADER_SIZE ||
            image->record_count == CKD_IMAGE_MAX_RECORDS) {
            return -EINVAL;
        }

        record = &image->records[image->record_count];
        record->cylinder = lduw_be_p(track + offset);
        record->head = lduw_be_p(track + offset + 2);
        record->number = track[offset + 4];
        record->key_length = track[offset + 5];
        record->data_length = lduw_be_p(track + offset + 6);
        record->header_offset = offset;
        record->key_offset = offset + CKD_RECORD_HEADER_SIZE;
        record->data_offset = record->key_offset + record->key_length;
        end = (uint64_t)record->data_offset + record->data_length;

        if (record->cylinder != cylinder || record->head != head ||
            record->number <= previous_record || end > image->track_size) {
            return -EINVAL;
        }
        previous_record = record->number;
        image->record_count++;
        offset = end;
    }
    return -EINVAL;
}

/*
 * Read the home address of track slot @track_number into @ha (bin byte,
 * cylinder, head).  False if the slot is not (fully) in the image or cannot
 * be read.
 */
static bool ckd_image_peek_home_address(BlockBackend *blk, int64_t length,
                                        uint32_t track_size,
                                        uint64_t track_number, uint8_t *ha)
{
    uint64_t offset = CKD_IMAGE_HEADER_SIZE + track_number * track_size;

    if (offset + track_size > length) {
        return false;
    }
    return blk_pread(blk, offset, CKD_TRACK_HEADER_SIZE, ha, 0) >= 0;
}

/*
 * Whether the track slot right behind the volume's last cylinder holds a
 * formatted track 0 of cylinder @cylinders, i.e. the volume continues there:
 * the image is a member of a multi-file volume or holds more cylinders than
 * claimed.  Padding (zeros or whatever the block device held before) does
 * not look like that.
 */
static bool ckd_image_continues(BlockBackend *blk, int64_t length,
                                uint32_t track_size, uint32_t heads,
                                uint32_t cylinders)
{
    uint8_t ha[CKD_TRACK_HEADER_SIZE];

    return ckd_image_peek_home_address(blk, length, track_size,
                                       (uint64_t)cylinders * heads, ha) &&
           ha[0] == 0 && lduw_be_p(ha + 1) == cylinders &&
           lduw_be_p(ha + 3) == 0;
}

/*
 * Volume size.
 *
 * The device header defines the geometry (device type, heads, track size);
 * the number of cylinders comes from the header where it declares one, else
 * from the image length:
 *
 *  - high_cylinder != 0: the one-origin cylinder count of a single-file
 *    volume.  Hercules writes it with file_sequence 0 (or 1 in its old
 *    single-file form, "fileseq == 1 && highcyl == cyls" in ckddasd.c);
 *    zPDT-style images carry other file_sequence values (seen: 3 and 4)
 *    with the exact count.
 *  - high_cylinder == 0: Hercules' default for a single-file volume
 *    (file_sequence 0): the length defines the size and must be exact,
 *    unless the device's cylinders property gives the count.
 *
 * Hercules multi-file volumes (file_sequence 1..n, high_cylinder = the
 * zero-origin number of the last cylinder in that file, 0 in the last
 * file) are not supported.  Their members are recognised and refused:
 * a member after the first starts with a cylinder other than 0, the last
 * member has high_cylinder 0, and the first member holds one cylinder more
 * than its high_cylinder, whose formatted track follows the claimed end.
 *
 * Bytes behind the volume's last cylinder are padding: block storage rounds
 * volume sizes up (zvol block size, LVM extents, RBD objects).  They are
 * never read as tracks, never written and never shown to the guest; the
 * header is never rewritten, so truncating the image to the header size
 * gives back the original file.  An image shorter than the volume is an
 * error.
 */
bool ckd_image_open(CkdImage *image, BlockBackend *blk, uint32_t cylinders,
                    Error **errp)
{
    CkdDeviceHeader header;
    int64_t length;
    uint64_t data_length;
    uint64_t cylinder_size;
    uint16_t high_cylinder;
    uint8_t ha[CKD_TRACK_HEADER_SIZE];
    unsigned int i;

    memset(image, 0, sizeof(*image));
    length = blk_getlength(blk);
    if (length < 0) {
        error_setg_errno(errp, -length, "cannot determine CKD image size");
        return false;
    }
    if (length < CKD_IMAGE_HEADER_SIZE ||
        blk_pread(blk, 0, sizeof(header), &header, 0) < 0) {
        error_setg(errp, "cannot read CKD image header");
        return false;
    }

    if (!memcmp(header.identifier, "CKD_P370", 8)) {
        image->image64 = false;
    } else if (!memcmp(header.identifier, "CKD_P064", 8)) {
        image->image64 = true;
    } else {
        error_setg(errp, "unsupported DASD image (expected CKD_P370 or "
                   "CKD_P064)");
        return false;
    }

    image->heads = le32_to_cpu(header.heads);
    image->track_size = le32_to_cpu(header.track_size);
    for (i = 0; i < ARRAY_SIZE(ckd_track_formats); i++) {
        if (header.device_type == ckd_track_formats[i].header_type &&
            image->track_size == ckd_track_formats[i].track_size) {
            image->device_type = ckd_track_formats[i].device_type;
        }
    }
    if (!image->device_type || image->heads != CKD_HEADS) {
        error_setg(errp, "unsupported CKD geometry (only 3390 and 3380 "
                   "track formats with 15 heads)");
        return false;
    }

    cylinder_size = (uint64_t)image->heads * image->track_size;
    data_length = length - CKD_IMAGE_HEADER_SIZE;
    high_cylinder = le16_to_cpu(header.high_cylinder);

    if (high_cylinder) {
        /* The header declares the cylinder count. */
        image->cylinders = high_cylinder;
        if (cylinders && cylinders != image->cylinders) {
            error_setg(errp, "cylinders=%u does not match the CKD image "
                       "header (%u cylinders)", cylinders, image->cylinders);
            return false;
        }
    } else if (header.file_sequence) {
        /* the last file of a multi-file volume */
        error_setg(errp, "multi-file CKD images are unsupported");
        return false;
    } else if (cylinders) {
        image->cylinders = cylinders;
    } else {
        /* Neither header nor device tell: the length must be exact. */
        if (data_length % cylinder_size) {
            error_setg(errp, "CKD image length does not contain whole "
                       "cylinders and neither the image header nor the "
                       "cylinders property gives the cylinder count");
            return false;
        }
        image->cylinders = data_length / cylinder_size;
    }

    if (!image->cylinders || image->cylinders > CKD_MAX_CYLINDERS) {
        error_setg(errp, "CKD cylinder count %u is outside 1..%u",
                   image->cylinders, CKD_MAX_CYLINDERS);
        return false;
    }
    if (header.file_sequence &&
        ckd_image_peek_home_address(blk, length, image->track_size, 0, ha) &&
        ha[0] == 0 && lduw_be_p(ha + 1) != 0) {
        /* starts with a cylinder other than 0: a later multi-file member */
        error_setg(errp, "multi-file CKD images are unsupported");
        return false;
    }
    if (data_length < image->cylinders * cylinder_size) {
        error_setg(errp, "CKD image is too short for %u cylinders: "
                   "%" PRId64 " bytes, at least %" PRIu64 " required",
                   image->cylinders, length,
                   CKD_IMAGE_HEADER_SIZE +
                   image->cylinders * cylinder_size);
        return false;
    }

    /*
     * With file_sequence 0 the header's count is authoritative; otherwise
     * make sure the volume does not go on behind the cylinders taken.
     */
    if ((header.file_sequence || !high_cylinder) &&
        ckd_image_continues(blk, length, image->track_size, image->heads,
                            image->cylinders)) {
        if (high_cylinder) {
            error_setg(errp, "multi-file CKD images are unsupported");
        } else {
            error_setg(errp, "CKD image holds more than %u cylinders: "
                       "cylinder %u follows", image->cylinders,
                       image->cylinders);
        }
        return false;
    }

    image->blk = blk;
    memcpy(image->serial, header.serial, sizeof(image->serial));
    image->track = blk_blockalign(blk, image->track_size);
    return true;
}

void ckd_image_close(CkdImage *image)
{
    if (image->blk) {
        ckd_image_flush(image);
    }
    g_clear_pointer(&image->track, qemu_vfree);
    image->blk = NULL;
}

int ckd_image_load_track(CkdImage *image, uint16_t cylinder, uint16_t head)
{
    uint64_t track_number;
    uint64_t offset;
    int ret;

    if (cylinder >= image->cylinders || head >= image->heads) {
        return -ERANGE;
    }
    if (image->track_loaded && image->loaded_cylinder == cylinder &&
        image->loaded_head == head) {
        return 0;
    }

    track_number = (uint64_t)cylinder * image->heads + head;
    offset = CKD_IMAGE_HEADER_SIZE + track_number * image->track_size;
    ret = blk_pread(image->blk, offset, image->track_size, image->track, 0);
    if (ret < 0) {
        image->track_loaded = false;
        return ret;
    }
    /*
     * A slot that was never written (sparse file, thin volume, or a new
     * volume of which only the header was written) reads as zeros.  Present
     * it as an empty track so the guest can read and format it; the slot
     * itself is only written when the guest writes the track.
     */
    if (buffer_is_zero(image->track, image->track_size)) {
        ckd_image_build_empty_track(image, cylinder, head);
    }
    ret = ckd_image_parse_track(image, cylinder, head);
    if (ret < 0) {
        image->track_loaded = false;
        return ret;
    }
    image->loaded_cylinder = cylinder;
    image->loaded_head = head;
    image->track_loaded = true;
    image->track_stale = false;
    return 0;
}

int ckd_image_store_track(CkdImage *image, uint16_t cylinder, uint16_t head,
                          const void *data, size_t length)
{
    uint64_t track_number;
    uint64_t offset;
    int ret;

    if (!blk_is_writable(image->blk)) {
        return -EROFS;
    }
    if (cylinder >= image->cylinders || head >= image->heads ||
        length > image->track_size) {
        return -ERANGE;
    }

    memset(image->track, 0, image->track_size);
    memcpy(image->track, data, length);
    ret = ckd_image_parse_track(image, cylinder, head);
    if (ret < 0) {
        image->track_loaded = false;
        return ret;
    }

    track_number = (uint64_t)cylinder * image->heads + head;
    offset = CKD_IMAGE_HEADER_SIZE + track_number * image->track_size;
    image->dirty = true;
    image->write_count++;
    ret = blk_pwrite(image->blk, offset, image->track_size, image->track, 0);
    if (ret < 0) {
        image->track_loaded = false;
        return ret;
    }
    image->loaded_cylinder = cylinder;
    image->loaded_head = head;
    image->track_loaded = true;
    image->track_stale = false;
    return 0;
}

const CkdImageRecord *ckd_image_find_record(const CkdImage *image,
                                            uint8_t number)
{
    unsigned int i;

    for (i = 0; i < image->record_count; i++) {
        if (image->records[i].number == number) {
            return &image->records[i];
        }
    }
    return NULL;
}

/*
 * The backend may have changed behind the device (block graph change,
 * snapshot revert, another writer): forget the cached track contents.  The
 * parsed records still describe the device's orientation; the caller
 * reads the track again with ckd_image_revalidate() before it uses them.
 */
void ckd_image_invalidate(CkdImage *image)
{
    if (image->track_loaded) {
        image->track_stale = true;
    }
    image->track_loaded = false;
}

/*
 * Read the track that was cached before ckd_image_invalidate() again.  On
 * failure the track has no records, as if it could not be read at all.
 */
int ckd_image_revalidate(CkdImage *image)
{
    int ret;

    if (!image->track_stale) {
        return 0;
    }
    image->track_stale = false;
    ret = ckd_image_load_track(image, image->loaded_cylinder,
                               image->loaded_head);
    if (ret < 0) {
        image->record_count = 0;
        image->used_length = 0;
    }
    return ret;
}

/* Forget the cached track and the records parsed from it (device reset). */
void ckd_image_drop_track(CkdImage *image)
{
    image->track_loaded = false;
    image->track_stale = false;
    image->record_count = 0;
    image->used_length = 0;
}

/*
 * Make the tracks written since the last flush durable.  Nothing is done
 * when no track was written, so read-only channel programs cost nothing.
 * After a failed flush the image stays dirty and the next flush retries.
 */
int ckd_image_flush(CkdImage *image)
{
    BlockAcctStats *stats;
    BlockAcctCookie cookie;
    int ret;

    if (!image->dirty) {
        return 0;
    }
    stats = blk_get_stats(image->blk);
    block_acct_start(stats, &cookie, 0, BLOCK_ACCT_FLUSH);
    ret = blk_flush(image->blk);
    if (ret == 0) {
        block_acct_done(stats, &cookie);
        image->dirty = false;
    } else {
        block_acct_failed(stats, &cookie);
    }
    return ret;
}
