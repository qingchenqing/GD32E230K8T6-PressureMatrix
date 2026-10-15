#include "gd32e23x.h"
#include "systick.h"

#define LED_HALF_PERIOD_MS 500U /* 亮 500 ms + 灭 500 ms，即每秒闪一次 */

/* 1 ms SysTick 中断：更新阻塞延时，并定时翻转 PB1 上的低电平有效指示灯。 */
void SysTick_Handler(void)
{
    static uint16_t led_elapsed_ms; /* 距离上次灯状态翻转经过的毫秒数 */

    delay_decrement();
    if(++led_elapsed_ms >= LED_HALF_PERIOD_MS) {
        led_elapsed_ms = 0U;
        gpio_bit_toggle(GPIOB, GPIO_PIN_1);
    }
}
