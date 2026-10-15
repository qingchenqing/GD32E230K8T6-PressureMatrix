#include "matrix.h"
#include "gd32e23x.h"
#include "systick.h"

/* 原理图中两片级联 74HC595 的控制引脚。 */
#define PIN_595_EN   GPIO_PIN_15  /* PA15：OE#，低电平允许输出 */
#define PIN_595_RES  GPIO_PIN_3   /* PB3：MR#，低电平清除移位寄存器 */
#define PIN_595_CLK  GPIO_PIN_4   /* PB4：SHCP，移位时钟 */
#define PIN_595_RCK  GPIO_PIN_5   /* PB5：STCP，输出锁存时钟 */
#define PIN_595_DAT  GPIO_PIN_7   /* PB7：第一片 595 的串行数据 DS */

#define ADC_CONVERSION_TIMEOUT 200000UL /* 等待转换完成的最大轮询次数，不是毫秒数 */
//#define ROW_RELEASE_MS 10U /* 全部行关闭后的放电时间 */
//#define ROW_SETTLE_MS 10U /* 目标行选通后的稳定时间 */
//#define ADC_DISCARD_COUNT 3U /* 每次换列后丢弃的 ADC 转换次数 */

#define ROW_RELEASE_MS     0U
#define ROW_SETTLE_MS      3U
#define ADC_DISCARD_COUNT  0U

static uint32_t adc_errors; /* ADC 转换超时累计次数，由 matrix_adc_error_count() 读取 */

/* 行号到 16 位移位数据位号的映射：U1 的 Q0～Q7 对应 ROW1～ROW8；
   U3 的 Q7～Q0 对应 ROW9～ROW16。U1 的 Q7S 连接 U3 的 DS，发送时高位先行。 */
static const uint8_t row_bit_position[MATRIX_ROWS] = {
    0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
    15U, 14U, 13U, 12U, 11U, 10U, 9U, 8U
};

/* 列号到 ADC 通道的映射：第 1～8 列依次接 PA0～PA7 / ADC_IN0～ADC_IN7。 */
static const uint8_t column_adc_channel[MATRIX_COLS] = {
    ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7
};

/* 将行选通位图移入两片 595，再锁存到输出端；value 为 0 时关闭全部行。 */
static void shift_595(uint16_t value)
{
    uint8_t bit; /* 当前发送的位序号：0 对应最高位 bit15 */

    gpio_bit_reset(GPIOB, PIN_595_RCK);
    for(bit = 0U; bit < 16U; ++bit) {
        if((value & (uint16_t)(0x8000U >> bit)) != 0U) {
            gpio_bit_set(GPIOB, PIN_595_DAT);
        } else {
            gpio_bit_reset(GPIOB, PIN_595_DAT);
        }
        gpio_bit_set(GPIOB, PIN_595_CLK);
        gpio_bit_reset(GPIOB, PIN_595_CLK);
    }
    gpio_bit_set(GPIOB, PIN_595_RCK);
    gpio_bit_reset(GPIOB, PIN_595_RCK);
}

/* 初始化 595 控制引脚；先锁存全零，再开启输出，避免上电误选通。 */
static void row_driver_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);

    /* GPIO 切成输出前先写安全电平：关闭输出、释放复位、时钟和数据置低。 */
    gpio_bit_set(GPIOA, PIN_595_EN);
    gpio_bit_set(GPIOB, PIN_595_RES);
    gpio_bit_reset(GPIOB, PIN_595_CLK | PIN_595_RCK | PIN_595_DAT);

    gpio_mode_set(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, PIN_595_EN);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ, PIN_595_EN);
    gpio_mode_set(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  PIN_595_RES | PIN_595_CLK | PIN_595_RCK | PIN_595_DAT);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ,
                            PIN_595_RES | PIN_595_CLK | PIN_595_RCK | PIN_595_DAT);

    shift_595(0U);
    gpio_bit_reset(GPIOA, PIN_595_EN); /* 全零已锁存，此时才允许 Q 端输出 */
}

/* 初始化 PA0～PA7 模拟输入和单次软件触发 ADC。 */
static void adc_input_init(void)
{
    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE,
                  GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
                  GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);

    rcu_periph_clock_enable(RCU_ADC);
    rcu_adc_clock_config(RCU_ADCCK_APB2_DIV6); /* 72 MHz 主频 / 6 = 12 MHz ADC 时钟 */
    adc_deinit();
    adc_resolution_config(ADC_RESOLUTION_12B);
    adc_data_alignment_config(ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(ADC_REGULAR_CHANNEL, 1U);
    adc_external_trigger_source_config(ADC_REGULAR_CHANNEL, ADC_EXTTRIG_REGULAR_NONE);
    adc_external_trigger_config(ADC_REGULAR_CHANNEL, ENABLE);
    adc_enable();
    delay_1ms(1U);
    adc_calibration_enable();
}

static uint16_t adc_convert_once(void)
{
    uint32_t timeout = ADC_CONVERSION_TIMEOUT; /* 本次转换剩余的轮询次数 */

    adc_flag_clear(ADC_FLAG_EOC);
    adc_software_trigger_enable(ADC_REGULAR_CHANNEL);
    while(RESET == adc_flag_get(ADC_FLAG_EOC)) {
        if(--timeout == 0U) {
            ++adc_errors;
            return MATRIX_ADC_INVALID;
        }
    }
    adc_flag_clear(ADC_FLAG_EOC);
    return adc_regular_data_read();
}

/* 读取一个 ADC 通道；先丢弃 3 次转换，再返回第 4 次结果。 */
static uint16_t sample_column(uint8_t channel)
{
    uint8_t discard;

    adc_regular_channel_config(0U, channel, ADC_SAMPLETIME_55POINT5);
    for(discard = 0U; discard < ADC_DISCARD_COUNT; ++discard) {
        if(adc_convert_once() == MATRIX_ADC_INVALID) {
            return MATRIX_ADC_INVALID;
        }
    }
    return adc_convert_once();
}

void matrix_init(void)
{
    adc_errors = 0U;
    row_driver_init();
    adc_input_init();
}

/* 扫描顺序为 ROW1/COL1 → ROW16/COL8，结果按 frame[row][col] 保存。 */
void matrix_scan_frame(uint16_t frame[MATRIX_ROWS][MATRIX_COLS])
{
    uint8_t row; /* 当前行下标：0～15 */
    uint8_t col; /* 当前列下标：0～7 */

    for(row = 0U; row < MATRIX_ROWS; ++row) {
//        shift_595(0U);
//        delay_1ms(ROW_RELEASE_MS); /* 所有行关闭，给压力膜和放大器放电 */
        shift_595((uint16_t)(1U << row_bit_position[row]));
        delay_1ms(ROW_SETTLE_MS); /* 目标行选通后等待压力膜、放大器和 ADC 稳定 */
        for(col = 0U; col < MATRIX_COLS; ++col) {
            frame[row][col] = sample_column(column_adc_channel[col]);
        }
    }
    shift_595(0U); /* 两帧之间不选通任何行 */
}

/* 给主循环提供可观察的累计超时次数。 */
uint32_t matrix_adc_error_count(void)
{
    return adc_errors;
}
