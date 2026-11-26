/*
 * Inter-user communication vehicle (IUCV) provided by QEMU
 *
 * QEMU plays the part of the hypervisor (z/VM's CP): it executes the IUCV
 * instruction of the guest and presents its external interruptions. The
 * communication partners of the guest are services inside QEMU (for
 * instance a TCP/IP service or a point-to-point IP link). The core is the
 * transport; each service implements a protocol on top of it.

 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_IUCV_H
#define HW_S390X_IUCV_H

#include "hw/core/qdev.h"
#include "qom/object.h"
#include "qemu/queue.h"

#define TYPE_S390_IUCV "s390-iucv"
OBJECT_DECLARE_SIMPLE_TYPE(S390IUCVState, S390_IUCV)

#define TYPE_IUCV_SERVICE "iucv-service"
OBJECT_DECLARE_TYPE(IUCVService, IUCVServiceClass, IUCV_SERVICE)

typedef struct IUCVPath IUCVPath;

/* a message of the guest, as its service sees it */
typedef struct IUCVMessage {
    uint32_t id;
    uint32_t trgcls;            /* target class */
    bool two_way;               /* a reply is expected */
    bool priority;
    bool prmdata;               /* the data came in the parameter list */
    uint8_t *data;
    uint32_t len;
    uint32_t reply_max;         /* size of the guest's answer area */
} IUCVMessage;

/* return codes (IPRCODE) a service can use to refuse a connection */
#define IUCV_RC_NO_PARTNER      0x0b    /* partner not logged on */
#define IUCV_RC_NO_PATHS        0x0e    /* partner has no paths left */
#define IUCV_RC_NOT_AUTHORIZED  0x0f    /* not authorized */

struct IUCVServiceClass {
    DeviceClass parent_class;

    /*
     * The guest connects to the service. @user holds the 16 bytes of user
     * data. Return 0 to accept (the path can be used at once), or an
     * IUCV_RC_ code. The service may lower the message limit with
     * iucv_path_set_msglim() and set the user data of the completion with
     * iucv_path_set_user().
     */
    int (*connect)(IUCVService *svc, IUCVPath *path, const uint8_t *user);
    /*
     * A message of the guest. A one-way message is complete when the
     * callback returns. A two-way message waits for iucv_reply() or
     * iucv_reject(); @msg stays valid until then or until cancelled().
     */
    void (*message)(IUCVService *svc, IUCVPath *path, IUCVMessage *msg);
    /* the guest purged a two-way message the service has not answered */
    void (*cancelled)(IUCVService *svc, IUCVPath *path, IUCVMessage *msg);
    /* the guest received a one-way message of the service completely */
    void (*received)(IUCVService *svc, IUCVPath *path, uint32_t msgid);
    /* the guest replied to a two-way message of the service */
    void (*replied)(IUCVService *svc, IUCVPath *path, uint32_t msgid,
                    const uint8_t *data, uint32_t len);
    /* the guest rejected a message of the service */
    void (*rejected)(IUCVService *svc, IUCVPath *path, uint32_t msgid);
    /* the guest quiesced (true) or resumed (false) the path */
    void (*quiesced)(IUCVService *svc, IUCVPath *path, bool quiesced);
    /*
     * The path is gone: the guest severed it or the machine was reset.
     * Messages of the guest the service still holds are freed afterwards.
     */
    void (*severed)(IUCVService *svc, IUCVPath *path);
};

struct IUCVService {
    DeviceState parent_obj;

    char *name;                 /* the user ID the guest connects to */
    uint8_t userid[8];          /* @name in EBCDIC, padded with blanks */
    S390IUCVState *iucv;
    QTAILQ_ENTRY(IUCVService) next;
};

/* paths */
uint16_t iucv_path_id(IUCVPath *path);
void *iucv_path_get_opaque(IUCVPath *path);
void iucv_path_set_opaque(IUCVPath *path, void *opaque);
uint16_t iucv_path_msglim(IUCVPath *path);
void iucv_path_set_msglim(IUCVPath *path, uint16_t msglim);
void iucv_path_set_user(IUCVPath *path, const uint8_t *user);
/* the service severs the path; @user is NULL or 16 bytes */
void iucv_sever(IUCVService *svc, IUCVPath *path, const uint8_t *user);

/*
 * A message of the service for the guest; @data is copied. With @two_way
 * the guest answers with REPLY (replied()), and @reply_max is the reply
 * size the guest is told about. Returns the message ID, or 0 if the path
 * cannot take a message now (message limit, quiesced or severed); the
 * service tries again after received(), replied(), rejected() or
 * quiesced(false).
 */
uint32_t iucv_send(IUCVService *svc, IUCVPath *path, uint32_t trgcls,
                   const void *data, uint32_t len, bool two_way,
                   uint32_t reply_max);
/* messages of the service on @path that the guest has not finished */
unsigned iucv_send_outstanding(IUCVPath *path);

/* the service answers a two-way message of the guest; frees @msg */
void iucv_reply(IUCVService *svc, IUCVPath *path, IUCVMessage *msg,
                const void *data, uint32_t len);
void iucv_reject(IUCVService *svc, IUCVPath *path, IUCVMessage *msg);

/* the user ID of the guest (s390-iucv userid=, else the machine's) */
const char *s390_iucv_userid(void);

/* user IDs in EBCDIC, blank padded */
void iucv_userid_from_str(uint8_t *userid, const char *str);

#endif
