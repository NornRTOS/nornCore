/**
 * @file port_clock.c
 * CPU clock of the ESP32. The ROM leaves the CPU on the 40 MHz crystal; this switches
 * it to the PLL for 80, 160 or 240 MHz, following the sequence of Espressif's rtc_clk.c
 * (Apache-2.0): power up the PLL, program it through the internal I2C bus with the ROM's
 * rom_i2c_writeReg, select the divider in DPORT and the PLL as system clock source. The
 * APB bus then runs at 80 MHz regardless of the CPU frequency.
 *
 * F_CPU selects the frequency at compile time (port/xtensa/arch.mk, CPU_MHZ). 80, 160
 * (320 MHz PLL) and 240 MHz (480 MHz PLL) pass the test suite on the board. The PLL
 * constants are those of ESP-IDF 5.5.2 (hal/esp32/include/hal/clk_tree_ll.h); two of
 * them were wrong until 2026-11-03: BBADC_DSM 0x4c instead of 0x84 for the 320 MHz PLL
 * left the WiFi baseband unable to decode anything above MCS 1, and lref 1 instead of 0
 * for 480 MHz made 240 MHz crash at random within seconds.
 */

#include "rtos.h"


/*
 * Defines
 */

#define RTC_CNTL_OPTIONS0       PORT_REG(DR_REG_RTCCNTL_BASE + 0x00)
#define RTC_CNTL_CLK_CONF       PORT_REG(DR_REG_RTCCNTL_BASE + 0x70)
#define RTC_CNTL_REG            PORT_REG(DR_REG_RTCCNTL_BASE + 0x7C)
#define RTC_CNTL_STORE4         PORT_REG(DR_REG_RTCCNTL_BASE + 0xB0)  /* RTC_XTAL_FREQ_REG */
#define RTC_CNTL_STORE5         PORT_REG(DR_REG_RTCCNTL_BASE + 0xB4)  /* RTC_APB_FREQ_REG */
#define DPORT_CPU_PER_CONF      PORT_REG(DR_REG_DPORT_BASE + 0x3C)

#define RTC_CNTL_BIAS_I2C_FORCE_PD  (1ul << 18)
#define RTC_CNTL_BBPLL_FORCE_PD     (1ul << 10)
#define RTC_CNTL_BBPLL_I2C_FORCE_PD (1ul << 8)
#define RTC_CNTL_BB_I2C_FORCE_PD    (1ul << 6)

#define RTC_CNTL_SOC_CLK_SEL_SHIFT  27          /* 0 XTAL, 1 PLL, 2 8M, 3 APLL */
#define RTC_CNTL_SOC_CLK_SEL_MASK   (3ul << 27)
#define RTC_CNTL_DIG_DBIAS_WAK_SHIFT 11         /* 3 bits */
#define RTC_CNTL_DBIAS_1V10         4ul
#define RTC_CNTL_DBIAS_1V25         7ul

#define UART_STATUS             PORT_REG(DR_REG_UART_BASE + 0x1C)

/* The PLL registers behind the internal I2C bus, block 0x66, host 4. */
#define I2C_BBPLL               0x66u
#define I2C_BBPLL_HOSTID        4u
#define I2C_BBPLL_IR_CAL_DELAY  0u
#define I2C_BBPLL_IR_CAL_EXT_CAP 1u
#define I2C_BBPLL_OC_LREF       2u
#define I2C_BBPLL_OC_DIV_7_0    3u
#define I2C_BBPLL_OC_ENB_FCAL   4u
#define I2C_BBPLL_OC_DCUR       5u
#define I2C_BBPLL_OC_ENB_VCON   6u
#define I2C_BBPLL_BBADC_DSM     9u
#define I2C_BBPLL_ENDIV5        11u
#define I2C_BBPLL_BBADC_CAL_7_0 12u

