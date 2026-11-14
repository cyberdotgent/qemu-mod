/*
 * Exercise s390 KVM hardware breakpoints at taken branch targets.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

int main(void)
{
    asm volatile(
        "larl %%r1,taken_target\n"
        "br %%r1\n"
        ".globl not_taken\n"
        "not_taken:\n"
        "j test_done\n"
        ".globl taken_target\n"
        "taken_target:\n"
        "lghi %%r2,1\n"
        "larl %%r1,second_target\n"
        "br %%r1\n"
        ".globl second_target\n"
        "second_target:\n"
        "lghi %%r2,2\n"
        ".globl test_done\n"
        "test_done:\n"
        :
        :
        : "r1", "r2", "cc");
    return 0;
}
