/*
 * IUCV echo service: a partner for tests of the IUCV transport
 *
 * Two-way messages are answered with their own data. One-way messages
 * are sent back according to their target class: 1 as a one-way message,
 * 2 as a two-way message (whose reply then comes back as a one-way
 * message), 3 makes the service sever the path; others are just taken.

 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/s390x/iucv.h"

#define TYPE_IUCV_ECHO "iucv-echo"

typedef struct IUCVEcho {
    IUCVService parent_obj;
} IUCVEcho;

typedef struct IUCVEchoClass {
    IUCVServiceClass parent_class;
} IUCVEchoClass;

static int echo_connect(IUCVService *svc, IUCVPath *path, const uint8_t *user)
{
    iucv_path_set_user(path, user);
    return 0;
}

static void echo_message(IUCVService *svc, IUCVPath *path, IUCVMessage *msg)
{
    if (msg->two_way) {
        iucv_reply(svc, path, msg, msg->data, msg->len);
        return;
    }
    switch (msg->trgcls) {
    case 1:
        iucv_send(svc, path, msg->trgcls, msg->data, msg->len, false, 0);
        break;
    case 2:
        iucv_send(svc, path, msg->trgcls, msg->data, msg->len, true,
                  msg->len);
        break;
    case 3:
        iucv_sever(svc, path, NULL);
        break;
    }
}

static void echo_replied(IUCVService *svc, IUCVPath *path, uint32_t msgid,
                         const uint8_t *data, uint32_t len)
{
    iucv_send(svc, path, 4, data, len, false, 0);
}

static void echo_class_init(ObjectClass *oc, const void *data)
{
    IUCVServiceClass *sc = IUCV_SERVICE_CLASS(oc);

    sc->connect = echo_connect;
    sc->message = echo_message;
    sc->replied = echo_replied;
}

static const TypeInfo echo_info = {
    .name = TYPE_IUCV_ECHO,
    .parent = TYPE_IUCV_SERVICE,
    .instance_size = sizeof(IUCVEcho),
    .class_size = sizeof(IUCVEchoClass),
    .class_init = echo_class_init,
};

static void echo_register_types(void)
{
    type_register_static(&echo_info);
}

type_init(echo_register_types)