/** ROM function: tells the ROM the CPU frequency (g_ticks_per_us), which its ets_delay_us
    and everything built on it (Espressif's radio libraries, for RF settling times)
    rely on. Without this call a delay of n us lasts n/4 us at 160 MHz. */
typedef void (*ets_update_cpu_frequency_t)(uint32_t ticksPerUs);
#define ets_update_cpu_frequency ((ets_update_cpu_frequency_t)0x40008550)

/** ROM function: write one register of an analog block over the internal I2C bus. */
typedef void (*rom_i2c_writeReg_t)(uint8_t block, uint8_t hostId, uint8_t reg, uint8_t data);
#define rom_i2c_writeReg        ((rom_i2c_writeReg_t)0x400041a4)


/* Registers of the RTC and analog power control that ESP-IDF's rtc_init (second stage
   bootloader) sets up and the ROM leaves at its own defaults. */
#define RTC_CNTL_TIMER1         PORT_REG(DR_REG_RTCCNTL_BASE + 0x1C)
#define RTC_CNTL_ANA_CONF       PORT_REG(DR_REG_RTCCNTL_BASE + 0x30)
#define RTC_CNTL_INT_ENA        PORT_REG(DR_REG_RTCCNTL_BASE + 0x3C)
#define RTC_CNTL_INT_CLR        PORT_REG(DR_REG_RTCCNTL_BASE + 0x48)
#define RTC_CNTL_BIAS_CONF      PORT_REG(DR_REG_RTCCNTL_BASE + 0x78)
#define RTC_CNTL_PWC            PORT_REG(DR_REG_RTCCNTL_BASE + 0x80)
#define RTC_CNTL_DIG_PWC        PORT_REG(DR_REG_RTCCNTL_BASE + 0x84)
#define RTC_CNTL_DIG_ISO        PORT_REG(DR_REG_RTCCNTL_BASE + 0x88)
#define SENS_SAR_MEAS_WAIT2     PORT_REG(0x3FF48800ul + 0x0C)
#define ANA_CONFIG              PORT_REG(0x6000E044ul)      /* internal I2C bus control */
#define DPORT_PRO_CACHE_CTRL1   PORT_REG(DR_REG_DPORT_BASE + 0x044)
#define DPORT_APP_CACHE_CTRL1   PORT_REG(DR_REG_DPORT_BASE + 0x05C)
#define DPORT_ROM_FO_CTRL       PORT_REG(DR_REG_DPORT_BASE + 0x094)
#define DPORT_SRAM_FO_CTRL_0    PORT_REG(DR_REG_DPORT_BASE + 0x0A0)
#define DPORT_SRAM_FO_CTRL_1    PORT_REG(DR_REG_DPORT_BASE + 0x0A4)
#define DPORT_TAG_FO_CTRL       PORT_REG(DR_REG_DPORT_BASE + 0x0AC)
#define DPORT_PERI_CLK_EN       PORT_REG(DR_REG_DPORT_BASE + 0x01C)  /* AES, SHA, RSA, secure boot */
#define DPORT_PERI_RST_EN       PORT_REG(DR_REG_DPORT_BASE + 0x020)
#define DPORT_PERIP_CLK_EN      PORT_REG(DR_REG_DPORT_BASE + 0x0C0)
#define DPORT_PERIP_RST_EN      PORT_REG(DR_REG_DPORT_BASE + 0x0C4)
#define DPORT_WIFI_CLK_EN_REG   PORT_REG(DR_REG_DPORT_BASE + 0x0CC)

/*
 * Function implementation
 */

/**
 * The RTC, bias and analog power control setup of ESP-IDF's rtc_init and rtc_clk_init
 * (components/esp_hw_support/port/esp32, Apache-2.0), which the IDF bootloader runs on
 * every boot and the ROM does not: the forced power-ups of the RF and PLL analog blocks
 * off, the audio PLL off, biases at their defaults, the crystal and PLL buffer waits,
 * the 8 MHz and 150 kHz oscillator tuning, the internal I2C bus reset and the SAR
 * (ADC, temperature sensor) control on. Without it the radio runs, but the access point
 * only gets its lowest rates through (nornWIF, 2026-11-03).
 */
