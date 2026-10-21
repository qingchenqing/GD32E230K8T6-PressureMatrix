#include "serial.h"
#include "gd32e23x.h"

/* USART0 TX 经 PA9 接到原理图的 U0_TX，电平为 3.3 V TTL，格式为 115200 8N1。 */
static void send_byte(uint8_t value)
{
    while(RESET == usart_flag_get(USART0, USART_FLAG_TBE)) {
    }
    usart_data_transmit(USART0, value);
}

/* Keil Watch 可直接查看这 155 个实际待发送字节。 */
uint8_t g_serial_tx_packet[SERIAL_MATRIX_FRAME_LEN];

/* 文档规定 CRC8：初值 00，多项式 07，MSB-first，无反转或结果异或。 */
static uint8_t crc8_byte(uint8_t crc, uint8_t value)
{
    uint8_t bit;

    crc ^= value;
    for(bit = 0U; bit < 8U; ++bit) {
        crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x07U)
                            : (uint8_t)(crc << 1);
    }
    return crc;
}

/* 仅启用串口发送；接收端由 USB 转串口模块或其他主机提供。 */
void serial_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART0);
    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_9);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_9);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_9);

    usart_deinit(USART0);
    usart_word_length_set(USART0, USART_WL_8BIT);
    usart_stop_bit_set(USART0, USART_STB_1BIT);
    usart_parity_config(USART0, USART_PM_NONE);
    usart_baudrate_set(USART0, 115200U);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_enable(USART0);
}

/* 固定文档协议：10 字节头 + 128 字节点值 + 16 字节保留零 + 1 字节 CRC8。
   frame 已由 main.c 缩放为 0～255；序号与扫描帧数独立。 */
void serial_send_frame(const uint16_t frame[MATRIX_ROWS][MATRIX_COLS], uint16_t sequence)
{
    uint8_t row;
    uint8_t col;
    uint8_t source_row;
    uint8_t crc = 0U;
    uint16_t i;
    uint16_t value;
    uint8_t temporary;

    g_serial_tx_packet[0] = 0x5AU;
    g_serial_tx_packet[1] = 0xA5U;
    g_serial_tx_packet[2] = 0x01U;
    g_serial_tx_packet[3] = 0x10U;
    g_serial_tx_packet[4] = 0x03U;
    g_serial_tx_packet[5] = 0x02U;
    g_serial_tx_packet[6] = (uint8_t)(sequence >> 8);
    g_serial_tx_packet[7] = (uint8_t)sequence;
    g_serial_tx_packet[8] = 0x00U;
    g_serial_tx_packet[9] = 0x9BU;

    /* 包行 Y7～Y0、Y8～Y15，每行依次 HC0～HC7。 */
    for(row = 0U; row < MATRIX_ROWS; ++row) {
        source_row = (row < 8U) ? (uint8_t)(7U - row) : row;
        for(col = 0U; col < MATRIX_COLS; ++col) {
            value = frame[source_row][col];
            g_serial_tx_packet[10U + row * MATRIX_COLS + col] =
                (value == MATRIX_ADC_INVALID) ? 0U : (uint8_t)value;
        }
    }
    /* 全部 16 行分别反转每行的 8 个点，保持行的先后顺序。
       例如点 1～8 -> 8～1，点 9～16 -> 16～9，依此类推。 */
    for(row = 0U; row < MATRIX_ROWS; ++row) {
        for(col = 0U; col < MATRIX_COLS / 2U; ++col) {
            i = (uint16_t)(10U + row * MATRIX_COLS);
            temporary = g_serial_tx_packet[i + col];
            g_serial_tx_packet[i + col] = g_serial_tx_packet[i + MATRIX_COLS - 1U - col];
            g_serial_tx_packet[i + MATRIX_COLS - 1U - col] = temporary;
        }
    }
    /* 偏移 138～153 为保留区，全零，不存放超时位图。 */
    for(i = 138U; i < 154U; ++i) {
        g_serial_tx_packet[i] = 0U;
    }
    /* 偏移 0～153 全部参与 CRC8，包含 5A A5 帧头。 */
    for(i = 0U; i < 154U; ++i) {
        crc = crc8_byte(crc, g_serial_tx_packet[i]);
    }
    g_serial_tx_packet[154] = crc;

    /* 仅发送这 155 个二进制字节，不添加文本、换行或终止符。 */
    for(i = 0U; i < SERIAL_MATRIX_FRAME_LEN; ++i) {
        send_byte(g_serial_tx_packet[i]);
    }
}
