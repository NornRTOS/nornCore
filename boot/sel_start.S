/**
 * @file sel_start.S
 * Entry of the OTA selector (boot/sel_main.c): what port_start.S does for an application,
 * without vectors (the selector takes no interrupts) and with its own stack.
 */
#include "port_regs.h"

    .section .entry, "ax"
    .literal_position
    .global _sel_start
    .align  4
_sel_start:
    movi    a0, 0
    wsr     a0, INTENABLE
    movi    a0, 1
    wsr     a0, WINDOWSTART
    movi    a0, 0
    wsr     a0, WINDOWBASE
    rsync
    movi    a0, PS_WOE | PS_UM | PS_INTLEVEL(PORT_INTLEVEL_CRITICAL)
    wsr     a0, PS
    rsync
    movi    a1, _sel_stack_top
    movi    a0, 0
    movi    a2, _sel_bss_start
    movi    a3, _sel_bss_end
    movi    a4, 0
1:  bgeu    a2, a3, 2f
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       1b
2:  call4   sel_main
3:  j       3b
