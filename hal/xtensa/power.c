/**
 * @file power.c
 * Deep sleep on the ESP32 (hal/power.h): the RTC timer wakes the chip after the given
 * time, everything else is powered down. The register sequence follows ESP-IDF 5.1's
 * rtc_sleep_init() with the deep sleep configuration (RTC_SLEEP_PD_DIG, _RTC_PERIPH,
 * _RTC_FAST_MEM, _VDDSDIO, _MODEM, _XTAL, _INT_8M) and rtc_deep_sleep_start(), minus the
 * radio memory bits that only matter for light sleep and the wake stub CRC (no stub: the
 * ROM boots the image from flash as after any reset). The slow clock is the 150 kHz RC
 * oscillator, calibrated against the 40 MHz crystal with timer group 0 right before the
 * sleep, as rtc_clk_cal() does.
 */

#include "rtos.h"
#include "uart.h"
#include "power.h"
#include "port_regs.h"


/*
 * Defines
 */

#define RTC(off)                PORT_REG(DR_REG_RTCCNTL_BASE + (off))
#define RTC_OPTIONS0            RTC(0x00)
#define RTC_SLP_TIMER0          RTC(0x04)
#define RTC_SLP_TIMER1          RTC(0x08)
#define RTC_TIME_UPDATE         RTC(0x0C)
#define RTC_TIME0               RTC(0x10)
#define RTC_TIME1               RTC(0x14)
#define RTC_STATE0              RTC(0x18)
#define RTC_TIMER3              RTC(0x24)
#define RTC_TIMER4              RTC(0x28)
#define RTC_TIMER5              RTC(0x2C)
#define RTC_ANA_CONF            RTC(0x30)
#define RTC_WAKEUP_STATE        RTC(0x38)
#define RTC_INT_CLR             RTC(0x48)
#define RTC_SLP_REJECT_CONF     RTC(0x64)
#define RTC_CLK_CONF            RTC(0x70)
#define RTC_SDIO_CONF           RTC(0x74)
#define RTC_BIAS_CONF           RTC(0x78)
#define RTC_REG                 RTC(0x7C)
#define RTC_PWC                 RTC(0x80)
#define RTC_DIG_PWC             RTC(0x84)
#define RTC_DIG_ISO             RTC(0x88)
#define RTC_STORE5              RTC(0xB4)      /* RTC_APB_FREQ_REG: what the ROM's UART code assumes */
#define RTC_STORE6              RTC(0xB8)      /* RTC_ENTRY_ADDR_REG: the wake stub, 0 = none */

#define TIMG0_RTCCALICFG        PORT_REG(DR_REG_TIMG0_BASE + 0x68)
#define TIMG0_RTCCALICFG1       PORT_REG(DR_REG_TIMG0_BASE + 0x6C)

#define XTAL_MHZ                40ul
#define CAL_CYCLES              1024ul
#define CAL_FRACT               19              /* the period in us as Q13.19 */

/** Sets a field of @p reg: @p mask already shifted, @p val not. */
#define SET_FIELD(reg, mask, shift, val)  ((reg) = ((reg) & ~(mask)) | (((uint32_t)(val) << (shift)) & (mask)))


/*
 * Local functions
 */

/** The 48 bit RTC counter. */
static uint64_t rtcTime(void)
{
    RTC_TIME_UPDATE = 1ul << 31;                        /* TIME_UPDATE */
    while((RTC_TIME_UPDATE & (1ul << 30)) == 0)         /* TIME_VALID */
        ;
    return ((uint64_t)(RTC_TIME1 & 0xFFFFul) << 32) | RTC_TIME0;
}

/** Period of the RTC slow clock in Q13.19 microseconds: CAL_CYCLES of it counted in
    crystal cycles by timer group 0 (rtc_clk_cal_internal, RTC_CAL_RTC_MUX). */
