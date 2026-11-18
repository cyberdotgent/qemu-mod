/*
 * QEMU readline: cursor and editing keys
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/readline.h"

typedef struct {
    ReadLineState *rs;
    GString *output;
    char *command;
} TestState;

static void G_GNUC_PRINTF(2, 3) test_printf(void *opaque, const char *fmt,
                                            ...)
{
    TestState *ts = opaque;
    va_list ap;

    va_start(ap, fmt);
    g_string_append_vprintf(ts->output, fmt, ap);
    va_end(ap);
}

static void test_flush(void *opaque)
{
}

static void test_completion(void *opaque, const char *cmdline)
{
}

static void test_command(void *opaque, const char *str, void *readline_opaque)
{
    TestState *ts = opaque;

    g_free(ts->command);
    ts->command = g_strdup(str);
}

static void test_init(TestState *ts)
{
    ts->output = g_string_new(NULL);
    ts->command = NULL;
    ts->rs = readline_init(test_printf, test_flush, ts, test_completion);
    readline_start(ts->rs, "", 0, test_command, ts);
}

static void test_cleanup(TestState *ts)
{
    readline_free(ts->rs);
    g_string_free(ts->output, true);
    g_free(ts->command);
}

/* Type @keys and return the command entered with the final CR. */
static const char *type(TestState *ts, const char *keys)
{
    g_free(ts->command);
    ts->command = NULL;
    for (; *keys; keys++) {
        readline_handle_byte(ts->rs, (uint8_t)*keys);
    }
    g_assert_nonnull(ts->command);
    return ts->command;
}

static void test_home_end(void)
{
    static const struct {
        const char *home;
        const char *end;
    } keys[] = {
        { "\033[H", "\033[F" },     /* xterm, normal cursor key mode */
        { "\033OH", "\033OF" },     /* xterm, application mode */
        { "\033[1~", "\033[4~" },   /* VT220, Linux console */
        { "\033[7~", "\033[8~" },   /* rxvt */
        { "\033[1;5H", "\033[1;5F" }, /* with a modifier (Ctrl) */
        { "\001", "\005" },         /* ^A, ^E */
    };
    TestState ts;
    unsigned i;

    test_init(&ts);
    for (i = 0; i < ARRAY_SIZE(keys); i++) {
        g_autofree char *line = g_strdup_printf("bc%sa%sd\r",
                                                keys[i].home, keys[i].end);

        g_assert_cmpstr(type(&ts, line), ==, "abcd");
    }
    test_cleanup(&ts);
}

static void test_cursor(void)
{
    TestState ts;

    test_init(&ts);
    /* Left, right, delete; application-mode arrows too. */
    g_assert_cmpstr(type(&ts, "abd\033[Dc\033[C\r"), ==, "abcd");
    g_assert_cmpstr(type(&ts, "abd\033ODc\033OC\r"), ==, "abcd");
    g_assert_cmpstr(type(&ts, "axbc\033[H\033[C\033[3~\r"), ==, "abc");
    g_assert_cmpstr(type(&ts, "abc\033[1;5D\033[1;5Dx\r"), ==, "axbc");
    test_cleanup(&ts);
}

static void test_history(void)
{
    TestState ts;

    test_init(&ts);
    g_assert_cmpstr(type(&ts, "one\r"), ==, "one");
    g_assert_cmpstr(type(&ts, "two\r"), ==, "two");
    g_assert_cmpstr(type(&ts, "\033[A\033[A\r"), ==, "one");
    g_assert_cmpstr(type(&ts, "\033OA\r"), ==, "one");
    g_assert_cmpstr(type(&ts, "\033[A\033[A\r"), ==, "two");
    g_assert_cmpstr(type(&ts, "\033[A\033[A\033[B\r"), ==, "two");
    /* End is not history: it leaves the (empty) line alone. */
    g_assert_cmpstr(type(&ts, "\033[F\r"), ==, "");
    test_cleanup(&ts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/readline/home-end", test_home_end);
    g_test_add_func("/readline/cursor", test_cursor);
    g_test_add_func("/readline/history", test_history);
    return g_test_run();
}
