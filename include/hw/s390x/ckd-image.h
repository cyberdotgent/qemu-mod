/*
 * Plain CKD/CKD64 image access
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_CKD_IMAGE_H
#define HW_S390X_CKD_IMAGE_H

#include "qapi/error.h"

typedef struct BlockBackend BlockBackend;

#define CKD_IMAGE_HEADER_SIZE 512
#define CKD_IMAGE_MAX_RECORDS 256

typedef struct CkdImageRecord {
    uint16_t cylinder;
    uint16_t head;
    uint8_t number;
    uint8_t key_length;
    uint16_t data_length;
    uint32_t header_offset;
    uint32_t key_offset;
    uint32_t data_offset;
} CkdImageRecord;

typedef struct CkdImage {
    BlockBackend *blk;
    bool image64;
    uint32_t cylinders;
    uint32_t heads;
    uint32_t track_size;
    uint8_t serial[12];

    uint8_t *track;
    uint16_t loaded_cylinder;
    uint16_t loaded_head;
    bool track_loaded;
    uint32_t used_length;
    unsigned int record_count;
    CkdImageRecord records[CKD_IMAGE_MAX_RECORDS];
} CkdImage;

bool ckd_image_open(CkdImage *image, BlockBackend *blk, Error **errp);
void ckd_image_close(CkdImage *image);
int ckd_image_load_track(CkdImage *image, uint16_t cylinder, uint16_t head);
int ckd_image_store_track(CkdImage *image, uint16_t cylinder, uint16_t head,
                          const void *data, size_t length);
const CkdImageRecord *ckd_image_find_record(const CkdImage *image,
                                            uint8_t number);
int ckd_image_flush(CkdImage *image);

#endif
