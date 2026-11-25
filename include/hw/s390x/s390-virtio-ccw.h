/*
 * virtio ccw machine definitions
 *
 * Copyright 2012, 2016 IBM Corp.
 * Author(s): Cornelia Huck <cornelia.huck@de.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */
#ifndef HW_S390X_S390_VIRTIO_CCW_H
#define HW_S390X_S390_VIRTIO_CCW_H

#include "hw/core/boards.h"
#include "qom/object.h"
#include "qemu/notify.h"
#include "hw/s390x/sclp.h"
#include "hw/s390x/css.h"
#include "qapi/qapi-types-machine-s390x.h"

#define TYPE_S390_CCW_MACHINE               "s390-ccw-machine"
/* Abstract base of the mainframe machine types (czam) */
#define TYPE_S390_MAINFRAME_MACHINE         MACHINE_TYPE_NAME("s390-mainframe")

OBJECT_DECLARE_TYPE(S390CcwMachineState, S390CcwMachineClass, S390_CCW_MACHINE)


struct S390CcwMachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    bool aes_key_wrap;
    bool dea_key_wrap;
    bool esa390_cm;
    bool pv;
    bool secure_boot;
    uint8_t loadparm[8];
    CssDevId ipl_devno;
    bool ipl_devno_set;
    bool ipl_devno_full;
    DeviceState *ipl_device;
    Notifier ipl_notifier;
    uint64_t memory_limit;
    uint64_t max_pagesize;
    BootCertificatesList *boot_certs;
    bool ext_measurement_block;
    bool chsc_config_token_set;
    uint8_t chsc_config_token[64];

    /* Host-mirrored LPAR personality (mainframe machine types: czam) */
    char *lpar_name;
    uint16_t lpar_number;
    uint64_t storage_increment;
    uint32_t host_capability;

    SCLPDevice *sclp;
};

static inline uint64_t s390_get_memory_limit(S390CcwMachineState *s390ms)
{
    /* We expect to be called only after the limit was set. */
    assert(s390ms->memory_limit);
    return s390ms->memory_limit;
}

#define S390_PTF_REASON_NONE (0x00 << 8)
#define S390_PTF_REASON_DONE (0x01 << 8)
#define S390_PTF_REASON_BUSY (0x02 << 8)
#define S390_TOPO_FC_MASK 0xffUL
void s390_handle_ptf(S390CPU *cpu, uint8_t r1, uintptr_t ra);

/*
 * Host-mirrored LPAR personality of the mainframe machine types.
 * s390_mainframe_lpar_identity() returns false for machines without one;
 * otherwise it provides the partition number and the blank-padded EBCDIC
 * partition name to report in SYSIB 2.2.2.
 * s390_mainframe_storage_increment() returns the SCLP storage increment in
 * bytes, or 0 to keep the default.
 */
bool s390_mainframe_lpar_identity(uint16_t *number, uint8_t name_ebcdic[8]);
/*
 * Whether the channel subsystem has the extended-I/O-measurement-block
 * facility (machine property "ext-measurement-block"): general
 * characteristic 48 of CHSC store channel-subsystem characteristics, the
 * measurement-block-format control and the measurement-block address of
 * the subchannels.
 */
bool s390_css_ext_measurement_block(void);
uint64_t s390_mainframe_storage_increment(void);
/* LPAR-personality overlays for SCLP Read SCP Info and CHSC (see .c) */
void s390_mainframe_patch_read_info(uint8_t *read_info);
bool s390_mainframe_chsc_characteristics(uint32_t *general_chars,
                                         uint32_t *chsc_chars);
/* The machine performs the z/OS STSI compatibility handshake (kvm.c). */
bool s390_mainframe_zos_compat(void);

struct S390CcwMachineClass {
    /*< private >*/
    MachineClass parent_class;

    /*< public >*/
    int max_threads;
    bool use_cpi;
    bool use_certs;
    bool use_secure;
    /* Machine type of a host-mirrored LPAR personality, 0 if none */
    uint16_t mirror_type;
    /* z/OS STSI compatibility handshake (see kvm_zos_compat_stsi()) */
    bool zos_compat;
    /*
     * Map -serial #0 to the line-mode console ("Operating System
     * Messages") and #1 to the integrated ASCII console, as an LPAR's
     * hardware console presents them.
     */
    bool line_mode_console_first;
};

#endif
