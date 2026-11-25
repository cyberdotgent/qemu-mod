/*
 * Initialised 3390 CKD volumes (volume label and VTOC)
 *
 * What a minimal initialisation writes, written from the published record
 * formats:
 *
 *  - cylinder 0, head 0: record 0, the IPL bootstrap records IPL1 (24 bytes)
 *    and IPL2 (144 bytes) and the volume label VOL1 (80 bytes) as record 3,
 *    each with a 4-byte key equal to its name (IBM SC26-7410, Using Data
 *    Sets, "Volume Label Group"; IBM GC35-0033, ICKDSF User's Guide, "IPL
 *    bootstrap record contents").  IPL1 of a volume without an IPL
 *    program holds a disabled wait PSW and a NOP CCW, so an IPL from it
 *    stops at once with wait code X'0F'; IPL2 is the system-supplied
 *    bootstrap record.
 *  - the VTOC, by default at cylinder 0, head 1 for 14 tracks (the ICKDSF
 *    INIT default on a 3390): record 0 and 50 DSCB records (key 44, data 96)
 *    per track: a format-4 DSCB (volume and VTOC description), a format-5
 *    DSCB (free space) and format-0 DSCBs (unused, all zero) for the rest
 *    (IBM SC26-7400, Advanced Services, "Data Set Control Block (DSCB)
 *    Types").
 *  - every other track empty: home address and record 0 only, which an
 *    all-zero track slot stands for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/ckd-init.h"
#include "hw/s390x/ebcdic.h"

#define CKD_HA_SIZE 5
#define CKD_COUNT_SIZE 8
#define CKD_DSCB_KEY 44
#define CKD_DSCB_DATA 96

/* 3390 device constants for the format-4 DSCB. */
#define CKD_3390_TRACK_LENGTH 58786
#define CKD_3390_DEVFG 0x30
#define CKD_3390_DIR_BLOCKS 45

/* DS4VTOCI: free space in the format-5 DSCBs is not to be trusted. */
#define DS4DOSBT 0x80

static const struct {
    const char *name;
    uint32_t cylinders;
} ckd_models[] = {
    { "3390-1", 1113 },
    { "3390-2", 2226 },
    { "3390-3", 3339 },
    { "3390-9", 10017 },
    { "3390-27", 32760 },
    { "3390-54", 65520 },
};

uint32_t ckd_init_model_cylinders(const char *model)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(ckd_models); i++) {
        if (!g_ascii_strcasecmp(model, ckd_models[i].name)) {
            return ckd_models[i].cylinders;
        }
    }
    return 0;
}

uint64_t ckd_init_image_size(uint32_t cylinders)
{
    return CKD_INIT_HEADER_SIZE +
           (uint64_t)cylinders * CKD_INIT_HEADS * CKD_INIT_TRACK_SIZE;
}

static bool ckd_volser_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '@' || c == '#' || c == '$' || c == '-';
}

bool ckd_init_parse_volser(const char *text, char *volser, Error **errp)
{
    size_t len = strlen(text);
    size_t i;

    if (len < 1 || len > 6) {
        error_setg(errp, "volume serial '%s' must have 1 to 6 characters",
                   text);
        return false;
    }
    for (i = 0; i < len; i++) {
        char c = g_ascii_toupper(text[i]);

        if (!ckd_volser_char(c)) {
            error_setg(errp, "volume serial '%s': invalid character '%c' "
                       "(allowed: A-Z, 0-9, @, #, $, -)", text, text[i]);
            return false;
        }
        volser[i] = c;
    }
    volser[len] = 0;
    return true;
}

uint32_t ckd_init_vtoc_first(const CkdInitParams *p)
{
    return p->vtoc_cylinder * CKD_INIT_HEADS + p->vtoc_head;
}

uint32_t ckd_init_vtoc_last(const CkdInitParams *p)
{
    return ckd_init_vtoc_first(p) + p->vtoc_tracks - 1;
}

