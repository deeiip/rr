from rrutil import *

send_gdb('b first_breakpoint\n')
expect_gdb('Breakpoint 1')

send_gdb('b second_breakpoint\n')
expect_gdb('Breakpoint 2')

send_gdb('c\n')
# If we hit first_breakpoint, then we never continue and never reach
# second_breakpoint.
expect_gdb('Breakpoint 2, second_breakpoint')

restart_replay(1)
expect_gdb('Breakpoint 1, first_breakpoint')

ok()
