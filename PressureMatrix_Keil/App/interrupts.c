#include "gd32e23x.h"
#include "systick.h"
#include "serial.h"

/* 1 ms SysTick 中断：更新延时、毫秒计数和报警灯。 */
void SysTick_Handler(void)
{
    delay_decrement();
    systick_tick();
    serial_alarm_led_tick();
}