bool ckd_init_check(CkdInitParams *p, Error **errp)
{
    uint64_t tracks;
    uint64_t first, last;
    size_t i;

    if (p->cylinders < 1 || p->cylinders > CKD_INIT_MAX_CYLINDERS) {
        error_setg(errp, "a 3390 volume has 1 to %u cylinders, not %u "
                   "(extended address volumes are not supported)",
                   CKD_INIT_MAX_CYLINDERS, p->cylinders);
        return false;
    }
    if (!p->serial[0]) {
        for (i = 0; i < 12; i++) {
            p->serial[i] = '0' + g_random_int_range(0, 10);
        }
        p->serial[12] = 0;
    }
    if (p->header_only) {
        return true;
    }
    if (!p->volser[0]) {
        error_setg(errp, "a volume serial is needed to initialise a volume");
        return false;
    }
    if (strlen(p->owner) > 14) {
        error_setg(errp, "the owner has at most 14 characters");
        return false;
    }
    if (!p->vtoc_tracks) {
        /* ICKDSF default: cylinder 0, track 1 for the rest of cylinder 0 */
        p->vtoc_cylinder = 0;
        p->vtoc_head = 1;
        p->vtoc_tracks = CKD_INIT_HEADS - 1;
    }
    tracks = (uint64_t)p->cylinders * CKD_INIT_HEADS;
    if (p->vtoc_head >= CKD_INIT_HEADS) {
        error_setg(errp, "VTOC head %u: a 3390 cylinder has heads 0 to %u",
                   p->vtoc_head, CKD_INIT_HEADS - 1);
        return false;
    }
    first = (uint64_t)p->vtoc_cylinder * CKD_INIT_HEADS + p->vtoc_head;
    last = first + p->vtoc_tracks - 1;
    if (first == 0) {
        error_setg(errp, "the VTOC cannot start on cylinder 0, head 0");
        return false;
    }
    if (last >= tracks) {
        error_setg(errp, "the VTOC (tracks %" PRIu64 " to %" PRIu64 ") does "
                   "not fit on a volume of %u cylinders", first, last,
                   p->cylinders);
        return false;
    }
    if (last > CKD_INIT_VTOC_MAX_TRACK) {
        error_setg(errp, "the VTOC must end below relative track 65535 "
                   "(cylinder 4369, head 0)");
        return false;
    }
    if ((uint64_t)p->vtoc_tracks * CKD_INIT_DSCBS_PER_TRACK - 2 > 0xffff) {
        error_setg(errp, "a VTOC of %u tracks is too large", p->vtoc_tracks);
        return false;
    }
    return true;
}

void ckd_init_build_header(const CkdInitParams *p, uint8_t *buf)
{
    memset(buf, 0, CKD_INIT_HEADER_SIZE);
    memcpy(buf, p->ckd64 ? "CKD_P064" : "CKD_P370", 8);
    stl_le_p(buf + 8, CKD_INIT_HEADS);
    stl_le_p(buf + 12, CKD_INIT_TRACK_SIZE);
    buf[16] = CKD_INIT_DEVICE_TYPE;
    buf[17] = 0;                                  /* file sequence */
    stw_le_p(buf + 18, p->cylinders);             /* high cylinder */
    memcpy(buf + 20, p->serial, MIN(strlen(p->serial), 12));
}

bool ckd_init_track_is_formatted(const CkdInitParams *p, uint32_t track)
{
    if (p->header_only) {
        return false;
    }
    return track == 0 ||
           (track >= ckd_init_vtoc_first(p) && track <= ckd_init_vtoc_last(p));
}

/* Store a CCHHR address of track @track, record @record. */
static void ckd_put_cchhr(uint8_t *p, uint32_t track, uint8_t record)
{
    stw_be_p(p, track / CKD_INIT_HEADS);
    stw_be_p(p + 2, track % CKD_INIT_HEADS);
    p[4] = record;
}

static void ckd_put_cchh(uint8_t *p, uint32_t track)
{
    stw_be_p(p, track / CKD_INIT_HEADS);
    stw_be_p(p + 2, track % CKD_INIT_HEADS);
}

/* Append a record (count, key, data) at @pos; returns the new position. */
static uint8_t *ckd_put_record(uint8_t *pos, uint32_t track, uint8_t record,
                               const void *key, uint8_t key_length,
                               const void *data, uint16_t data_length)
{
    ckd_put_cchhr(pos, track, record);
    pos[5] = key_length;
    stw_be_p(pos + 6, data_length);
    pos += CKD_COUNT_SIZE;
    if (key_length) {
        memcpy(pos, key, key_length);
        pos += key_length;
    }
    memcpy(pos, data, data_length);
    return pos + data_length;
}

