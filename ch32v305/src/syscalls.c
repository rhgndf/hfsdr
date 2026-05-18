/********************************** (C) COPYRIGHT  *******************************
* File Name          : debug.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : This file contains all the functions prototypes for UART
*                      Printf , Delay functions.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "debug.h"
#include "hw/usb.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

static uint32_t s_ticks_per_us = 0U;

/*********************************************************************
 * @fn      Delay_Init
 *
 * @brief   Initializes Delay Funcation.
 *
 * @return  none
 */
void Delay_Init(void)
{
    s_ticks_per_us = SystemCoreClock / 1000000U;
    if(s_ticks_per_us == 0U)
    {
        s_ticks_per_us = 1U;
    }

    SysTick->CTLR = 0;
    SysTick->SR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = 0xFFFFFFFFFFFFFFFFULL;
    SysTick->CTLR = 0x0FU;
}

/*********************************************************************
 * @fn      Delay_Us
 *
 * @brief   Microsecond Delay Time.
 *
 * @param   n - Microsecond number.
 *
 * @return  None
 */
void Delay_Us(uint32_t n)
{
    uint64_t start_tick;
    uint64_t wait_ticks;

    if(n == 0U)
    {
        return;
    }

    start_tick = SysTick->CNT;
    wait_ticks = (uint64_t)n * (uint64_t)s_ticks_per_us;
    if(wait_ticks == 0U)
    {
        wait_ticks = 1U;
    }

    while((SysTick->CNT - start_tick) < wait_ticks)
    {
    }
}

/*********************************************************************
 * @fn      Delay_Ms
 *
 * @brief   Millisecond Delay Time.
 *
 * @param   n - Millisecond number.
 *
 * @return  None
 */
void Delay_Ms(uint32_t n)
{
    uint64_t start_tick;
    uint64_t wait_ticks;

    if(n == 0U)
    {
        return;
    }

    start_tick = SysTick->CNT;
    wait_ticks = ((uint64_t)SystemCoreClock * (uint64_t)n) / 1000ULL;
    if(wait_ticks == 0U)
    {
        wait_ticks = 1U;
    }

    while((SysTick->CNT - start_tick) < wait_ticks)
    {
    }
}

/*********************************************************************
 * @fn      _write
 *
 * @brief   Support Printf Function over USB CDC.
 *
 * @param   *buf - USB send data.
 *          size - Data length
 *
 * @return  size: Data length written
 */
__attribute__((used)) int _write(int fd, char *buf, int size)
{
    (void)fd;

    if((buf == 0) || (size <= 0))
    {
        return 0;
    }

    return (int)usb_send_data((uint8_t const *)buf, (uint32_t)size);
}

__attribute__((used)) int _close(int fd)
{
    (void)fd;
    errno = EBADF;
    return -1;
}

__attribute__((used)) int _fstat(int fd, struct stat *st)
{
    (void)fd;

    if(st == 0)
    {
        errno = EINVAL;
        return -1;
    }

    st->st_mode = S_IFCHR;
    return 0;
}

__attribute__((used)) int _getpid(void)
{
    return 1;
}

__attribute__((used)) int _isatty(int fd)
{
    (void)fd;
    return 1;
}

__attribute__((used)) int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

__attribute__((used)) off_t _lseek(int fd, off_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    errno = ESPIPE;
    return (off_t)-1;
}

__attribute__((used)) int _read(int fd, char *buf, int size)
{
    (void)fd;
    (void)buf;
    (void)size;
    return 0;
}

/*********************************************************************
 * @fn      _sbrk
 *
 * @brief   Change the spatial position of data segment.
 *
 * @return  size: Data length
 */
__attribute__((used)) void *_sbrk(ptrdiff_t incr)
{
    extern char _end[];
    extern char _heap_end[];
    static char *curbrk = _end;

    if ((curbrk + incr < _end) || (curbrk + incr > _heap_end))
    {
        return (void *)-1;
    }

    curbrk += incr;
    return curbrk - incr;
}
