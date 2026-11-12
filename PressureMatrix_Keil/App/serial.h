#ifndef PRESSURE_SERIAL_H
#define PRESSURE_SERIAL_H

#include <stdint.h>
#include "matrix.h"

/*
 * 报警初版调试串口协议
 *
 * USART0：115200、8N1、3.3 V TTL，PA9 为 TX，PA10 为 RX。
 *
 * 下行（上位机 -> 板）：单字节 HEX 命令，无帧头、校验、结束符或应答。
 * 00 停止，PA11 保持低电平，已使能的 PA12 保持高电平，绿灯恢复。
 * 01 BEEP 一次，02 低级报警一次，声音由模块自行结束。
 * 03 中级持续报警（PA8 黄灯约 0.6 Hz），04 高级持续报警（PB6 红灯 2 Hz）。
 * 05～07 均设高音量并清除旧报警，08～FF 忽略。发声一律高音量。
 * 串口助手勾选 HEX 发送，文本“03”对应 30 33，会被忽略。
 *
 * 上行（板 -> 上位机）：约每 500 ms 发送 155 字节，偏移从 0 起。
 *   0～5    固定 5A A5 01 10 03 02
 *   6～7    16 位流水号，大端，首包为 0，完整发出后加一并回绕
 *   8～9    固定 00 9B（总帧长 155）
 *   10～137 128 个点值，行 Y7～Y0、Y15～Y8，每行列 HC7～HC0
 *   138～153 保留，全为 00
 *   154     CRC8：初值 00，多项式 07，最高位先行，不反转、无结果异或
 * CRC8 覆盖偏移 0～153，包含帧头。有效点值为 ADC 原值右移 4 位，
 * 采样超时编码 00。帧后不附加换行符。
 *
 * 断电触发高级报警并关闭 USART0，发送中的帧可能被截断。
 * 上位机应按帧头、155 字节长度及 CRC8 重新同步。
 * 电源稳定恢复后从新扫描的完整帧继续发送，沿用下一包流水号。
 */
#define SERIAL_MATRIX_FRAME_LEN 155U

/* 实际发送缓冲区，供 Keil Watch 核对。 */
extern uint8_t g_serial_tx_packet[SERIAL_MATRIX_FRAME_LEN];

/* PA9 发送、PA10 接收，115200、8N1；PA11 输出报警控制脉冲。 */
void serial_init(void);
void serial_suspend(void);
void serial_resume(void);
/* 发送 155 字节压力帧。完整发出返回 1，断电中止返回 0。 */
uint8_t serial_send_frame(const uint16_t frame[MATRIX_ROWS][MATRIX_COLS], uint16_t sequence);

/* 主循环调用：每次处理一个 HEX 命令 00～07，无应答；03/04 需发 00 停止。 */
void serial_process_commands(void);
/* 初始化完成后调用一次：高音量 BEEP、中级、高级，各播放 200 ms 后停止。 */
void serial_startup_beep(void);
/* SysTick 每 1 ms 调用，更新报警灯闪烁和单次提示的绿灯状态。 */
void serial_alarm_led_tick(void);
/* 主循环在电源状态变化时调用，断电优先执行高级报警。 */
void serial_set_power_failure(uint8_t active);
#endif
