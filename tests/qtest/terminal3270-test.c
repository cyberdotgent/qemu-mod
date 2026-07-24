/*
 * QTest testcase for emulated 3270 CCW terminals
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/sockets.h"
#include "hw/s390x/ebcdic.h"
#include "libqtest.h"

#define TERMINAL_PATH "/machine/peripheral/dev3270-0"
#define TN_IAC 0xff

static const uint8_t tn3270_offer[] = {
    TN_IAC, 0xfd, 0x19, TN_IAC, 0xfb, 0x19,
    TN_IAC, 0xfd, 0x00, TN_IAC, 0xfb, 0x00,
    TN_IAC, 0xfd, 0x18,
    TN_IAC, 0xfa, 0x18, 0x01, TN_IAC, 0xf0,
};

static const uint8_t tn3270_answer[] = {
    TN_IAC, 0xfb, 0x19, TN_IAC, 0xfd, 0x19,
    TN_IAC, 0xfb, 0x00, TN_IAC, 0xfd, 0x00,
    TN_IAC, 0xfb, 0x18,
    TN_IAC, 0xfa, 0x18, 0x00,
    'I', 'B', 'M', '-', '3', '2', '7', '8', '-', '2', '-', 'E',
    TN_IAC, 0xf0,
};

static int get_free_port(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    g_assert_cmpint(getsockname(fd, (struct sockaddr *)&addr, &len), ==, 0);
    close(fd);
    return ntohs(addr.sin_port);
}

static int connect_terminal(int port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    return fd;
}

static void socket_read_all(int fd, uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t ret = recv(fd, buf + done, len - done, 0);

        g_assert_cmpint(ret, >, 0);
        done += ret;
    }
}

static void socket_write_fragmented(int fd, const uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        g_assert_cmpint(send(fd, buf + i, 1, 0), ==, 1);
    }
}

static GByteArray *socket_read_record(int fd)
{
    GByteArray *record = g_byte_array_new();

    while (record->len < 65537) {
        uint8_t byte;

        socket_read_all(fd, &byte, 1);
        g_byte_array_append(record, &byte, 1);
        if (record->len >= 2 && record->data[record->len - 2] == TN_IAC &&
            record->data[record->len - 1] == 0xef) {
            return record;
        }
    }
    g_assert_not_reached();
}

static int64_t qom_get_int(QTestState *qts, const char *path,
                           const char *property)
{
    QDict *response;
    int64_t value;

    response = qtest_qmp(
        qts,
        "{'execute': 'qom-get', 'arguments': "
        " {'path': %s, 'property': %s}}",
        path, property);
    g_assert(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static void wait_for_bool(QTestState *qts, const char *property, bool value)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (qtest_qom_get_bool(qts, TERMINAL_PATH, property) != value) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void wait_for_int(QTestState *qts, const char *property, int64_t value)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (qom_get_int(qts, TERMINAL_PATH, property) != value) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void negotiate_terminal(QTestState *qts, int fd)
{
    uint8_t received[sizeof(tn3270_offer)];

    socket_read_all(fd, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received),
                    tn3270_offer, sizeof(tn3270_offer));
    wait_for_bool(qts, "connected", true);
    socket_write_fragmented(fd, tn3270_answer, sizeof(tn3270_answer));
    wait_for_bool(qts, "ready", true);
}

static void negotiate_terminal_type(QTestState *qts, int fd, const char *type)
{
    g_autoptr(GByteArray) response = g_byte_array_new();
    const uint8_t prefix[] = { TN_IAC, 0xfa, 0x18, 0x00 };
    const uint8_t suffix[] = { TN_IAC, 0xf0 };
    uint8_t received[sizeof(tn3270_offer)];

    socket_read_all(fd, received, sizeof(received));
    socket_write_fragmented(fd, tn3270_answer, 15);
    g_byte_array_append(response, prefix, sizeof(prefix));
    g_byte_array_append(response, (const uint8_t *)type, strlen(type));
    g_byte_array_append(response, suffix, sizeof(suffix));
    socket_write_fragmented(fd, response->data, response->len);
    wait_for_bool(qts, "ready", true);
}

static void wait_for_migration(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    for (;;) {
        QDict *response = qtest_qmp(qts, "{'execute': 'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            qobject_unref(response);
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(response);
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static char *qom_get_string(QTestState *qts, const char *path,
                            const char *property)
{
    QDict *response;
    char *value;

    response = qtest_qmp(
        qts,
        "{'execute': 'qom-get', 'arguments': "
        " {'path': %s, 'property': %s}}",
        path, property);
    g_assert(qdict_haskey(response, "return"));
    value = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);
    return value;
}

static void test_multiple_terminals(void)
{
    QTestState *qts;
    g_autofree char *chardev0 = NULL;
    g_autofree char *chardev1 = NULL;
    g_autofree char *devid0 = NULL;
    g_autofree char *devid1 = NULL;

    qts = qtest_init(
        "-nodefaults "
        "-chardev ringbuf,id=tn0 "
        "-chardev ringbuf,id=tn1 "
        "-device x-terminal3270,id=term0,chardev=tn0,devno=fe.0.000a "
        "-device x-terminal3270,id=term1,chardev=tn1,devno=fe.0.000b");

    chardev0 = qom_get_string(qts, "/machine/peripheral/term0", "chardev");
    chardev1 = qom_get_string(qts, "/machine/peripheral/term1", "chardev");
    devid0 = qom_get_string(qts, "/machine/peripheral/term0", "dev_id");
    devid1 = qom_get_string(qts, "/machine/peripheral/term1", "dev_id");

    g_assert_cmpstr(chardev0, ==, "tn0");
    g_assert_cmpstr(chardev1, ==, "tn1");
    g_assert_cmpstr(devid0, ==, "fe.0.000a");
    g_assert_cmpstr(devid1, ==, "fe.0.000b");

    qtest_quit(qts);
}

static void test_dev3270_shortcut(void)
{
    QTestState *qts;
    g_autofree char *chardev0 = NULL;
    g_autofree char *chardev1 = NULL;
    g_autofree char *devid0 = NULL;
    g_autofree char *devid1 = NULL;

    qts = qtest_init(
        "-nodefaults "
        "-dev3270 port=0,devno=300 "
        "-dev3270 port=0,devno=0301");

    chardev0 = qom_get_string(qts, "/machine/peripheral/dev3270-0",
                              "chardev");
    chardev1 = qom_get_string(qts, "/machine/peripheral/dev3270-1",
                              "chardev");
    devid0 = qom_get_string(qts, "/machine/peripheral/dev3270-0", "dev_id");
    devid1 = qom_get_string(qts, "/machine/peripheral/dev3270-1", "dev_id");

    g_assert_cmpstr(chardev0, ==, "dev3270-chardev0");
    g_assert_cmpstr(chardev1, ==, "dev3270-chardev1");
    g_assert_cmpstr(devid0, ==, "fe.0.0300");
    g_assert_cmpstr(devid1, ==, "fe.0.0301");

    qtest_quit(qts);
}

static void test_auto_devnos(void)
{
    g_autofree char *devid0 = NULL;
    g_autofree char *devid1 = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults -dev3270 port=0 -dev3270 port=0");
    devid0 = qom_get_string(qts, "/machine/peripheral/dev3270-0", "dev_id");
    devid1 = qom_get_string(qts, "/machine/peripheral/dev3270-1", "dev_id");
    g_assert_cmpstr(devid0, ==, "fe.0.0700");
    g_assert_cmpstr(devid1, ==, "fe.0.0701");
    qtest_quit(qts);
}

/*
 * A CSS has only 256 CHPIDs, one of which is reserved for virtio-ccw. If each
 * terminal consumed a separate channel path, the last device here would fail
 * to realize. Emulated terminals of the same type must share a virtual path.
 */
