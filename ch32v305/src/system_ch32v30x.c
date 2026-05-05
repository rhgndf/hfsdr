/********************************** (C) COPYRIGHT *******************************
* File Name          : system_ch32v30x.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2024/03/05
* Description        : CH32V30x Device Peripheral Access Layer System Source File.
*                      HSE crystal frequency is set via HSE_VALUE at 24 MHz.
*                      SYSCLK 144 MHz from HSE: PLL multiplier must match HSE (8M*18 or 24M*6).
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v30x.h"

// Uncomment to use internal HSI oscillator (always 144 MHz); leave commented for HSE.
// #define SYSCLK_USE_HSI

// Target system clock frequency when using HSE.
// Valid multipliers: 3–16, 18 (integer), or 6.5 (half-integer).
#ifndef SYSCLK_FREQ
#define SYSCLK_FREQ  144000000
#endif

#ifdef SYSCLK_USE_HSI
#if SYSCLK_FREQ != 144000000
#error HSI only supports 144MHz
#endif
#endif

// Detect 6.5x half-integer multiplier: 2*SYSCLK == 13*HSE
#define _PLL_IS_6_5    (2 * SYSCLK_FREQ == 13 * HSE_VALUE)
#define _PLL_MULT      (SYSCLK_FREQ / HSE_VALUE)

// D8C EXTEN encoding: 3–14 → (n-2)<<18, 15 → 14<<18, 16 → 15<<18, 18 → 0, 6.5 → 13<<18
#define _PLL_MULT_REG  ((uint32_t)(         \
    _PLL_IS_6_5      ? (13 << 18) :         \
    _PLL_MULT == 18  ? 0 :                  \
    _PLL_MULT == 15  ? (14 << 18) :         \
    _PLL_MULT == 16  ? (15 << 18) :         \
    ((_PLL_MULT - 2) << 18)))

static_assert(
    _PLL_IS_6_5 ||
    (_PLL_MULT >= 3 && _PLL_MULT <= 16) ||
    _PLL_MULT == 18,
    "SYSCLK_FREQ / HSE_VALUE must be 3..16, 18, or 6.5");
static_assert(
    _PLL_IS_6_5 || (SYSCLK_FREQ == HSE_VALUE * _PLL_MULT),
    "SYSCLK_FREQ must be an exact multiple of HSE_VALUE");

uint32_t SystemCoreClock = SYSCLK_FREQ;

__I uint8_t AHBPrescTable[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};

static void SetSysClock(void);

/*********************************************************************
 * @fn      SystemInit
 *
 * @brief   Setup the microcontroller system Initialize the Embedded Flash Interface,
 *        the PLL and update the SystemCoreClock variable.
 *
 * @return  none
 */
void SystemInit (void)
{
  RCC->CTLR |= (uint32_t)0x00000001;

  RCC->CFGR0 &= (uint32_t)0xF0FF0000;

  RCC->CTLR &= (uint32_t)0xFEF6FFFF;
  RCC->CTLR &= (uint32_t)0xFFFBFFFF;
  RCC->CFGR0 &= (uint32_t)0xFF00FFFF;

  RCC->CTLR &= (uint32_t)0xEBFFFFFF;
  RCC->INTR = 0x00FF0000;
  RCC->CFGR2 = 0x00000000;

  SetSysClock();

  /* Enable PVD brown-out detection at 2.7V (with ~200mV hysteresis per RM §2.2.2) */
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
  PWR_PVDLevelConfig(PWR_PVDLevel_2V7);
  PWR_PVDCmd(ENABLE);
}

/*********************************************************************
 * @fn      SystemCoreClockUpdate
 *
 * @brief   Update SystemCoreClock variable according to Clock Register Values.
 *
 * @return  none
 */
void SystemCoreClockUpdate (void)
{
  uint32_t tmp = 0, pllmull = 0, pllsource = 0;
  uint8_t Pll_6_5 = 0;
  uint8_t Pll2mull = 0;

  tmp = RCC->CFGR0 & RCC_SWS;

  switch (tmp)
  {
    case 0x00:
      SystemCoreClock = HSI_VALUE;
      break;
    case 0x04:
      SystemCoreClock = HSE_VALUE;
      break;
    case 0x08:
      pllmull = RCC->CFGR0 & RCC_PLLMULL;
      pllsource = RCC->CFGR0 & RCC_PLLSRC;
      pllmull = ( pllmull >> 18) + 2;

      if(pllmull == 2) pllmull = 18;
      if(pllmull == 15){
          pllmull = 13;  /* *6.5 */
          Pll_6_5 = 1;
      }
      if(pllmull == 16) pllmull = 15;
      if(pllmull == 17) pllmull = 16;

      if (pllsource == 0x00)
      {
          if(EXTEN->EXTEN_CTR & EXTEN_PLL_HSI_PRE) SystemCoreClock = HSI_VALUE * pllmull;
          else SystemCoreClock = (HSI_VALUE >> 1) * pllmull;
      }
      else
      {
          if(RCC->CFGR2 & (1<<16)){ /* PLL2 */
              SystemCoreClock = HSE_VALUE/(((RCC->CFGR2 & 0xF0)>>4) + 1);  /* PREDIV2 */

              Pll2mull = (uint8_t)((RCC->CFGR2 & 0xF00)>>8);

              if(Pll2mull == 0) SystemCoreClock = (SystemCoreClock * 5)>>1;
              else if(Pll2mull == 1) SystemCoreClock = (SystemCoreClock * 25)>>1;
              else if(Pll2mull == 15) SystemCoreClock = SystemCoreClock * 20;
              else  SystemCoreClock = SystemCoreClock * (Pll2mull + 2);

              SystemCoreClock = SystemCoreClock/((RCC->CFGR2 & 0xF) + 1);  /* PREDIV1 */
          }
          else{/* HSE */
              SystemCoreClock = HSE_VALUE/((RCC->CFGR2 & 0xF) + 1);  /* PREDIV1 */
          }

          SystemCoreClock = SystemCoreClock * pllmull;
      }


      if(Pll_6_5 == 1) SystemCoreClock = (SystemCoreClock / 2);

      break;
    default:
      SystemCoreClock = HSI_VALUE;
      break;
  }

  tmp = AHBPrescTable[((RCC->CFGR0 & RCC_HPRE) >> 4)];
  SystemCoreClock >>= tmp;
}