uint32_t power_slowClockPeriod(void)
{
    uint64_t xtalCycles, divider;

    TIMG0_RTCCALICFG &= ~(1ul << 31);                   /* START off while configuring */
    SET_FIELD(TIMG0_RTCCALICFG, 3ul << 13, 13, 0);      /* CLK_SEL: the RTC slow clock */
    TIMG0_RTCCALICFG &= ~(1ul << 12);                   /* START_CYCLING off: one measurement */
    SET_FIELD(TIMG0_RTCCALICFG, 0x7FFFul << 16, 16, CAL_CYCLES);
    TIMG0_RTCCALICFG |= 1ul << 31;                      /* START */
    while((TIMG0_RTCCALICFG & (1ul << 15)) == 0)        /* RDY */
        ;
    xtalCycles = TIMG0_RTCCALICFG1 >> 7;
    divider = (uint64_t)XTAL_MHZ * CAL_CYCLES;
    return (uint32_t)(((xtalCycles << CAL_FRACT) + divider / 2 - 1) / divider);
}


/*
 * Global functions
 */

void power_deepSleep(uint32_t ms)
{
    uint64_t wakeAt;
    uint32_t period;

    uart_flush();
    (void)rtos_port_enterCritical();                    /* nothing runs after this */

    /* CPU and APB back on the crystal before the sleep, as esp_deep_sleep_start does: the
       clock selection lives in the RTC domain and survives the wake-up reset, and the ROM
       and the port's clock init expect to start from the crystal. */
    PORT_REG(DR_REG_DPORT_BASE + 0x3C) &= ~3ul;         /* DPORT_CPU_PER_CONF: 80 MHz divider */
    RTC_CLK_CONF &= ~(3ul << 27);                       /* SOC_CLK_SEL: XTAL */
    RTC_STORE5 = (40000000ul >> 12) | ((40000000ul >> 12) << 16);   /* the ROM's APB frequency note: 40 MHz */

    /* The wake-up time in slow clock cycles from now (rtc_time_us_to_slowclk). */
    period = power_slowClockPeriod();
    wakeAt = rtcTime() + (((uint64_t)ms * 1000ul) << CAL_FRACT) / period;
    RTC_SLP_TIMER0 = (uint32_t)wakeAt;
    RTC_SLP_TIMER1 = (uint32_t)(wakeAt >> 32) & 0xFFFFul;
    RTC_INT_CLR = 1ul << 8;                             /* MAIN_TIMER */
    RTC_SLP_TIMER1 |= 1ul << 16;                        /* MAIN_TIMER_ALARM_EN */
    RTC_STORE6 = 0;                                     /* no wake stub: boot from flash */

    /* rtc_sleep_init with the deep sleep configuration. Power-up and wait timers at their
       minimum (1 cycle each), the shortest sleep 128 cycles. */
    SET_FIELD(RTC_TIMER5, 0xFFul << 8, 8, 128);         /* MIN_SLP_VAL */
    SET_FIELD(RTC_TIMER3, 0x7Ful << 25, 25, 1);         /* ROM_RAM_POWERUP_TIMER */
    SET_FIELD(RTC_TIMER3, 0x1FFul << 16, 16, 1);        /* ROM_RAM_WAIT_TIMER */
    SET_FIELD(RTC_TIMER3, 0x7Ful << 9, 9, 1);           /* WIFI_POWERUP_TIMER */
    SET_FIELD(RTC_TIMER3, 0x1FFul, 0, 1);               /* WIFI_WAIT_TIMER */
    SET_FIELD(RTC_TIMER4, 0x7Ful << 9, 9, 1);           /* POWERUP_TIMER */
    SET_FIELD(RTC_TIMER4, 0x1FFul, 0, 1);               /* WAIT_TIMER */
    SET_FIELD(RTC_TIMER4, 0x7Ful << 25, 25, 1);         /* DG_WRAP_POWERUP_TIMER */
    SET_FIELD(RTC_TIMER4, 0x1FFul << 16, 16, 1);        /* DG_WRAP_WAIT_TIMER */
    SET_FIELD(RTC_TIMER5, 0x7Ful << 25, 25, 1);         /* RTCMEM_POWERUP_TIMER */
    SET_FIELD(RTC_TIMER5, 0x1FFul << 16, 16, 1);        /* RTCMEM_WAIT_TIMER */

    RTC_DIG_PWC &= ~(1ul << 4);                         /* LSLP_MEM_FORCE_PU off */
    RTC_PWC &= ~((1ul << 11) | (1ul << 8));             /* SLOWMEM/FASTMEM_FORCE_LPU off */
    RTC_PWC &= ~((1ul << 16) | (1ul << 13));            /* MEM_FORCE_PU off */
    RTC_PWC &= ~((1ul << 9) | (1ul << 6));              /* MEM_FOLW_CPU off */
    RTC_PWC |= 1ul << 14;                               /* FASTMEM_PD_EN: fast RTC memory off */
    RTC_PWC &= ~((1ul << 13) | (1ul << 0));             /* FASTMEM_FORCE_PU, FASTMEM_FORCE_NOISO off */
    RTC_PWC &= ~(1ul << 17);                            /* SLOWMEM_PD_EN off: POWER_RTC_DATA stays */
    RTC_PWC |= (1ul << 16) | (1ul << 2);                /* SLOWMEM_FORCE_PU, SLOWMEM_FORCE_NOISO */
    RTC_PWC |= 1ul << 20;                               /* PD_EN: RTC peripherals off */
    RTC_DIG_ISO &= ~(1ul << 29);                        /* WIFI_FORCE_NOISO off */
    RTC_DIG_PWC &= ~(1ul << 18);                        /* WIFI_FORCE_PU off */
    RTC_DIG_PWC |= 1ul << 30;                           /* WIFI_PD_EN */
    RTC_DIG_PWC &= ~(0x3Ful << 24);                     /* ROM0, INTER_RAM0..4 PD_EN off (follow the domain) */
    /* deep sleep: the digital domain goes down */
    RTC_DIG_ISO &= ~((1ul << 13) | (1ul << 12));        /* DG_PAD_FORCE_ISO, DG_PAD_FORCE_NOISO off */
    RTC_DIG_PWC |= 1ul << 31;                           /* DG_WRAP_PD_EN */
    RTC_DIG_PWC &= ~((1ul << 20) | (1ul << 19));        /* DG_WRAP_FORCE_PU/PD off */
    RTC_OPTIONS0 &= ~(1ul << 16);                       /* BIAS_FORCE_NOSLEEP off */
    RTC_ANA_CONF &= ~((1ul << 30) | (1ul << 31) | (1ul << 28) | (1ul << 27));  /* CKGEN/PLL I2C, RFRX, TXRF off */
    RTC_OPTIONS0 &= ~(1ul << 7);                        /* BB_I2C_FORCE_PU off */
    RTC_OPTIONS0 &= ~(1ul << 13);                       /* XTL_FORCE_PU off: the crystal stops */
    RTC_CLK_CONF &= ~(1ul << 26);                       /* CK8M_FORCE_PU off */
    RTC_SDIO_CONF &= ~(1ul << 22);                      /* SDIO_FORCE off */
    RTC_SDIO_CONF |= 1ul << 21;                         /* SDIO_PD_EN: VDD_SDIO (the flash) off */
    SET_FIELD(RTC_REG, 7ul << 22, 22, 0);               /* DBIAS_SLP 0.90 V */
    SET_FIELD(RTC_REG, 7ul << 25, 25, 4);               /* DBIAS_WAK 1.10 V */
    SET_FIELD(RTC_REG, 7ul << 11, 11, 4);               /* DIG_DBIAS_WAK 1.10 V */
    SET_FIELD(RTC_REG, 7ul << 8, 8, 0);                 /* DIG_DBIAS_SLP 0.90 V */
    SET_FIELD(RTC_BIAS_CONF, 3ul << 24, 24, 3);         /* DBG_ATTEN default */
    RTC_SLP_REJECT_CONF |= (1ul << 27) | (1ul << 26);   /* DEEP/LIGHT_SLP_REJECT_EN */

    /* rtc_deep_sleep_start: the timer is the wake-up source, then SLEEP_EN. */
    SET_FIELD(RTC_WAKEUP_STATE, 0x7FFul << 11, 11, 1ul << 3);   /* WAKEUP_ENA = RTC_TIMER_TRIG_EN */
    RTC_INT_CLR = (1ul << 1) | (1ul << 0);              /* SLP_REJECT, SLP_WAKEUP */
    RTC_STATE0 |= 1ul << 31;                            /* SLEEP_EN */
    __asm__ __volatile__ ("memw" : : : "memory");
    for(;;)
        __asm__ __volatile__ ("waiti 0");               /* the wake-up is a reset */
}
