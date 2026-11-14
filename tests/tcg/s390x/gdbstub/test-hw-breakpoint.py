"""Test zero-kind s390 KVM hardware breakpoints.

This runs as a sourced script (via -x, via run-test.py).
"""

import gdb
from test_gdbstub import main, report


def symbol_address(name):
    return int(gdb.parse_and_eval("&" + name))


def rsp_breakpoint(operation, address):
    output = gdb.execute(
        "maintenance packet %s1,%x,0" % (operation, address),
        False,
        True,
    )
    report('received: "OK"' in output, "%s1 accepted at %#x" %
           (operation, address))


def run_test():
    taken = symbol_address("taken_target")
    second = symbol_address("second_target")

    # Install more than one zero-kind breakpoint before resuming.  The first
    # target is reached by a taken branch, which exercises the PER range edge.
    rsp_breakpoint("Z", taken)
    rsp_breakpoint("Z", second)

    gdb.execute("continue")
    report(int(gdb.parse_and_eval("$pc")) == taken,
           "stopped at taken branch target")

    gdb.execute("continue")
    report(int(gdb.parse_and_eval("$pc")) == second,
           "stopped at second hardware breakpoint")

    rsp_breakpoint("z", taken)
    rsp_breakpoint("z", second)


main(run_test, "s390:64-bit")
