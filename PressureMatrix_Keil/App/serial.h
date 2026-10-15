#ifndef PRESSURE_SERIAL_H
#define PRESSURE_SERIAL_H

#include <stdint.h>
#include "matrix.h"

/* 0：十进制文本；1：旧版 12 位原始值二进制帧；2：统一通信协议矩阵包。 */
#define SERIAL_OUTPUT_MODE 2U

void serial_init(void); /* 配置 PA9 / USART0，115200、8N1 */
/* 模式 0/2 传入 0～255 显示帧，模式 1 传入 12 位原始帧；模式 2 每 500 ms 调用一次。 */
void serial_send_frame(const uint16_t frame[MATRIX_ROWS][MATRIX_COLS], uint32_t frame_number);

#endif
