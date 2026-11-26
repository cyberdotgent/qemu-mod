/*
 * QTest testcase for the s390x machine property tod-source
 *
 * tod-source=machine (default) leaves the guest TOD clock at the TOD clock
 * of the host machine (KVM epoch difference 0); tod-source=host sets it
 * once from the host's CLOCK_REALTIME when the guest first starts.
 *
 * The property tests run with the qtest accelerator.  The clock tests need
 * KVM: a two-instruction guest stores its TOD clock with STORE CLOCK
 * EXTENDED in a loop and the test compares the value with the host's
 * real time and, on an s390x host, with the host's own TOD clock.
 * They are skipped without KVM; the clock comparison is meaningless with
 * an emulated TOD clock.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

/* The TOD clock value of 1970-01-01 00:00:00 UTC */
#define TOD_UNIX_EPOCH  0x7d91048bca000000ULL
/* TOD clock units: bit 51 is one microsecond */
#define TOD_PER_US      4096ULL
#define TOD_PER_SEC     (TOD_PER_US * 1000000ULL)

#define G_CODE  0x200
#define G_TOD   0xc00
#define G_IMAGE 0x1000

static const char *machines[] = {
    "s390-ccw-virtio", "czam", "czam-v1", "czam-compat", "czam-compat-v1",
};

static char *get_tod_source(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                           "{ 'path': '/machine', 'property': 'tod-source' } }");
    char *val;

    g_assert(qdict_haskey(rsp, "return"));
    val = g_strdup(qdict_get_str(rsp, "return"));
    qobject_unref(rsp);
    return val;
}

static void test_property_values(void)
{
    QTestState *qts;
    QDict *rsp;
    g_autofree char *def = NULL;
    g_autofree char *host = NULL;
    g_autofree char *machine = NULL;

    qts = qtest_init("-machine s390-ccw-virtio -accel qtest");
    def = get_tod_source(qts);
    g_assert_cmpstr(def, ==, "machine");
    /* Unknown values are refused */
    rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': "
                    "{ 'path': '/machine', 'property': 'tod-source', "
                    "'value': 'utc' } }");
    g_assert(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    qtest_quit(qts);

    qts = qtest_init("-machine s390-ccw-virtio,tod-source=host -accel qtest");
    host = get_tod_source(qts);
    g_assert_cmpstr(host, ==, "host");
    qtest_quit(qts);

    qts = qtest_init("-machine s390-ccw-virtio,tod-source=machine "
                     "-accel qtest");
    machine = get_tod_source(qts);
    g_assert_cmpstr(machine, ==, "machine");
    qtest_quit(qts);
}

/* Every machine type lists the property, as an S390TodSource enum. */
static void test_property_listed(void)
{
    QTestState *qts = qtest_init("-machine none -accel qtest");
    int i;

    for (i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree char *type = NULL;
        QDict *rsp;
        QListEntry *e;
        bool found = false;

        if (!qtest_has_machine(machines[i])) {
            continue;
        }
        rsp = qtest_qmp(qts, "{ 'execute': 'qom-list-properties', "
                        "'arguments': { 'typename': %s } }",
                        g_str_has_prefix(machines[i], "s390-ccw-virtio") ?
                        "s390-ccw-machine" :
                        (type = g_strdup_printf("%s-machine", machines[i])));
        if (!qdict_haskey(rsp, "return")) {
            /* aliases have no type of their own */
            qobject_unref(rsp);
            continue;
        }
        QLIST_FOREACH_ENTRY(qdict_get_qlist(rsp, "return"), e) {
            QDict *prop = qobject_to(QDict, qlist_entry_obj(e));

            if (g_str_equal(qdict_get_str(prop, "name"), "tod-source")) {
                g_assert_cmpstr(qdict_get_str(prop, "type"), ==,
                                "S390TodSource");
                found = true;
            }
        }
        g_assert(found);
        qobject_unref(rsp);
    }
    qtest_quit(qts);
}

static char *create_guest_image(void)
{
    static const uint8_t code[] = {
        0xb2, 0x78, 0x0c, 0x00,     /* 200 stcke TOD  */
        0xa7, 0xf4, 0xff, 0xfe,     /* 204 j     200  */
    };
    g_autofree uint8_t *image = g_malloc0(G_IMAGE);
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-s390x-tod-XXXXXX.img", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    /* Short IPL PSW: 31-bit addressing, start at G_CODE. */
    stl_be_p(image, 0x00080000);
    stl_be_p(image + 4, 0x80000000 | G_CODE);
    memcpy(image + G_CODE, code, sizeof(code));
    g_assert_cmpint(write(fd, image, G_IMAGE), ==, G_IMAGE);
    close(fd);
    return path;
}