static void rtcInit(void)
{
    /* Modem domain not forced down while touching the analog blocks. */
    RTC_CNTL_DIG_PWC &= ~(1ul << 17);                   /* WIFI_FORCE_PD */
    RTC_CNTL_DIG_ISO &= ~(1ul << 28);                   /* WIFI_FORCE_ISO */
    RTC_CNTL_ANA_CONF &= ~((1ul << 26) | (1ul << 27) | (1ul << 28) | (1ul << 30) | (1ul << 31));
                            /* PVTMON_PU, TXRF_I2C_PU, RFRX_PBUS_PU, CKGEN_I2C_PU, PLL_I2C_PU */
    /* Buffer waits: PLL 20 (bits 24..31), crystal 20 (bits 14..23), 8M 20 (bits 6..13). */
    RTC_CNTL_TIMER1 = (RTC_CNTL_TIMER1 & ~((0xFFul << 24) | (0x3FFul << 14) | (0xFFul << 6)))
                      | (20ul << 24) | (20ul << 14) | (20ul << 6);
    /* Bias: DBG_ATTEN 3 (bits 24..25), DEC_HEARTBEAT_WIDTH, INC_HEARTBEAT_PERIOD. */
    RTC_CNTL_BIAS_CONF = (RTC_CNTL_BIAS_CONF & ~(3ul << 24)) | (3ul << 24) | (1ul << 30) | (1ul << 29);
    /* RTC domain bias 1.10 V awake (bits 25..27) and in sleep (bits 22..24); the 150 kHz
       oscillator tuning SCK_DCAP 255 (bits 14..21). */
    RTC_CNTL_REG = (RTC_CNTL_REG & ~((7ul << 25) | (7ul << 22) | (0xFFul << 14)))
                   | (4ul << 25) | (4ul << 22) | (255ul << 14);
    /* Clock control: no forced-on clocks for the cache MMU, ROM, SRAM and cache tags. */
    DPORT_PRO_CACHE_CTRL1 &= ~(1ul << 11);
    DPORT_APP_CACHE_CTRL1 &= ~(1ul << 11);
    DPORT_ROM_FO_CTRL &= ~((0x3Ful << 2) | (1ul << 1) | (1ul << 0));
    DPORT_SRAM_FO_CTRL_0 = 0;
    DPORT_SRAM_FO_CTRL_1 &= ~(1ul << 0);
    DPORT_TAG_FO_CTRL &= ~((1ul << 8) | (1ul << 0));
    /* Power control: nothing forced up, biases follow the 8M clock, audio PLL off. */
    RTC_CNTL_CLK_CONF &= ~(1ul << 26);                  /* CK8M_FORCE_PU */
    RTC_CNTL_OPTIONS0 &= ~((1ul << 13) | (1ul << 22) | (1ul << 19) | (1ul << 16));
                            /* XTL_FORCE_PU, BIAS_CORE_FORCE_PU, BIAS_I2C_FORCE_PU, BIAS_FORCE_NOSLEEP */
    RTC_CNTL_OPTIONS0 |= (1ul << 20) | (1ul << 17) | (1ul << 14);
                            /* BIAS_CORE_FOLW_8M, BIAS_I2C_FOLW_8M, BIAS_SLEEP_FOLW_8M */
    RTC_CNTL_ANA_CONF &= ~(1ul << 24);                  /* PLLA_FORCE_PU */
    RTC_CNTL_ANA_CONF |= (1ul << 23);                   /* PLLA_FORCE_PD */
    RTC_CNTL_OPTIONS0 &= ~((1ul << 11) | (1ul << 9));   /* BBPLL_FORCE_PU, BBPLL_I2C_FORCE_PU */
    RTC_CNTL_REG &= ~((1ul << 31) | (1ul << 29));       /* FORCE_PU, DBOOST_FORCE_PU */
    RTC_CNTL_REG |= (1ul << 28);                        /* DBOOST_FORCE_PD */
    RTC_CNTL_DIG_PWC &= ~((1ul << 4) | (1ul << 20) | (1ul << 18)   /* LSLP_MEM, DG_WRAP, WIFI force PU */
                          | (1ul << 16) | (1ul << 14) | (1ul << 12) | (1ul << 10) | (1ul << 8) | (1ul << 6));
                            /* INTER_RAM4..0, ROM0 force PU */
    RTC_CNTL_PWC &= ~((1ul << 19) | (1ul << 5)          /* PWC_FORCE_PU, FORCE_NOISO */
                      | (1ul << 16) | (1ul << 13) | (1ul << 2) | (1ul << 0));
                            /* SLOWMEM/FASTMEM force PU and force NOISO */
    RTC_CNTL_DIG_ISO &= ~((1ul << 31) | (1ul << 29) | (1ul << 14) | (1ul << 12)
                          | (1ul << 27) | (1ul << 25) | (1ul << 23) | (1ul << 21) | (1ul << 19) | (1ul << 17));
                            /* DG_WRAP_FORCE_NOISO, WIFI_FORCE_NOISO, DG_PAD_FORCE_UNHOLD, DG_PAD_FORCE_NOISO,
                               INTER_RAM4..0 and ROM0 force NOISO */
    /* Modem domain down until the radio driver powers it (as the IDF leaves it). */
    RTC_CNTL_DIG_ISO |= (1ul << 28);
    RTC_CNTL_DIG_PWC |= (1ul << 17);
    RTC_CNTL_INT_ENA = 0;
    RTC_CNTL_INT_CLR = 0xFFFFFFFFul;
    /* SAR control on (ADC, temperature sensor the PHY uses). */
    SENS_SAR_MEAS_WAIT2 = (SENS_SAR_MEAS_WAIT2 & ~(3ul << 18)) | (3ul << 18);
    /* 8 MHz oscillator: tuning CK8M_DFREQ 172 (bits 17..24), divider 1 (bits 12..14 and
       4..5 zero), its /256 output off (bit 9 clear, bit 7 set), and it feeds the RTC fast
       clock (bit 29), as rtc_clk_init leaves it. */
    RTC_CNTL_CLK_CONF = (RTC_CNTL_CLK_CONF & ~((0xFFul << 17) | (7ul << 12) | (3ul << 4) | (1ul << 9)))
                        | (172ul << 17) | (1ul << 7) | (1ul << 29);
    /* Peripheral clocks the application start of ESP-IDF turns off (esp_perip_clk_init):
       everything but UART0, the flash SPI and timer group 0; the hardware crypto blocks
       held in reset; of the modem clocks only what the radio driver enables itself. */
    {
        const uint32_t unused = (1ul << 3) | (1ul << 4) | (1ul << 5) | (1ul << 6) | (1ul << 7) | (1ul << 8)
                                | (1ul << 9) | (1ul << 10) | (1ul << 11) | (1ul << 12) | (1ul << 15) | (1ul << 16)
                                | (1ul << 17) | (1ul << 18) | (1ul << 19) | (1ul << 20) | (1ul << 21) | (1ul << 22)
                                | (1ul << 23) | (1ul << 25) | (1ul << 26);
        DPORT_PERIP_CLK_EN &= ~unused;
        DPORT_PERIP_RST_EN |= unused;
        DPORT_PERIP_RST_EN &= ~unused;
        DPORT_PERI_CLK_EN &= ~0xFul;
        DPORT_PERI_RST_EN |= 0xFul;
        DPORT_WIFI_CLK_EN_REG &= ~(0x406ul | (0x61ul << 11) | (1ul << 4) | (1ul << 5) | (1ul << 12) | (1ul << 13) | (1ul << 14));
    }
    /* Internal I2C bus: reset all, then enable the BBPLL and APLL register blocks. */
    ANA_CONFIG |= (0x3FFul << 8);
    ANA_CONFIG &= ~((1ul << 17) | (1ul << 14));
}