static void test_many_terminals_share_chpid(void)
{
    g_autoptr(GString) args = g_string_new("-nodefaults");
    QTestState *qts;
    unsigned int i;

    for (i = 0; i < 256; i++) {
        g_string_append_printf(args,
                               " -device x-terminal3270,id=term%u,"
                               "devno=fe.0.%04x",
                               i, 0x100 + i);
    }

    qts = qtest_init(args->str);
    qtest_quit(qts);
}

static void test_tn3270_negotiation_and_records(void)
{
    static const uint8_t records[] = {
        0x7d, 0x40, 0x40, 0xc1, TN_IAC, TN_IAC, TN_IAC, 0xef,
        0x6d, 0x40, 0x40, 0xc2, TN_IAC, 0xef,
    };
    g_autofree char *type = NULL;
    g_autoptr(GByteArray) banner = NULL;
    uint8_t qemu_ebcdic[4];
    QTestState *qts;
    int port = get_free_port();
    int fd;

    qts = qtest_initf("-nodefaults -dev3270 port=%d,devno=fe.0.000a",
                      port);
    fd = connect_terminal(port);
    g_assert_false(qtest_qom_get_bool(qts, TERMINAL_PATH, "ready"));
    negotiate_terminal(qts, fd);

    banner = socket_read_record(fd);
    ebcdic_put(qemu_ebcdic, "QEMU", sizeof(qemu_ebcdic));
    g_assert_cmpuint(banner->len, >, 4);
    g_assert_cmphex(banner->data[0], ==, 0xf5);
    g_assert_cmphex(banner->data[1], ==, 0x42);
    g_assert_nonnull(memmem(banner->data, banner->len,
                           qemu_ebcdic, sizeof(qemu_ebcdic)));

    type = qom_get_string(qts, TERMINAL_PATH, "terminal-type");
    g_assert_cmpstr(type, ==, "IBM-3278-2-E");
    g_assert_cmpint(qom_get_int(qts, TERMINAL_PATH, "model"), ==, 2);
    g_assert_cmpint(qom_get_int(qts, TERMINAL_PATH, "rows"), ==, 24);
    g_assert_cmpint(qom_get_int(qts, TERMINAL_PATH, "columns"), ==, 80);
    g_assert_true(qtest_qom_get_bool(qts, TERMINAL_PATH,
                                     "extended-attributes"));

    /* Both EOR-delimited records arrive in one host write; doubled IAC is data. */
    g_assert_cmpint(send(fd, records, sizeof(records), 0), ==,
                    sizeof(records));
    wait_for_int(qts, "queued-records", 2);

    close(fd);
    wait_for_bool(qts, "connected", false);
    wait_for_int(qts, "queued-records", 0);
    qtest_quit(qts);
}

