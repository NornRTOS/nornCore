#ifndef PORT_REGS_INCLUDED
#define PORT_REGS_INCLUDED
/**
 * @file port_regs.h
 * Xtensa LX6 special register bits and ESP32 peripheral addresses used by the port and
 * the HAL. Usable from C and assembler.
 */

/* PS register fields. */
#define PS_INTLEVEL_MASK    0x0000000F
#define PS_INTLEVEL(n)      ((n) & PS_INTLEVEL_MASK)
#define PS_EXCM             0x00000010
#define PS_UM               0x00000020
#define PS_RING(n)          (((n) & 3) << 6)
#define PS_OWB_MASK         0x00000F00
#define PS_CALLINC(n)       (((n) & 3) << 16)
#define PS_WOE              0x00040000

/* PS.INTLEVEL of a critical section and of the kernel's interrupt handlers: every CPU
   interrupt of this level and below is masked while the kernel works, every one above is
   never masked by the kernel ("zero latency") and may therefore never call it. All the
   sources the kernel and the modules attach are level 1 (PORT_LEVEL1_INT_MASK), so the
   value is a margin, not a need; a build may override it (EXTRA_CFLAGS). The levels of the
   ESP32's CPU interrupts: 1 for 0..10, 12, 13, 17, 18; 2 for 19..21; 3 for 23, 27; 4 for
   24, 25, 28, 30; 5 for 16, 26, 31; NMI 14 (port_vectors.S sends 2..5 and the NMI to the
   fatal handler, there is no C dispatcher above level 1). */
#ifndef PORT_INTLEVEL_CRITICAL
# define PORT_INTLEVEL_CRITICAL  3
#endif

/* Data breakpoint 0 (RTOS_CFG_STACK_GUARD): DBREAKC bit 31 breaks on a store, bits 5..0 are
   the address bits compared (0x20: a 32 byte window, naturally aligned). DEBUGCAUSE bit 2
   says a data breakpoint caused the debug exception, which is taken at PORT_DEBUGLEVEL. */
#define DBREAKC_STORE           0x80000000ul
#define DBREAKC_MASK_32         0x20ul
#define DEBUGCAUSE_DBREAK       0x04ul
#define PORT_DEBUGLEVEL         6

/* EXCCAUSE value of a level 1 interrupt. */
#define EXCCAUSE_LEVEL1_INTERRUPT   4

/* CPU interrupt numbers that are level 1 on the ESP32 (0-10, 12, 13, 17, 18). 6 is the
   CCOMPARE0 timer, 7 is software; the rest are external, mapped by the interrupt matrix. */
#define PORT_LEVEL1_INT_MASK    0x000637FF
#define PORT_INT_TIMER0         6

/*
 * Interrupt frame on the stack of the interrupted task, built by _level1Interrupt in
 * port_context.S. HSP is the stack pointer while C handlers run; the saved registers sit
 * at HSP + PORT_ISR_FP. The 64 bytes between are the window save areas of the handler's
 * own register window (see port_context.S), and the 16 bytes at the very top are the base
 * save area of the interrupted frame's caller, which the spill at entry fills in.
 */
#define PORT_ISR_FRMSZ      176
#define PORT_ISR_FP         64
#define PORT_FP_PS          0
#define PORT_FP_PC          4
#define PORT_FP_A0          8
#define PORT_FP_A1          12
#define PORT_FP_A2          16      /* a2..a15 follow, 4 bytes each */
#define PORT_FP_A3          20
#define PORT_FP_AR(n)       (PORT_FP_A2 + 4 * ((n) - 2))
#define PORT_FP_SAR         72
#define PORT_FP_LBEG        76
#define PORT_FP_LEND        80
#define PORT_FP_LCOUNT      84
#define PORT_FP_CAUSE       88
#define PORT_FP_CPENABLE    92
#define PORT_FP_SIZE        112     /* incl. the 16 byte base save area at the top */

/* EXCCAUSE of a coprocessor 0 (FPU) instruction while CPENABLE bit 0 is clear. */
#define EXCCAUSE_CP0_DISABLED   32

/* Initial frame of a task, built by rtos_port_initStack: a0 and PS for the retw that
   starts it, with the fake caller's a0..a3 in the 16 bytes below. */
#define PORT_INIT_FRMSZ     32

/* Peripheral register base addresses (ESP32 TRM). */
#define DR_REG_DPORT_BASE   0x3FF00000
#define DR_REG_UART_BASE    0x3FF40000
#define DR_REG_GPIO_BASE    0x3FF44000
#define DR_REG_RTCCNTL_BASE 0x3FF48000
#define DR_REG_IO_MUX_BASE  0x3FF49000
#define DR_REG_TIMG0_BASE   0x3FF5F000
#define DR_REG_TIMG1_BASE   0x3FF60000

/* Watchdog write protection key. */
#define WDT_WKEY            0x50D83AA1

#ifndef __ASSEMBLER__
# include <stdint.h>
# define PORT_REG(addr)     (*(volatile uint32_t *)(addr))
# define RTOS_PORT_STRINGIFY_(x) #x
# define RTOS_PORT_STRINGIFY(x)  RTOS_PORT_STRINGIFY_(x)
#endif

#endif  /* PORT_REGS_INCLUDED */
