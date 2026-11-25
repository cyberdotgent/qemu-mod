/*
 * Initialised 3390 CKD volumes (volume label and VTOC)
 *
 * Builds the device header and the formatted tracks of a plain CKD or
 * CKD64 image (see docs/system/s390x/3390.rst) holding an empty,
 * initialised 3390 volume: IPL records, a standard volume label and a
 * VTOC with its format-4, format-5 and format-0 DSCBs.  Every track the
 * builder does not produce is an all-zero slot, which the eckd-ccw device
 * presents as an empty track (record 0 only).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_CKD_INIT_H
#define QEMU_CKD_INIT_H

#define CKD_INIT_HEADER_SIZE 512
#define CKD_INIT_TRACK_SIZE 56832
#define CKD_INIT_HEADS 15
#define CKD_INIT_MAX_CYLINDERS 65520
#define CKD_INIT_DEVICE_TYPE 0x90
#define CKD_INIT_DSCBS_PER_TRACK 50
/* The VTOC must end below relative track 65535 (TTR addressing). */
#define CKD_INIT_VTOC_MAX_TRACK 65534

typedef struct CkdInitParams {
    /* Number of cylinders (1 .. CKD_INIT_MAX_CYLINDERS). */
    uint32_t cylinders;
    /* CKD_P064 (true, the default) or CKD_P370 identifier. */
    bool ckd64;
    /* 12 characters for the device header, NUL terminated. */
    char serial[13];
    /* Only write the device header: the guest initialises the volume. */
    bool header_only;
    /* Volume serial, 1 to 6 characters, blank padded in the label. */
    char volser[7];
    /* Owner name in the volume label, up to 14 characters. */
    char owner[15];
    /* VTOC start (cylinder, head) and size in tracks; 0 tracks: default. */
    uint32_t vtoc_cylinder;
    uint32_t vtoc_head;
    uint32_t vtoc_tracks;
} CkdInitParams;

/* Cylinders of a named 3390 model ("3390-3"), 0 if the name is unknown. */
uint32_t ckd_init_model_cylinders(const char *model);

/* Bytes of an image holding @cylinders cylinders, header included. */
uint64_t ckd_init_image_size(uint32_t cylinders);

/*
 * Check the volume serial (1 to 6 characters A-Z, 0-9, national @ # $ or
 * hyphen; lower case letters are converted) and store it normalised in
 * @volser (7 bytes).
 */
bool ckd_init_parse_volser(const char *text, char *volser, Error **errp);

/*
 * Check @p, fill in the defaults (the VTOC at cylinder 0, head 1 for the
 * rest of cylinder 0, as ICKDSF INIT places it on a 3390; a random decimal
 * serial if none is set).
 */
bool ckd_init_check(CkdInitParams *p, Error **errp);

/* The 512-byte device header. */
void ckd_init_build_header(const CkdInitParams *p, uint8_t *buf);

/* First and last track number of the VTOC. */
uint32_t ckd_init_vtoc_first(const CkdInitParams *p);
uint32_t ckd_init_vtoc_last(const CkdInitParams *p);

/*
 * Whether track @track (0-origin, cylinder * 15 + head) is written by the
 * initialisation; all others are empty (all-zero slots).
 */
bool ckd_init_track_is_formatted(const CkdInitParams *p, uint32_t track);

/* Build formatted track @track into @buf (CKD_INIT_TRACK_SIZE bytes). */
void ckd_init_build_track(const CkdInitParams *p, uint32_t track,
                          uint8_t *buf);

#endif
