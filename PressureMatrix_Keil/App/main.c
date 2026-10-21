#include <stdint.h>
#include "gd32e23x.h"
#include "matrix.h"
#include "serial.h"
#include "systick.h"

#define POWER_LED_PIN GPIO_PIN_1 /* PB1：上电指示灯，低电平点亮 */

/* 在应用初始化开始时点亮指示灯；后续由 SysTick 每 500 ms 翻转一次。 */
static void power_led_on(void)
{
    rcu_periph_clock_enable(RCU_GPIOB);
    gpio_bit_reset(GPIOB, POWER_LED_PIN); /* 先把输出锁存位置低，避免切换模式时闪灭 */
    gpio_mode_set(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, POWER_LED_PIN);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ, POWER_LED_PIN);
}

/* Keil Watch 可查看原始帧和 0～255 显示帧；二者都不是压力单位。 */
uint16_t g_matrix_frame[MATRIX_ROWS][MATRIX_COLS];     /* 12 位原始 ADC 值：0～4095 */
uint16_t g_matrix_frame_255[MATRIX_ROWS][MATRIX_COLS]; /* 显示值：0～255；65535 表示采样超时 */
volatile uint32_t g_matrix_frame_number;               /* 已完成的整帧扫描次数 */
volatile uint32_t g_matrix_adc_errors;                 /* ADC 转换超时累计次数 */

/* 保留 12 位原始采样，只把显示帧缩放到 8 位范围。 */
static void update_display_frame(void)
{
    uint8_t row;
    uint8_t col;

    for(row = 0U; row < MATRIX_ROWS; ++row) {
        for(col = 0U; col < MATRIX_COLS; ++col) {
            uint16_t raw = g_matrix_frame[row][col];
            g_matrix_frame_255[row][col] = (raw == MATRIX_ADC_INVALID)
                                             ? MATRIX_ADC_INVALID
                                             : (uint16_t)(raw >> 4);
        }
    }
}

int main(void)
{
    uint32_t next_packet_ms;
    uint16_t packet_sequence = 0U;
    power_led_on();  /* PB1 拉低，指示灯先亮 500 ms */
    systick_config(); /* 配置 1 ms 节拍，供行切换后的稳定延时使用 */
    matrix_init();    /* 初始化 74HC595 行驱动和 8 路 ADC 输入 */
    serial_init();    /* 初始化 PA9 上的 USART0 发送功能 */
    next_packet_ms = systick_millis();

    for(;;) {
        matrix_scan_frame(g_matrix_frame); /* 逐行选通，每行读取 8 列 */
        update_display_frame();            /* 12 位原始值转换成 0～255 的显示值 */
        ++g_matrix_frame_number;
        g_matrix_adc_errors = matrix_adc_error_count();
        if((int32_t)(systick_millis() - next_packet_ms) >= 0) {
            serial_send_frame(g_matrix_frame_255, packet_sequence);
            packet_sequence = (uint16_t)(packet_sequence + 1U);
            next_packet_ms += 500U;
        }
    }
}
