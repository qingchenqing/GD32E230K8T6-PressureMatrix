#ifndef PRESSURE_SERIAL_H
#define PRESSURE_SERIAL_H

#include <stdint.h>
#include "matrix.h"

#define SERIAL_MATRIX_FRAME_LEN 155U

/* 实际发送缓冲区，供 Keil Watch 核对。 */
extern uint8_t g_serial_tx_packet[SERIAL_MATRIX_FRAME_LEN];

void serial_init(void); /* PA9 / USART0，115200、8N1 */
/* 0～255 显示帧，独立 16 位发送流水号；固定 155 字节 CRC8 协议。 */
void serial_send_frame(const uint16_t frame[MATRIX_ROWS][MATRIX_COLS], uint16_t sequence);

#endif