static void test_migration_reconnect(void)
{
    static const uint8_t record[] = {
        0x7d, 0x40, 0x40, 0xc1, TN_IAC, 0xef,
    };
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from;
    QTestState *to;
    int from_port;
    int to_port = get_free_port();
    int from_fd;
    int to_fd;

    path = g_strdup_printf("%s/qemu-3270-migrate-%u.sock",
                           g_get_tmp_dir(), g_random_int());
    uri = g_strdup_printf("unix:%s", path);
    unlink(path);

    to = qtest_initf("-nodefaults -incoming defer "
                     "-dev3270 port=%d,devno=fe.0.000a", to_port);
    from_port = get_free_port();
    from = qtest_initf("-nodefaults -dev3270 port=%d,devno=fe.0.000a",
                       from_port);
    from_fd = connect_terminal(from_port);
    negotiate_terminal(from, from_fd);
    g_assert_cmpint(send(from_fd, record, sizeof(record), 0), ==,
                    sizeof(record));
    wait_for_int(from, "queued-records", 1);

    qtest_qmp_assert_success(to,
        "{'execute': 'migrate-incoming', 'arguments': {'uri': %s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute': 'migrate', 'arguments': {'uri': %s}}", uri);
    wait_for_migration(from);
    wait_for_migration(to);
    wait_for_int(to, "queued-records", 1);
    wait_for_bool(to, "connected", false);

    to_fd = connect_terminal(to_port);
    negotiate_terminal(to, to_fd);
    wait_for_int(to, "queued-records", 1);

    close(to_fd);
    close(from_fd);
    qtest_quit(from);
    qtest_quit(to);
    unlink(path);
}

static void test_negotiation_retry_refusal_and_queue_limit(void)
{
    static const uint8_t bad_type[] = {
        TN_IAC, 0xfa, 0x18, 0x00, 'A', 'N', 'S', 'I', TN_IAC, 0xf0,
    };
    static const uint8_t type_request[] = {
        TN_IAC, 0xfa, 0x18, 0x01, TN_IAC, 0xf0,
    };
    static const uint8_t one_record[] = {
        0x7d, 0x40, 0x40, TN_IAC, 0xef,
    };
    uint8_t received[sizeof(tn3270_offer)];
    QTestState *qts;
    int port = get_free_port();
    int fd;
    int i;

    qts = qtest_initf("-nodefaults -dev3270 port=%d,devno=fe.0.000a",
                      port);
    fd = connect_terminal(port);
    socket_read_all(fd, received, sizeof(tn3270_offer));
    socket_write_fragmented(fd, tn3270_answer, 15);
    socket_write_fragmented(fd, bad_type, sizeof(bad_type));
    socket_read_all(fd, received, sizeof(type_request));
    g_assert_cmpmem(received, sizeof(type_request),
                    type_request, sizeof(type_request));
    socket_write_fragmented(fd, tn3270_answer + 15,
                            sizeof(tn3270_answer) - 15);
    wait_for_bool(qts, "ready", true);

    for (i = 0; i <= 16; i++) {
        ssize_t ret = send(fd, one_record, sizeof(one_record), 0);

        if (ret < 0) {
            break;
        }
        g_assert_cmpint(ret, ==, sizeof(one_record));
    }
    wait_for_bool(qts, "connected", false);
    close(fd);

    /* A new session must not inherit an incomplete parser or full queue. */
    fd = connect_terminal(port);
    negotiate_terminal(qts, fd);
    wait_for_int(qts, "queued-records", 0);
    close(fd);
    wait_for_bool(qts, "connected", false);

    /* Refusing a required option terminates the session cleanly. */
    fd = connect_terminal(port);
    socket_read_all(fd, received, sizeof(tn3270_offer));
    socket_write_fragmented(fd,
                            (const uint8_t[]){ TN_IAC, 0xfc, 0x00 }, 3);
    wait_for_bool(qts, "connected", false);
    close(fd);
    qtest_quit(qts);
}

