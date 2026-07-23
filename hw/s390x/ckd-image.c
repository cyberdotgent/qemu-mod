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
#include "qemu/bswap.h"
#include "qemu/memalign.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"

#define CKD_TRACK_HEADER_SIZE 5
#define CKD_RECORD_HEADER_SIZE 8
#define CKD_END_TRACK_SIZE 8
#define CKD_3390_HEADS 15
#define CKD_3390_MAX_CYLINDERS 65520
#define CKD_3390_TRACK_SIZE 56832

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

bool ckd_image_open(CkdImage *image, BlockBackend *blk, Error **errp)
{
    CkdDeviceHeader header;
    int64_t length;
    uint64_t data_length;

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
    if (header.device_type != 0x90 || image->heads != CKD_3390_HEADS ||
        image->track_size != CKD_3390_TRACK_SIZE ||
        header.file_sequence || le16_to_cpu(header.high_cylinder)) {
        error_setg(errp, "unsupported CKD geometry or multi-file image");
        return false;
    }

    data_length = length - CKD_IMAGE_HEADER_SIZE;
    if (data_length % ((uint64_t)image->heads * image->track_size)) {
        error_setg(errp, "CKD image length does not contain whole cylinders");
        return false;
    }
    image->cylinders = data_length / ((uint64_t)image->heads *
                                      image->track_size);
    if (!image->cylinders || image->cylinders > CKD_3390_MAX_CYLINDERS) {
        error_setg(errp, "CKD cylinder count %u is outside 1..%u",
                   image->cylinders, CKD_3390_MAX_CYLINDERS);
        return false;
    }

    image->blk = blk;
    memcpy(image->serial, header.serial, sizeof(image->serial));
    image->track = blk_blockalign(blk, image->track_size);
    return true;
}

void ckd_image_close(CkdImage *image)
{
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
        return ret;
    }
    ret = ckd_image_parse_track(image, cylinder, head);
    if (ret < 0) {
        image->track_loaded = false;
        return ret;
    }
    image->loaded_cylinder = cylinder;
    image->loaded_head = head;
    image->track_loaded = true;
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
    ret = blk_pwrite(image->blk, offset, image->track_size, image->track, 0);
    if (ret < 0) {
        image->track_loaded = false;
        return ret;
    }
    image->loaded_cylinder = cylinder;
    image->loaded_head = head;
    image->track_loaded = true;
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

int ckd_image_flush(CkdImage *image)
{
    return blk_flush(image->blk);
}
