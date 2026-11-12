#ifndef PRESSURE_MATRIX_H
#define PRESSURE_MATRIX_H

#include <stdint.h>

#define MATRIX_ROWS 16U          /* 压力垫行数 */
#define MATRIX_COLS 8U           /* 压力垫列数 */
#define MATRIX_ADC_INVALID 0xFFFFU /* ADC 超时时的无效标记，正常 12 位值最大为 4095 */

/* 原理图上的 ADC1～ADC8 已确认依次连接 PA0～PA7。 */
void matrix_init(void); /* 初始化行驱动 GPIO 和 ADC */
uint8_t matrix_scan_frame(uint16_t frame[MATRIX_ROWS][MATRIX_COLS]); /* 完整帧返回 1，断电中止返回 0。 */
void matrix_suspend(void);
void matrix_resume(void);
void matrix_adc_set_enabled(uint8_t enabled);
uint16_t matrix_read_power_adc(void); /* PB1/ADC_IN9 的原始读数，超时返回 0xFFFF。 */
uint32_t matrix_adc_error_count(void); /* 返回启动以来的 ADC 超时累计次数 */

#endif
