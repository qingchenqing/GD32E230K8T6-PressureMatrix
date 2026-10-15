/*!
    \file    systick.c
    \brief   SysTick 毫秒节拍和阻塞延时实现

    \version 2026-02-04, V2.5.0, firmware for GD32E23x
*/

/*
    Copyright (c) 2026, GigaDevice Semiconductor Inc.

    Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this
       list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its contributors
       may be used to endorse or promote products derived from this software without
       specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
*/

#include "gd32e23x.h"
#include "systick.h"

volatile static uint32_t delay; /* 剩余延时毫秒数，由主循环和 SysTick 中断共享 */
volatile static uint32_t ticks;

/*!
    \brief      配置 1 ms SysTick 节拍
    \param[in]  无
    \param[out] 无
    \retval     无
*/
void systick_config(void)
{
    ticks = 0U;
    /* 1 kHz 中断频率，对应每 1 ms 更新一次延时计数。 */
    if(SysTick_Config(SystemCoreClock / 1000U)) {
        /* 时钟参数无效时停在这里，防止延时逻辑在错误时基下运行。 */
        while(1) {
        }
    }
    /* 设置 SysTick 中断优先级。 */
    NVIC_SetPriority(SysTick_IRQn, 0x00U);
}

uint32_t systick_millis(void)
{
    return ticks;
}

void systick_tick(void)
{
    ++ticks;
}

/*!
    \brief      按毫秒数执行阻塞延时
    \param[in]  count: 延时毫秒数
    \param[out] 无
    \retval     无
*/
void delay_1ms(uint32_t count)
{
    delay = count; /* count：需要等待的毫秒数 */

    while(0U != delay) {
    }
}

/*!
    \brief      在每次 SysTick 中断中递减延时计数
    \param[in]  无
    \param[out] 无
    \retval     无
*/
void delay_decrement(void)
{
    /* 每次中断只减一次；到 0 后保持为 0，解除 delay_1ms() 的等待。 */
    if(0U != delay) {
        delay--;
    }
}