static void delayXtalUs(uint32_t us)
{
    /* Still on the 40 MHz crystal here. */
    const uint32_t start = rtos_port_ccount();
    while((uint32_t)(rtos_port_ccount() - start) < us * 40ul)
        ;
}


void rtos_port_clockInit(void)
{
    rtcInit();
#if F_CPU == 40000000ul
    /* Crystal: nothing to do. */
#else
    const bool pll480 = (F_CPU == 240000000ul);
    uint32_t lref, div, dcur, sel;

    /* Let the ROM's last log line out before the UART clock changes. */
    while(((UART_STATUS >> 16) & 0xFFu) != 0)
        ;

    /* Digital supply for 240 MHz, PLL and its I2C interface powered. */
    RTC_CNTL_REG = (RTC_CNTL_REG & ~(7ul << RTC_CNTL_DIG_DBIAS_WAK_SHIFT))
                   | ((pll480 ? RTC_CNTL_DBIAS_1V25 : RTC_CNTL_DBIAS_1V10) << RTC_CNTL_DIG_DBIAS_WAK_SHIFT);
    RTC_CNTL_OPTIONS0 &= ~(RTC_CNTL_BIAS_I2C_FORCE_PD | RTC_CNTL_BB_I2C_FORCE_PD
                           | RTC_CNTL_BBPLL_FORCE_PD | RTC_CNTL_BBPLL_I2C_FORCE_PD);

    /* Calibration and PLL mode (320 MHz for 80/160, 480 MHz for 240), 40 MHz crystal. */
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_IR_CAL_DELAY, 0x18);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_IR_CAL_EXT_CAP, 0x20);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_ENB_FCAL, 0x9a);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_ENB_VCON, 0x00);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_BBADC_CAL_7_0, 0x00);
    delayXtalUs(3);                     /* supply settled before the PLL is programmed */
    if(pll480)
    {
        rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_ENDIV5, 0xc3);
        rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_BBADC_DSM, 0x74);
        lref = 0u;                      /* lref 0, div10_8 0, div_ref 0 (clk_tree_ll.h, 40 MHz) */
        div = 28;
        dcur = (3u << 6) | 6u;          /* bw 3, dcur 6 */
    }
    else
    {
        rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_ENDIV5, 0x43);
        rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_BBADC_DSM, 0x84);
        lref = 0u;
        div = 32;
        dcur = (3u << 6) | 6u;
    }
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_LREF, (uint8_t)lref);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0, (uint8_t)div);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DCUR, (uint8_t)dcur);
    delayXtalUs(1000);                  /* PLL lock; Espressif waits 80 us, be generous */

    /* CPU divider from the PLL: 0 = 80, 1 = 160, 2 = 240 MHz; then PLL as source. */
    sel = (F_CPU == 80000000ul) ? 0u : (F_CPU == 160000000ul) ? 1u : 2u;
    DPORT_CPU_PER_CONF = (DPORT_CPU_PER_CONF & ~3ul) | sel;
    RTC_CNTL_CLK_CONF = (RTC_CNTL_CLK_CONF & ~RTC_CNTL_SOC_CLK_SEL_MASK) | (1ul << RTC_CNTL_SOC_CLK_SEL_SHIFT);
    ets_update_cpu_frequency(F_CPU / 1000000ul);
    /* The frequencies as ESP-IDF stores them for the ROM and its libraries: the crystal
       in MHz, the APB clock in Hz >> 12, each in both halves of the word. */
    RTC_CNTL_STORE4 = 40ul | (40ul << 16);
    RTC_CNTL_STORE5 = (80000000ul >> 12) | ((80000000ul >> 12) << 16);
#endif
}
