/*
 * QTest testcase for the CPU identification (STORE CPU ID) under KVM
 *
 * A tiny guest program stores its CPU ID and sets a flag; the test compares
 * the stored doubleword with what the host itself reports.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"

#define STIDP_CODE  0x200
#define STIDP_DATA  0x400
#define STIDP_DONE  0x408
#define STIDP_IMAGE 0x1000

typedef struct HostCpuId {
    uint8_t version;
    uint32_t ident;
    uint16_t machine;
} HostCpuId;

static char *create_image(void)
{
    static const uint8_t code[] = {
        0xb2, 0x02, 0x04, 0x00,     /* 200 stidp DATA          */
        0x92, 0x01, 0x04, 0x08,     /* 204 mvi   DONE,1        */
        0xa7, 0xf4, 0x00, 0x00,     /* 208 j     208           */
    };
    g_autofree uint8_t *image = g_malloc0(STIDP_IMAGE);
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-stidp-XXXXXX.img", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    /* Short IPL PSW: 31-bit addressing, start at STIDP_CODE. */
    stl_be_p(image, 0x00080000);
    stl_be_p(image + 4, 0x80000000 | STIDP_CODE);
    memcpy(image + STIDP_CODE, code, sizeof(code));
    g_assert_cmpint(write(fd, image, STIDP_IMAGE), ==, STIDP_IMAGE);
    close(fd);
    return path;
}

/* The CPU ID of the host, as its kernel shows it in /proc/cpuinfo. */
static bool host_cpu_id(HostCpuId *id)
{
    g_autofree char *contents = NULL;
    unsigned int version, ident, machine;
    const char *line;

    if (!g_file_get_contents("/proc/cpuinfo", &contents, NULL, NULL)) {
        return false;
    }
    line = strstr(contents, "processor 0: ");
    if (!line || sscanf(line, "processor 0: version = %x, identification = "
                        "%x, machine = %x", &version, &ident, &machine) != 3) {
        return false;
    }
    id->version = version;
    id->ident = ident;
    id->machine = machine;
    return true;
}

/* Run the guest program and return the doubleword it stored. */
static uint64_t guest_stidp(const char *machine, const char *cpu)
{
    g_autofree char *image = create_image();
    QTestState *qts;
    uint64_t cpuid;
    int i;

    qts = qtest_initf("-nodefaults -accel kvm -machine %s -cpu %s -m 1G "
                      "-kernel %s", machine, cpu, image);
    for (i = 0; i < 200000 && !qtest_readb(qts, STIDP_DONE); i++) {
        g_usleep(50);
    }
    g_assert_cmpint(qtest_readb(qts, STIDP_DONE), ==, 1);
    cpuid = qtest_readq(qts, STIDP_DATA);
    qtest_quit(qts);
    unlink(image);
    return cpuid;
}

/* The static base model of the host CPU, e.g. "z14ZR1-base". */
static char *host_base_model(void)
{
    QTestState *qts = qtest_init("-nodefaults -accel kvm -machine none");
    QDict *resp, *ret, *model;
    char *name;

    resp = qtest_qmp(qts, "{ 'execute': 'query-cpu-model-expansion', "
                     "'arguments': { 'type': 'static', "
                     "'model': { 'name': 'host' } } }");
    g_assert(qdict_haskey(resp, "return"));
    ret = qdict_get_qdict(resp, "return");
    model = qdict_get_qdict(ret, "model");
    name = g_strdup(qdict_get_str(model, "name"));
    qobject_unref(resp);
    qtest_quit(qts);
    return name;
}

static void check_cpuid(uint64_t cpuid, uint8_t version, const HostCpuId *host)
{
    g_test_message("STIDP %016" PRIx64, cpuid);
    g_assert_cmphex(cpuid >> 56, ==, version);
    g_assert_cmphex((cpuid >> 32) & 0xffffff, ==, host->ident);
    g_assert_cmphex((cpuid >> 16) & 0xffff, ==, host->machine);
}

/* -cpu host on the current machine reports the host's CPU version. */
static void test_host_model(const void *data)
{
    const char *machine = data;
    HostCpuId host;

    g_assert(host_cpu_id(&host));
    check_cpuid(guest_stidp(machine, "host"), host.version, &host);
}

/* Named (migration-safe) models report version 0 on current machines. */
static void test_named_model(void)
{
    g_autofree char *model = host_base_model();
    HostCpuId host;

    g_assert(host_cpu_id(&host));
    check_cpuid(guest_stidp("s390-ccw-virtio", model), 0, &host);
}

/* Machine types 11.1 report version 0 for every model. */
static void test_machine_11_1(void)
{
    HostCpuId host;

    g_assert(host_cpu_id(&host));
    check_cpuid(guest_stidp("s390-ccw-virtio-11.1", "host"), 0, &host);
}

/* Machine types up to 11.0 keep the KVM default of version X'FF'. */
static void test_machine_11_0(void)
{
    HostCpuId host;

    g_assert(host_cpu_id(&host));
    check_cpuid(guest_stidp("s390-ccw-virtio-11.0", "host"), 0xff, &host);
}

int main(int argc, char **argv)
{
    HostCpuId host;

    g_test_init(&argc, &argv, NULL);
    /* The tests compare with the host: they run under KVM only. */
    if (!qtest_has_accel("kvm") || access("/dev/kvm", R_OK | W_OK) ||
        !host_cpu_id(&host)) {
        return g_test_run();
    }
    qtest_add_data_func("/s390x/stidp/host-model", "s390-ccw-virtio",
                        test_host_model);
    if (qtest_has_machine("czam")) {
        qtest_add_data_func("/s390x/stidp/czam-host-model", "czam",
                            test_host_model);
    }
    qtest_add_func("/s390x/stidp/named-model", test_named_model);
    qtest_add_func("/s390x/stidp/machine-11.1", test_machine_11_1);
    qtest_add_func("/s390x/stidp/machine-11.0", test_machine_11_0);
    return g_test_run();
}
