// Code to setup clocks on Huada HC32F460
//
// Copyright (C) 2022  Steven Gotthardt <gotthardt@gmail.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_MACH_AVR
#include "sched.h"
#include "system_hc32f460.h"
#include "hc32f460_gpio.h"
#include "hc32f460_clk.h"


/****************************************************************
 * Startup
 ****************************************************************/

// The vendor bootloader leaves an unknown clock state; the app must set
// its own so SystemCoreClock and CONFIG_CLOCK_FREQ (200 MHz) agree with
// the hardware. Proven on the AD4 by the bare-metal probe (2026-10-21):
// without this, USART BRR is computed against a stale SystemCoreClock
// and the serial line is garble. MPLL from HRC: VCO in 8-10M, VCO out
// 400M, /2 -> 200 MHz system clock, PCLK1 (/4) = 50M USART clock.
static void clock_init_200m(void)
{
    uint32_t hrc = (1UL == (HRC_FREQ_MON() & 1UL)) ? 16000000UL : 20000000UL;
    stc_clk_mpll_cfg_t pll = {
        .PllpDiv = 2UL, .PllqDiv = 10UL, .PllrDiv = 10UL,
        .plln = 800000000UL / hrc,      // 50 @16M, 40 @20M -> VCO 400M
        .pllmDiv = 2UL,                 // VCO in 8M / 10M (1-12M range)
    };
    stc_clk_sysclk_cfg_t div = {
        .enHclkDiv  = ClkSysclkDiv1,    // HSB  200M (max 200M)
        .enExclkDiv = ClkSysclkDiv2,    // EXCLK 100M
        .enPclk0Div = ClkSysclkDiv2,    // PCLK0 100M
        .enPclk1Div = ClkSysclkDiv4,    // PCLK1  50M (USART clock, max 50M)
        .enPclk2Div = ClkSysclkDiv4,    // PCLK2  50M
        .enPclk3Div = ClkSysclkDiv4,    // PCLK3  50M
        .enPclk4Div = ClkSysclkDiv2,    // PCLK4 100M
    };
    CLK_HrcCmd(Enable);                 // HRC must run as the PLL source ref
    CLK_SetPllSource(ClkPllSrcHRC);
    CLK_MpllConfig(&pll);
    CLK_SysClkConfig(&div);
    CLK_SetSysClkSource(CLKSysSrcMPLL); // settle waits + SystemCoreClockUpdate()
}

// Main entry point - called from armcm_boot.c:ResetHandler()
void  __attribute__((noreturn))
armcm_main(void)
{
    // sets the system clock speed variable for library use
    SystemInit();

    // put the hardware at 200 MHz before any DECL_INIT reads SystemCoreClock
    clock_init_200m();

    // disable JTAG/SWD on pins PA13, PA14, PA15, PB3, PB4
    // SWD still works until the relevant pins are reconfigured. Proprietary
    // flash program (XHSC ISP) must be used to reflash afterwards.
    PORT_DebugPortSetting(ALL_DBG_PIN, Disable);

    // manage the system
    sched_main();

    // never get here
    for (;;)  ;
}
