#ifndef PRESSURE_MATRIX_H
#define PRESSURE_MATRIX_H

#include <stdint.h>

#define MATRIX_ROWS 16U          /* 压力垫行数 */
#define MATRIX_COLS 8U           /* 压力垫列数 */
#define MATRIX_ADC_INVALID 0xFFFFU /* ADC 超时时的无效标记，正常 12 位值最大为 4095 */

/* 原理图上的 ADC1～ADC8 已确认依次连接 PA0～PA7。 */
void matrix_init(void); /* 初始化行驱动 GPIO 和 ADC */
void matrix_scan_frame(uint16_t frame[MATRIX_ROWS][MATRIX_COLS]); /* 扫描并写入一帧原始 ADC 值 */
uint32_t matrix_adc_error_count(void); /* 返回启动以来的 ADC 超时累计次数 */

#endif