static void test_terminal_models(void)
{
    static const struct {
        const char *type;
        unsigned int model;
        unsigned int rows;
        unsigned int cols;
        bool eab;
    } models[] = {
        { "IBM-DYNAMIC", 0xff, 24, 80, true },
        { "IBM-3179-2", 2, 24, 80, false },
        { "IBM-3180-2", 5, 27, 132, false },
        { "IBM-3277-2", 2, 24, 80, false },
        { "IBM-3278-2", 2, 24, 80, false },
        { "IBM-3278-3", 3, 32, 80, false },
        { "IBM-3278-4", 4, 43, 80, false },
        { "IBM-3278-5", 5, 27, 132, false },
        { "IBM-3278-2-E", 2, 24, 80, true },
        { "IBM-3278-3-E", 3, 32, 80, true },
        { "IBM-3278-4-E", 4, 43, 80, true },
        { "IBM-3278-5-E", 5, 27, 132, true },
        { "IBM-3279-2", 2, 24, 80, false },
        { "IBM-3279-3", 3, 32, 80, false },
        { "IBM-3279-4", 4, 43, 80, false },
        { "IBM-3279-5", 5, 27, 132, false },
        { "IBM-3279-2-E", 2, 24, 80, true },
        { "IBM-3279-3-E", 3, 32, 80, true },
        { "IBM-3279-4-E", 4, 43, 80, true },
        { "IBM-3279-5-E", 5, 27, 132, true },
    };
    QTestState *qts;
    int port = get_free_port();
    unsigned int i;

    qts = qtest_initf("-nodefaults -dev3270 port=%d,devno=fe.0.000a",
                      port);
    for (i = 0; i < ARRAY_SIZE(models); i++) {
        g_autofree char *type = NULL;
        int fd = connect_terminal(port);

        negotiate_terminal_type(qts, fd, models[i].type);
        type = qom_get_string(qts, TERMINAL_PATH, "terminal-type");
        g_assert_cmpstr(type, ==, models[i].type);
        g_assert_cmpint(qom_get_int(qts, TERMINAL_PATH, "model"), ==,
                        models[i].model);
        g_assert_cmpint(qom_get_int(qts, TERMINAL_PATH, "rows"), ==,
                        models[i].rows);
        g_assert_cmpint(qom_get_int(qts, TERMINAL_PATH, "columns"), ==,
                        models[i].cols);
        g_assert_cmpint(qtest_qom_get_bool(qts, TERMINAL_PATH,
                                           "extended-attributes"), ==,
                        models[i].eab);
        close(fd);
        wait_for_bool(qts, "connected", false);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/terminal3270/multiple", test_multiple_terminals);
    qtest_add_func("/terminal3270/dev3270-shortcut",
                   test_dev3270_shortcut);
    qtest_add_func("/terminal3270/auto-devnos", test_auto_devnos);
    qtest_add_func("/terminal3270/shared-chpid",
                   test_many_terminals_share_chpid);
    qtest_add_func("/terminal3270/tn3270-negotiation-records",
                   test_tn3270_negotiation_and_records);
    qtest_add_func("/terminal3270/migration-reconnect",
                   test_migration_reconnect);
    qtest_add_func("/terminal3270/protocol-edge-cases",
                   test_negotiation_retry_refusal_and_queue_limit);
    qtest_add_func("/terminal3270/models", test_terminal_models);

    return g_test_run();
}
