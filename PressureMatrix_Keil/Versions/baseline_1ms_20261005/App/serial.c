#include "serial.h"
#include "gd32e23x.h"

/* USART0 TX 经 PA9 接到原理图的 U0_TX，电平为 3.3 V TTL，格式为 115200 8N1。 */
static void send_byte(uint8_t value)
{
    while(RESET == usart_flag_get(USART0, USART_FLAG_TBE)) {
    }
    usart_data_transmit(USART0, value);
}

#if SERIAL_OUTPUT_MODE == 0U
/* 不依赖 printf，逐位发送无符号十进制数。 */
static void send_decimal(uint32_t value)
{
    uint8_t digits[10];
    uint8_t count = 0U;

    do {
        digits[count++] = (uint8_t)(value % 10U);
        value /= 10U;
    } while(value != 0U);

    while(count != 0U) {
        send_byte((uint8_t)('0' + digits[--count]));
    }
}

static void send_text(const char *text)
{
    while(*text != '\0') {
        send_byte((uint8_t)*text++);
    }
}
#elif SERIAL_OUTPUT_MODE == 1U
/* 按小端序发送一个 16 位采样值，并把两个字节累加到校验和。 */
static void send_u16(uint16_t value, uint16_t *sum)
{
    uint8_t low = (uint8_t)value;         /* 低 8 位，先发送 */
    uint8_t high = (uint8_t)(value >> 8); /* 高 8 位，后发送 */
    send_byte(low);
    send_byte(high);
    *sum = (uint16_t)(*sum + low + high);
}
#elif SERIAL_OUTPUT_MODE == 2U
/* CRC-16/CCITT-FALSE：初值 FFFF，多项式 1021；从包类型算到无效位图。 */
static uint16_t crc16_byte(uint16_t crc, uint8_t value)
{
    uint8_t bit;

    crc ^= (uint16_t)value << 8;
    for(bit = 0U; bit < 8U; ++bit) {
        crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                               : (uint16_t)(crc << 1);
    }
    return crc;
}

static void send_checked(uint8_t value, uint16_t *crc)
{
    send_byte(value);
    *crc = crc16_byte(*crc, value);
}
#else
#error Unsupported SERIAL_OUTPUT_MODE
#endif

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

/* 模式 0：FRAME,帧号和 ROW01～ROW16 十进制文本。
   模式 1：旧版 A5 5A | 帧号 | 128 个原始 ADC 值 | 累加校验。
   模式 2：A5 5A | 类型 01 | 长度 94 | 帧号 4B | 值 128B | 无效位图 16B | CRC16 2B。
   值按行优先排列；每行一个无效位图字节，bit0～bit7 对应 COL1～COL8。 */
void serial_send_frame(const uint16_t frame[MATRIX_ROWS][MATRIX_COLS], uint32_t frame_number)
{
    uint8_t row;       /* 当前发送的行下标：0～15 */
    uint8_t col;       /* 当前发送的列下标：0～7 */
#if SERIAL_OUTPUT_MODE == 0U
    send_text("FRAME,");
    send_decimal(frame_number);
    send_text("\r\n");
    for(row = 0U; row < MATRIX_ROWS; ++row) {
        send_text("ROW");
        if(row < 9U) {
            send_byte('0');
        }
        send_decimal((uint32_t)row + 1U);
        for(col = 0U; col < MATRIX_COLS; ++col) {
            send_byte(',');
            send_decimal(frame[row][col]);
        }
        send_text("\r\n");
    }
    send_text("\r\n");
#elif SERIAL_OUTPUT_MODE == 1U
    uint16_t sum = 0U; /* 本帧的 16 位字节累加校验和 */
    uint8_t i;         /* 帧号的字节下标：0～3 */

    send_byte(0xA5U);
    send_byte(0x5AU);
    for(i = 0U; i < 4U; ++i) {
        uint8_t data = (uint8_t)(frame_number >> (8U * i)); /* 当前帧号字节，低位先发 */
        send_byte(data);
        sum = (uint16_t)(sum + data);
    }
    for(row = 0U; row < MATRIX_ROWS; ++row) {
        for(col = 0U; col < MATRIX_COLS; ++col) {
            send_u16(frame[row][col], &sum);
        }
    }
    send_byte((uint8_t)sum);
    send_byte((uint8_t)(sum >> 8));
#elif SERIAL_OUTPUT_MODE == 2U
    uint16_t crc = 0xFFFFU;
    uint8_t i;
    uint8_t invalid_bits;

    send_byte(0xA5U);
    send_byte(0x5AU);
    send_checked(0x01U, &crc); /* 包类型：8 位显示值 */
    send_checked(148U, &crc);  /* 4 字节帧号 + 128 字节值 + 16 字节无效位图 */
    for(i = 0U; i < 4U; ++i) {
        send_checked((uint8_t)(frame_number >> (8U * i)), &crc);
    }
    for(row = 0U; row < MATRIX_ROWS; ++row) {
        for(col = 0U; col < MATRIX_COLS; ++col) {
            uint16_t value = frame[row][col];
            send_checked((value == MATRIX_ADC_INVALID) ? 0U : (uint8_t)value, &crc);
        }
    }
    for(row = 0U; row < MATRIX_ROWS; ++row) {
        invalid_bits = 0U;
        for(col = 0U; col < MATRIX_COLS; ++col) {
            if(frame[row][col] == MATRIX_ADC_INVALID) {
                invalid_bits |= (uint8_t)(1U << col);
            }
        }
        send_checked(invalid_bits, &crc);
    }
    send_byte((uint8_t)crc);        /* CRC 低字节先发 */
    send_byte((uint8_t)(crc >> 8));
#endif
}