/* The guest TOD clock: epoch index and bits 0-63 */
static void read_guest_tod(QTestState *qts, uint8_t *epoch, uint64_t *tod)
{
    uint8_t buf[16];
    int i;

    for (i = 0; i < 100000; i++) {
        qtest_memread(qts, G_TOD, buf, sizeof(buf));
        if (ldq_be_p(buf + 1)) {
            *epoch = buf[0];
            *tod = ldq_be_p(buf + 1);
            return;
        }
        g_usleep(100);
    }
    g_assert_not_reached();
}

static uint64_t realtime_tod(void)
{
    return TOD_UNIX_EPOCH + (uint64_t)g_get_real_time() * TOD_PER_US;
}

#ifdef __s390x__
static uint64_t host_machine_tod(void)
{
    uint64_t tod;

    asm volatile("stck %0" : "=Q"(tod) : : "cc");
    return tod;
}
#endif

static double tod_diff_sec(uint64_t a, uint64_t b)
{
    return (double)(int64_t)(a - b) / TOD_PER_SEC;
}

static QTestState *start_guest(const char *machine, const char *opts,
                               char **image)
{
    *image = create_guest_image();
    /*
     * czam-compat starts in the ESA/390-compatibility mode by default, which
     * needs host kernel support; the TOD clock does not depend on it.
     */
    return qtest_initf("-nodefaults -accel kvm -machine %s%s%s -m %s "
                       "-kernel %s", machine,
                       g_str_has_prefix(machine, "czam-compat") ?
                       ",esa390-cm=off,esa390-dat=off" : "", opts,
                       g_str_has_prefix(machine, "czam") ? "1G" : "64M",
                       *image);
}

static void stop_guest(QTestState *qts, char *image)
{
    qtest_quit(qts);
    unlink(image);
    g_free(image);
}

static void test_kvm_host(const void *data)
{
    const char *machine = data;
    g_autofree char *image = NULL;
    QTestState *qts = start_guest(machine, ",tod-source=host", &image);
    uint8_t epoch;
    uint64_t tod;
    double diff;

    read_guest_tod(qts, &epoch, &tod);
    diff = tod_diff_sec(tod, realtime_tod());
    g_test_message("%s tod-source=host: guest TOD %02x:%016" PRIx64
                   ", guest - host CLOCK_REALTIME = %.6f s",
                   machine, epoch, tod, diff);
    g_assert_cmpint(epoch, ==, 0);
    g_assert_cmpfloat(fabs(diff), <, 1.0);

    /* system_reset keeps the clock running: it is not set again */
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    qtest_memset(qts, G_TOD, 0, 16);
    read_guest_tod(qts, &epoch, &tod);
    diff = tod_diff_sec(tod, realtime_tod());
    g_test_message("%s after system_reset: guest - host CLOCK_REALTIME = "
                   "%.6f s", machine, diff);
    g_assert_cmpfloat(fabs(diff), <, 1.0);
    stop_guest(qts, g_steal_pointer(&image));
}

static void test_kvm_machine(const void *data)
{
    const char *machine = data;
    g_autofree char *image = NULL;
    QTestState *qts = start_guest(machine, "", &image);
    uint8_t epoch;
    uint64_t tod;

    read_guest_tod(qts, &epoch, &tod);
    g_test_message("%s tod-source=machine: guest TOD %02x:%016" PRIx64
                   ", guest - host CLOCK_REALTIME = %.6f s",
                   machine, epoch, tod, tod_diff_sec(tod, realtime_tod()));
#ifdef __s390x__
    {
        double diff = tod_diff_sec(tod, host_machine_tod());

        g_test_message("%s tod-source=machine: guest - host TOD clock = "
                       "%.6f s", machine, diff);
        g_assert_cmpfloat(fabs(diff), <, 1.0);
    }
#endif
    stop_guest(qts, g_steal_pointer(&image));
}

int main(int argc, char **argv)
{
    int i;

    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("qtest")) {
        qtest_add_func("/s390x/tod-source/values", test_property_values);
        qtest_add_func("/s390x/tod-source/listed", test_property_listed);
    }
    /* Never TCG: an emulated TOD clock is always derived from host time. */
    if (qtest_has_accel("kvm")) {
        for (i = 0; i < ARRAY_SIZE(machines); i++) {
            g_autofree char *p1 = NULL;
            g_autofree char *p2 = NULL;

            if (!qtest_has_machine(machines[i]) ||
                g_str_has_suffix(machines[i], "-v1")) {
                continue;
            }
            p1 = g_strdup_printf("/s390x/tod-source/kvm/%s/host",
                                 machines[i]);
            p2 = g_strdup_printf("/s390x/tod-source/kvm/%s/machine",
                                 machines[i]);
            qtest_add_data_func(p1, machines[i], test_kvm_host);
            qtest_add_data_func(p2, machines[i], test_kvm_machine);
        }
    }
    return g_test_run();
}