/*********************************************************************
 * @fn      SetSysClock_HSE
 *
 * @brief   Sets System clock frequency to SYSCLK_FREQ using HSE and PLL.
 *
 * @return  none
 */
static void SetSysClock_HSE(void)
{
  __IO uint32_t StartUpCounter = 0, HSEStatus = 0;

  RCC->CTLR |= ((uint32_t)RCC_HSEON);

  do
  {
    HSEStatus = RCC->CTLR & RCC_HSERDY;
    StartUpCounter++;
  } while((HSEStatus == 0) && (StartUpCounter != HSE_STARTUP_TIMEOUT));

  if ((RCC->CTLR & RCC_HSERDY) != RESET)
  {
    HSEStatus = (uint32_t)0x01;
  }
  else
  {
    HSEStatus = (uint32_t)0x00;
  }

  if (SYSCLK_FREQ > 144000000) {
    EXTEN->EXTEN_CTR |= EXTEN_LDO_TRIM;
  }

  if (HSEStatus == (uint32_t)0x01)
  {
    RCC->CFGR0 |= (uint32_t)RCC_HPRE_DIV1;
    RCC->CFGR0 |= (uint32_t)RCC_PPRE2_DIV1;
    RCC->CFGR0 |= (uint32_t)RCC_PPRE1_DIV1;

    RCC->CFGR0 &= (uint32_t)((uint32_t)~(RCC_PLLSRC | RCC_PLLXTPRE |
                                        RCC_PLLMULL));

    RCC->CFGR0 |= (uint32_t)(RCC_PLLSRC_HSE | RCC_PLLXTPRE_HSE | _PLL_MULT_REG);

    RCC->CTLR |= RCC_PLLON;
    while((RCC->CTLR & RCC_PLLRDY) == 0)
    {
    }
    RCC->CFGR0 &= (uint32_t)((uint32_t)~(RCC_SW));
    RCC->CFGR0 |= (uint32_t)RCC_SW_PLL;
    while ((RCC->CFGR0 & (uint32_t)RCC_SWS) != (uint32_t)0x08)
    {
    }
  }
}

/*********************************************************************
 * @fn      SetSysClock_HSI
 *
 * @brief   Sets System clock frequency to 144MHz using HSI and PLL.
 *
 * @return  none
 */
[[maybe_unused]] static void SetSysClock_HSI(void)
{
    EXTEN->EXTEN_CTR |= EXTEN_PLL_HSI_PRE;

    RCC->CFGR0 |= (uint32_t)RCC_HPRE_DIV1;
    RCC->CFGR0 |= (uint32_t)RCC_PPRE2_DIV1;
    RCC->CFGR0 |= (uint32_t)RCC_PPRE1_DIV1;

    RCC->CFGR0 &= (uint32_t)((uint32_t)~(RCC_PLLSRC | RCC_PLLXTPRE | RCC_PLLMULL));

    RCC->CFGR0 |= (uint32_t)(RCC_PLLSRC_HSI_Div2 | RCC_PLLMULL18_EXTEN);

    RCC->CTLR |= RCC_PLLON;
    while((RCC->CTLR & RCC_PLLRDY) == 0)
    {
    }
    RCC->CFGR0 &= (uint32_t)((uint32_t)~(RCC_SW));
    RCC->CFGR0 |= (uint32_t)RCC_SW_PLL;
    while ((RCC->CFGR0 & (uint32_t)RCC_SWS) != (uint32_t)0x08)
    {
    }
}

/*********************************************************************
 * @fn      SetSysClock
 *
 * @brief   Configures the System clock frequency, HCLK, PCLK2 and PCLK1 prescalers.
 *
 * @return  none
 */
static void SetSysClock(void)
{
#ifdef SYSCLK_USE_HSI
    SetSysClock_HSI();
#else
    SetSysClock_HSE();
#endif
}