static void ckd_put_ebcdic(uint8_t *p, const char *text, size_t len)
{
    size_t i, n = strlen(text);

    for (i = 0; i < len; i++) {
        p[i] = ascii2ebcdic[(uint8_t)(i < n ? text[i] : ' ')];
    }
}

static void ckd_build_vol1(const CkdInitParams *p, uint8_t *vol1)
{
    /* 80 bytes, blank where nothing is set */
    memset(vol1, 0x40, 80);
    ckd_put_ebcdic(vol1, "VOL1", 4);
    ckd_put_ebcdic(vol1 + 4, p->volser, 6);
    vol1[10] = 0xc0;                              /* security: none */
    ckd_put_cchhr(vol1 + 11, ckd_init_vtoc_first(p), 1);
    ckd_put_ebcdic(vol1 + 37, p->owner, 14);
}

/* Free extents of the empty volume: everything but track 0 and the VTOC. */
typedef struct {
    uint32_t start;
    uint32_t length;
} CkdExtent;

static unsigned ckd_free_extents(const CkdInitParams *p, CkdExtent *ext)
{
    uint32_t tracks = p->cylinders * CKD_INIT_HEADS;
    uint32_t first = ckd_init_vtoc_first(p), last = ckd_init_vtoc_last(p);
    unsigned n = 0;

    if (first > 1) {
        ext[n++] = (CkdExtent) { 1, first - 1 };
    }
    if (last + 1 < tracks) {
        ext[n++] = (CkdExtent) { last + 1, tracks - last - 1 };
    }
    return n;
}

static void ckd_put_f5_extent(uint8_t *p, const CkdExtent *e)
{
    stw_be_p(p, e->start);                        /* relative track */
    stw_be_p(p + 2, e->length / CKD_INIT_HEADS);  /* cylinders */
    p[4] = e->length % CKD_INIT_HEADS;            /* additional tracks */
}

static void ckd_build_f4(const CkdInitParams *p, uint8_t *key, uint8_t *d)
{
    uint32_t first = ckd_init_vtoc_first(p), last = ckd_init_vtoc_last(p);

    memset(key, 0x04, CKD_DSCB_KEY);
    memset(d, 0, CKD_DSCB_DATA);
    d[0] = 0xf4;                                  /* DS4IDFMT */
    /*
     * DS4HPCHR: no format-1 DSCB yet; the highest DSCB in use is the
     * format-5 DSCB, record 2 of the first VTOC track.
     */
    ckd_put_cchhr(d + 1, first, 2);
    /* DS4DSREC: format-0 DSCBs */
    stw_be_p(d + 6, p->vtoc_tracks * CKD_INIT_DSCBS_PER_TRACK - 2);
    /* DS4HCCHH, DS4NOATK: no alternate tracks */
    /*
     * DS4VTOCI: nonindexed VTOC.  The format-5 DSCB describes all free
     * space; on a volume above 65,535 tracks the systems keep free space
     * in format-7 DSCBs instead, so the format-5 contents are marked for
     * the system to rebuild.
     */
    d[14] = (uint64_t)p->cylinders * CKD_INIT_HEADS > 0xffff ? DS4DOSBT : 0;
    d[15] = 1;                                    /* DS4NOEXT */
    d[16] = 0;                                    /* DS4SMSFG: non-SMS */
    d[17] = 0;                                    /* DS4DEVAC */
    stw_be_p(d + 18, p->cylinders);               /* DS4DSCYL */
    stw_be_p(d + 20, CKD_INIT_HEADS);             /* DS4DSTRK */
    stw_be_p(d + 22, CKD_3390_TRACK_LENGTH);      /* DS4DEVTK */
    /* DS4DEVI, DS4DEVL, DS4DEVK, DS4DEVTL: 0 for the 3390 */
    d[27] = CKD_3390_DEVFG;                       /* DS4DEVFG */
    d[30] = CKD_INIT_DSCBS_PER_TRACK;             /* DS4DEVDT */
    d[31] = CKD_3390_DIR_BLOCKS;                  /* DS4DEVDB */
    /* DS4VTOCE: extent type 1, sequence 0, lower and upper CCHH */
    d[61] = 0x01;
    d[62] = 0;
    ckd_put_cchh(d + 63, first);
    ckd_put_cchh(d + 67, last);
}

