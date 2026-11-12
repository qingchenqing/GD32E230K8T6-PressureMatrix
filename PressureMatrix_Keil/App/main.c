/*
 * 报警初版调试  2026-11-12
 * PB1 检测 D19 前的 5 V，PA11 输出报警命令，PA12 使能 Q17。
 * 空闲时 PB0 绿灯常亮，中级 PA8 黄灯约 0.6 Hz，高级 PB6 红灯 2 Hz。
 * 断电后优先高级报警，暂停矩阵与 USART0，间歇检查电源。
 * 串口协议及异常中止行为见 serial.h。
 */
#include <stdint.h>
#include "gd32e23x.h"
#include "matrix.h"
#include "power.h"
#include "serial.h"
#include "systick.h"

/* IRC32K 标称 32 kHz：128 * (499 + 1) / 32000 = 2 s。
   实际超时时间随内部低速振荡器频率偏差变化。 */
#define WATCHDOG_RELOAD 499U

/* R71 必须接 D19 前的外部 5 V。R71/R69 均为 10 kΩ，PB1 约为输入电压一半。
   以 VDDA=3.3 V 估算，2600/2400 分别约为外部 4.19/3.87 V。 */
#define POWER_PRESENT_ADC 2600U
#define POWER_LOST_ADC    2400U
#define POWER_CONFIRM_SAMPLES 2U
#define POWER_RECHECK_MS 20U
#define POWER_ADC_STARTUP_MS 2U
#define POWER_RECOVERY_STABLE_MS 200U

typedef enum {
    POWER_WAIT_PRESENT,
    POWER_PRESENT,
    POWER_LOST
} power_state_t;

/* Watch 中可观察外部 5 V 的原始读数及当前供电状态。 */
volatile uint16_t g_power_adc;
static power_state_t power_state;

uint8_t power_supply_ok(void)
{
    static uint32_t last_sample_ms = UINT32_MAX;
    static uint8_t present_samples;
    uint32_t now = systick_millis();

    if(power_state == POWER_LOST) {
        return 0U;
    }
    if(now == last_sample_ms) {
        return 1U;
    }
    last_sample_ms = now;
    uint16_t adc = matrix_read_power_adc();
    g_power_adc = adc;

    /* 调试器单独供电时，先等两次正常输入读数才启用断电判断。 */
    if(power_state == POWER_WAIT_PRESENT) {
        if(adc != MATRIX_ADC_INVALID && adc >= POWER_PRESENT_ADC) {
            if(++present_samples >= POWER_CONFIRM_SAMPLES) {
                power_state = POWER_PRESENT;
            }
        } else {
            present_samples = 0U;
        }
    } else if(adc != MATRIX_ADC_INVALID && adc <= POWER_LOST_ADC) {
        /* 已确认供电后，一次有效低压读数即触发。 */
        power_state = POWER_LOST;
        return 0U;
    }
    return 1U;
}

static void power_sleep_ms(uint32_t duration)
{
    uint32_t start = systick_millis();
    while((uint32_t)(systick_millis() - start) < duration) {
        __DSB();
        __WFI(); /* SysTick 唤醒 CPU，继续推进红灯和毫秒计数。 */
    }
}

/* 从主循环接管断电报警。恢复电源持续 200 ms 后才恢复工作。 */
static void power_failure_run(void)
{
    uint32_t good_since = 0U;
    uint8_t checking_recovery = 0U;

    matrix_suspend();
    serial_suspend();
    serial_set_power_failure(1U);
    SCB->SCR &= ~(SCB_SCR_SLEEPDEEP_Msk | SCB_SCR_SLEEPONEXIT_Msk);
    fwdgt_counter_reload();

    for(;;) {
        power_sleep_ms(POWER_RECHECK_MS - POWER_ADC_STARTUP_MS);
        matrix_adc_set_enabled(1U);
        power_sleep_ms(POWER_ADC_STARTUP_MS);
        uint16_t adc = matrix_read_power_adc();
        matrix_adc_set_enabled(0U);
        g_power_adc = adc;
        fwdgt_counter_reload();

        if(adc == MATRIX_ADC_INVALID || adc < POWER_PRESENT_ADC) {
            checking_recovery = 0U;
            continue;
        }
        uint32_t now = systick_millis();
        if(!checking_recovery) {
            good_since = now;
            checking_recovery = 1U;
        } else if((uint32_t)(now - good_since) >= POWER_RECOVERY_STABLE_MS) {
            break;
        }
    }

    /* 停响时继续保持模块接地，随后恢复扫描与串口。 */
    serial_set_power_failure(0U);
    matrix_adc_set_enabled(1U);
    delay_1ms(POWER_ADC_STARTUP_MS);
    matrix_resume();
    serial_resume();
    power_state = POWER_PRESENT;
}

static void watchdog_init(void)
{
    /* fwdgt_config 内部启动独立看门狗，并配置分频、重装值及首次喂狗。 */
    if(SUCCESS != fwdgt_config(WATCHDOG_RELOAD, FWDGT_PSC_DIV128)) {
        /* 配置失败时停止执行，等待已启动的看门狗复位。 */
        for(;;) {
        }
    }
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
    uint32_t next_packet_ms;        /* 下一次计划发送的时间点，单位 ms，不是剩余等待时间 */
    uint16_t packet_sequence = 0U; /* 串口包流水号：从 0 开始，只在发送一包后递增 */
    watchdog_init(); /* 在外设初始化前启动，覆盖 ADC 校准等阻塞等待 */
    systick_config(); /* 配置 1 ms 节拍，供行切换后的稳定延时使用 */
    matrix_init();    /* 初始化 74HC595 行驱动和 8 路 ADC 输入 */
    serial_init();    /* 初始化 USART0、报警接口和状态灯，空闲绿灯常亮 */
    /* systick_millis() 返回自 SysTick 启动以来的毫秒计数。
       将首次发送时间设为当前时刻，因此第一次完整扫描结束后即可发送，
       不需要先等 500 ms；后续发送时间按 500 ms 间隔推进。 */
    /* 上电提示前先确认外部电源，正常运行时每轮扫描前复查。 */
    power_supply_ok();
    delay_1ms(1U);
    power_supply_ok();
    serial_startup_beep();
    next_packet_ms = systick_millis();

    for(;;) {
        if(!power_supply_ok()) {
            power_failure_run();
            /* 恢复后从新扫描的第一帧开始发送，跳过断电期间的计划。 */
            next_packet_ms = systick_millis();
            continue;
        }

        serial_process_commands();
        if(!matrix_scan_frame(g_matrix_frame) || !power_supply_ok()) {
            continue;
        }
        update_display_frame();
        ++g_matrix_frame_number;
        g_matrix_adc_errors = matrix_adc_error_count();

        if((int32_t)(systick_millis() - next_packet_ms) >= 0) {
            if(!serial_send_frame(g_matrix_frame_255, packet_sequence)) {
                continue;
            }
            packet_sequence = (uint16_t)(packet_sequence + 1U);
            next_packet_ms += 500U;
        }
        fwdgt_counter_reload();
    }
}