static void ckd_build_f5(const CkdInitParams *p, uint8_t *key, uint8_t *d)
{
    CkdExtent ext[2];
    unsigned n = ckd_free_extents(p, ext), i;

    memset(key, 0, CKD_DSCB_KEY);
    memset(d, 0, CKD_DSCB_DATA);
    memset(key, 0x05, 4);                         /* DS5KEYID */
    d[0] = 0xf5;                                  /* DS5FMTID */
    /* DS5AVEXT and DS5EXTAV in the key, DS5MAVET in the data */
    for (i = 0; i < n; i++) {
        ckd_put_f5_extent(key + 4 + i * 5, &ext[i]);
    }
    /* DS5PTRDS: no further format-5 DSCB */
}

void ckd_init_build_track(const CkdInitParams *p, uint32_t track,
                          uint8_t *buf)
{
    static const uint8_t r0[8];
    uint8_t *pos = buf;

    memset(buf, 0, CKD_INIT_TRACK_SIZE);
    /* home address: flag, cylinder, head */
    pos[0] = 0;
    ckd_put_cchh(pos + 1, track);
    pos += CKD_HA_SIZE;
    pos = ckd_put_record(pos, track, 0, NULL, 0, r0, sizeof(r0));

    if (track == 0) {
        uint8_t key[4], ipl1[24] = { 0 }, ipl2[144] = { 0 }, vol1[80];

        /* IPL1: disabled wait PSW (code X'0F'), NOP CCW ending the IPL */
        stl_be_p(ipl1, 0x000a0000);
        stl_be_p(ipl1 + 4, 0x0000000f);
        stl_be_p(ipl1 + 8, 0x03000000);
        stl_be_p(ipl1 + 12, 0x00000001);
        ckd_put_ebcdic(key, "IPL1", 4);
        pos = ckd_put_record(pos, track, 1, key, 4, ipl1, sizeof(ipl1));

        /* IPL2: seek, search ID equal, TIC, read (the system bootstrap) */
        stl_be_p(ipl2, 0x07003ab8);
        stl_be_p(ipl2 + 4, 0x40000006);
        stl_be_p(ipl2 + 8, 0x31003abe);
        stl_be_p(ipl2 + 12, 0x40000005);
        stl_be_p(ipl2 + 16, 0x08003aa0);
        stl_be_p(ipl2 + 20, 0x00000000);
        stl_be_p(ipl2 + 24, 0x06000000);
        stl_be_p(ipl2 + 28, 0x20000000);
        /* seek address 0:0 at +32 (6 bytes), search argument R4 at +38 */
        ipl2[42] = 4;
        ckd_put_ebcdic(key, "IPL2", 4);
        pos = ckd_put_record(pos, track, 2, key, 4, ipl2, sizeof(ipl2));

        ckd_build_vol1(p, vol1);
        ckd_put_ebcdic(key, "VOL1", 4);
        pos = ckd_put_record(pos, track, 3, key, 4, vol1, sizeof(vol1));
    } else if (track >= ckd_init_vtoc_first(p) &&
               track <= ckd_init_vtoc_last(p)) {
        uint8_t key[CKD_DSCB_KEY], data[CKD_DSCB_DATA];
        unsigned r;

        for (r = 1; r <= CKD_INIT_DSCBS_PER_TRACK; r++) {
            if (track == ckd_init_vtoc_first(p) && r == 1) {
                ckd_build_f4(p, key, data);
            } else if (track == ckd_init_vtoc_first(p) && r == 2) {
                ckd_build_f5(p, key, data);
            } else {
                memset(key, 0, sizeof(key));      /* format 0 */
                memset(data, 0, sizeof(data));
            }
            pos = ckd_put_record(pos, track, r, key, sizeof(key),
                                 data, sizeof(data));
        }
    }
    memset(pos, 0xff, CKD_COUNT_SIZE);            /* end of track */
}
