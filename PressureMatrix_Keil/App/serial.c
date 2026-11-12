#include "serial.h"
#include "power.h"
#include "gd32e23x.h"
#include "systick.h"

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

/* 接收数据流：PA10 -> USART0_IRQHandler() -> rx_buf -> serial_process_commands()。
   中断只收数据，主循环再执行报警，避免在串口中断中阻塞输出波形。
   发送压力帧走 PA9，与 PA10 接收共用 USART0，串口参数均为 115200、8N1。 */

/* 64 字节环形缓冲区：rx_head 指向下次写入位置，rx_tail 指向下次读取位置。
   head == tail 表示空；保留一个空位区分满/空，所以最多排队 63 字节。
   中断写 head，主循环写 tail；volatile 使编译器每次读取实际共享变量。 */
static volatile uint8_t rx_buf[64], rx_head, rx_tail;
/* Watch 调试：收到的字节总数；缓冲满丢弃次数与硬件溢出事件的合计。
   g_serial_rx_dropped 是事件计数，不能精确表示硬件溢出时丢了多少字节。 */
volatile uint32_t g_serial_rx_count, g_serial_rx_dropped;
volatile uint8_t g_serial_rx_last; /* 最近收到的字节；HEX 发送 03 时应看到 0x03。 */

/* USART0 接收中断入口，名称与启动文件的中断向量一致。
   RBNE：接收数据寄存器非空；ORERR：上个字节未及时读取，又收到新字节。 */
void USART0_IRQHandler(void)
{
    uint32_t status = USART_STAT(USART0); /* 先读状态，随后读取接收数据。 */
    if(status & (USART_STAT_RBNE | USART_STAT_ORERR)) {
        uint8_t value = (uint8_t)usart_data_receive(USART0);
        if(status & USART_STAT_ORERR) ++g_serial_rx_dropped;
        if(status & USART_STAT_RBNE) {
            /* 与 63 相与，实现 0～63 循环；此写法依赖缓冲长度为 64。 */
            uint8_t next = (uint8_t)((rx_head + 1U) & 63U);
            g_serial_rx_last = value;
            ++g_serial_rx_count;
            /* 先存数据，再更新写指针；满时保留已排队数据，丢弃新字节。 */
            if(next != rx_tail) { rx_buf[rx_head] = value; rx_head = next; }
            else ++g_serial_rx_dropped;
        }
    }
}

/* 使用 SysTick 递减计数器等待微秒，不修改原来的 1 ms 系统节拍。
   必须先调用 systick_config()；处理计数器从 0 重装 LOAD 的回绕。
   中断保持开启，因此实际波形时间仍需示波器验证。 */
static void alarm_wait_us(uint32_t us)
{
    uint32_t left = (SystemCoreClock / 1000000U) * us; /* 等待的核心时钟周期数。 */
    uint32_t prev = SysTick->VAL;
    while(left) {
        uint32_t now = SysTick->VAL;
        uint32_t elapsed = prev >= now ? prev - now : prev + SysTick->LOAD + 1U - now;
        if(elapsed >= left) break;
        left -= elapsed;
        prev = now;
    }
}

/* YY0709 底层控制：cmd 为 PA11 输出的上升沿个数，不是串口发送函数。
   0 停止；1 BEEP；2 低级一次；3 中级持续；4 高级持续；5 高音量。
   先低电平清除状态，可从高级切回低级；非零命令最后保持高电平。 */
static void alarm_send_raw(uint8_t cmd)
{
    uint8_t i;
    gpio_bit_reset(GPIOA, GPIO_PIN_11);
    alarm_wait_us(500U); /* 低电平至少 400 us：停止声音并清除报警状态。 */
    for(i = 0U; i < cmd; ++i) {
        gpio_bit_set(GPIOA, GPIO_PIN_11); /* 产生一个上升沿。 */
        if(i + 1U < cmd) {
            /* 中间脉冲高、低各约 100 us，位于模块要求的 30～300 us 内。 */
            alarm_wait_us(100U);
            gpio_bit_reset(GPIOA, GPIO_PIN_11);
            alarm_wait_us(100U);
        }
    }
    if(cmd) alarm_wait_us(700U); /* 最后一段高电平至少 600 us，完成命令接收。 */
}

/* 三色指示灯低电平点亮、高电平熄灭。PA12 按键按下接地。 */
#define ALARM_KEY_PIN GPIO_PIN_12
#define ALARM_YELLOW_PIN GPIO_PIN_0 /* PB0 */
#define ALARM_RED_PIN GPIO_PIN_6    /* PB6 */
#define ALARM_GREEN_PIN GPIO_PIN_8  /* PA8 */
#define ALARM_MODULE_STARTUP_MS 200U
#define ALARM_STARTUP_BEEP_MS 2000U
#define ALARM_STARTUP_FEED_MS 250U
#define ALARM_KEY_DEBOUNCE_MS 20U
#define ALARM_KEY_DOUBLE_CLICK_MS 400U
#define ALARM_ACK_HOLDOFF_MS 20000U
#define POWER_ALARM_LIMIT_MS 60000U
#define ALARM_ONE_SHOT_LED_MS 200U /* 单次声音的灯态窗口，模块自行结束声音。 */
#define ALARM_YELLOW_HALF_MS 833U  /* 约 0.6 Hz，亮灭各 833 ms。 */
#define ALARM_RED_HALF_MS 250U     /* 2 Hz，亮灭各 250 ms。 */

/* 主循环选择灯状态，SysTick 负责闪烁和单次提示后的绿灯恢复。 */
static volatile uint8_t alarm_led_command;
static volatile uint16_t alarm_led_elapsed_ms;
static uint8_t power_failure_active; /* 仅主循环访问。 */
static volatile uint8_t alarm_key_ready;
static volatile uint8_t alarm_key_pressed; /* 消抖后的 PA12 状态，按下为 1。 */
static volatile uint8_t alarm_ack_pending; /* SysTick 记录双击，前台处理。 */
static uint8_t pending_alarm_command; /* 仍存在的持续报警条件：03、04 或 00。 */
static uint8_t alarm_acknowledged;
static uint32_t alarm_ack_ms;
static uint32_t power_alarm_start_ms;

static void alarm_key_tick(void)
{
    static uint8_t debounce_ms;
    static uint16_t click_window_ms;

    if(!alarm_key_ready) {
        return;
    }
    if(click_window_ms != 0U) {
        --click_window_ms;
    }

    uint8_t pressed = (RESET == gpio_input_bit_get(GPIOA, ALARM_KEY_PIN));
    if(pressed == alarm_key_pressed) {
        debounce_ms = 0U;
        return;
    }
    if(++debounce_ms < ALARM_KEY_DEBOUNCE_MS) {
        return;
    }
    debounce_ms = 0U;
    alarm_key_pressed = pressed;
    if(!pressed) {
        return;
    }
    /* 只计消抖后的按下沿，两次按下之间必须松开。 */
    if(click_window_ms != 0U) {
        click_window_ms = 0U;
        alarm_ack_pending = 1U;
    } else {
        click_window_ms = ALARM_KEY_DOUBLE_CLICK_MS;
    }
}

void serial_alarm_led_tick(void)
{
    uint8_t command = alarm_led_command;

    alarm_key_tick();
    if(command == 1U || command == 2U) {
        if(++alarm_led_elapsed_ms >= ALARM_ONE_SHOT_LED_MS) {
            alarm_led_elapsed_ms = 0U;
            alarm_led_command = 0U;
            gpio_bit_reset(GPIOA, ALARM_GREEN_PIN);
        }
    } else if(command == 3U) {
        if(++alarm_led_elapsed_ms >= ALARM_YELLOW_HALF_MS) {
            alarm_led_elapsed_ms = 0U;
            gpio_bit_toggle(GPIOB, ALARM_YELLOW_PIN);
        }
    } else if(command == 4U) {
        if(++alarm_led_elapsed_ms >= ALARM_RED_HALF_MS) {
            alarm_led_elapsed_ms = 0U;
            gpio_bit_toggle(GPIOB, ALARM_RED_PIN);
        }
    }
}

static void alarm_led_stop(void)
{
    /* 先让 SysTick 停止翻转，再清零计时并将黄、红灯置高熄灭。 */
    alarm_led_command = 0U;
    alarm_led_elapsed_ms = 0U;
    gpio_bit_set(GPIOB, ALARM_YELLOW_PIN);
    gpio_bit_set(GPIOB, ALARM_RED_PIN);
}

/* 按命令更新实际声光输出，持续报警条件由调用者另行保存。 */
static void alarm_command(uint8_t cmd)
{
    alarm_led_stop();
    if(cmd == 0U) {
        /* PA11 持续拉低停止声音。 */
        alarm_send_raw(0U);
        gpio_bit_reset(GPIOA, ALARM_GREEN_PIN);
        return;
    }

    if(cmd <= 4U) {
        gpio_bit_set(GPIOA, ALARM_GREEN_PIN);
    }
    if(cmd <= 4U) {
        alarm_send_raw(5U); /* 所有声音保持高音量。 */
        alarm_send_raw(cmd);
    } else {
        alarm_send_raw(5U); /* 05～07 统一设置高音量。 */
    }

    /* 中高级报警灯从点亮开始，单次报警由节拍计时恢复绿灯。 */
    if(cmd == 3U) {
        gpio_bit_reset(GPIOB, ALARM_YELLOW_PIN);
    } else if(cmd == 4U) {
        gpio_bit_reset(GPIOB, ALARM_RED_PIN);
    }
    if(cmd <= 4U) {
        alarm_led_command = cmd;
    } else {
        /* 设置音量会清除旧报警，恢复空闲绿灯。 */
        gpio_bit_reset(GPIOA, ALARM_GREEN_PIN);
    }
}

/* 断电只报警一次。正常供电时，双击暂停 20 秒后复核持续条件。 */
void serial_service_alarm(void)
{
    uint32_t now = systick_millis();
    uint8_t double_click = alarm_ack_pending;
    if(double_click) {
        alarm_ack_pending = 0U;
    }

    if(power_failure_active) {
        if(pending_alarm_command != 0U &&
           (double_click || (uint32_t)(now - power_alarm_start_ms) >= POWER_ALARM_LIMIT_MS)) {
            /* 结束本次声光，保留断电状态，主循环继续低功耗复查 PB1。 */
            pending_alarm_command = 0U;
            alarm_command(0U);
        }
        return;
    }

    if(double_click && !alarm_acknowledged) {
        if(alarm_led_command != 0U || pending_alarm_command != 0U) {
            alarm_acknowledged = 1U;
            alarm_ack_ms = now;
        }
        alarm_command(0U);
    }
    if(alarm_acknowledged && (uint32_t)(now - alarm_ack_ms) >= ALARM_ACK_HOLDOFF_MS) {
        alarm_acknowledged = 0U;
        if(pending_alarm_command != 0U) {
            alarm_command(pending_alarm_command);
        }
    }
}

/* 新断电优先启动高级报警，供电恢复则清除断电报警和等待状态。 */
void serial_set_power_failure(uint8_t active)
{
    power_failure_active = active;
    rx_tail = rx_head;
    alarm_ack_pending = 0U;
    alarm_acknowledged = 0U;
    pending_alarm_command = active ? 4U : 0U;
    alarm_command(pending_alarm_command);
    if(active) {
        power_alarm_start_ms = systick_millis(); /* 从声光启动后开始计时。 */
    }
}

/* 主循环每次调用最多处理一个字节，不在这里等待新数据。
   调试协议为单字节 HEX：00 停止，01 BEEP，02 低级，03 中级，04 高级，
   05～07 均设高音量；08～FF 忽略。无帧头、校验、回车换行或 ACK。
   注意：HEX 发送 03 才是 0x03；文本发送“03”会得到 0x30、0x33，均忽略。
   03、04 持续发声，需要另发 00 停止；连续发 03 00 可能很快停止听不到。 */
void serial_process_commands(void)
{
    /* 断电报警期间丢弃串口命令，避免 00 或其他等级覆盖高级报警。 */
    if(power_failure_active) {
        rx_tail = rx_head;
        return;
    }
    if(rx_head != rx_tail) { /* 读写指针不同，说明有待处理的数据。 */
        uint8_t cmd = rx_buf[rx_tail];
        rx_tail = (uint8_t)((rx_tail + 1U) & 63U); /* 取出后立即释放该位置。 */
        if(cmd <= 7U) {
            /* 暂停期间仍接收状态更新，00 清除待重报条件。单次声响不补播。 */
            pending_alarm_command = (cmd == 3U || cmd == 4U) ? cmd : 0U;
            if(!alarm_acknowledged) {
                alarm_command(cmd);
            }
        }
    }
}

/* 上电提示窗口内继续检查电源和按键，断电时交给主循环接管。 */
static uint8_t startup_wait(uint32_t duration)
{
    uint32_t now = systick_millis();
    uint32_t start = now;
    uint32_t last_feed = now;

    while((uint32_t)(now - start) < duration) {
        if(!power_supply_ok()) {
            return 0U;
        }
        serial_service_alarm();
        if(alarm_acknowledged) {
            return 0U;
        }
        /* 提示窗口长于看门狗实际超时下限，在前台检查正常后分段喂狗。 */
        if((uint32_t)(now - last_feed) >= ALARM_STARTUP_FEED_MS) {
            fwdgt_counter_reload();
            last_feed = now;
        }
        __WFI();
        now = systick_millis();
    }
    serial_service_alarm();
    return !alarm_acknowledged && power_supply_ok();
}

void serial_startup_beep(void)
{
    fwdgt_counter_reload();
    delay_1ms(ALARM_MODULE_STARTUP_MS); /* 模块共地，上电后等待其初始化。 */
    if(!power_supply_ok()) {
        return;
    }

    /* 只触发一次 BEEP，保留 2 秒播放窗口。实际发声长度由模块决定。 */
    alarm_command(1U);
    if(startup_wait(ALARM_STARTUP_BEEP_MS)) {
        alarm_command(0U);
    }
    fwdgt_counter_reload();
}

/* 初始化 PA9 TX、PA10 RX、PA11 报警输出、PA12 按键和三路状态灯。
   外部串口模块 TX 接 PA10，RX 接 PA9，并与单片机共地，使用 3.3 V TTL。 */
void serial_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA); /* GPIO 配置前先使能端口时钟。 */
    /* 输出前预置电平：PA11 置低，PA8 绿灯亮，PB0 黄灯及 PB6 红灯灭。 */
    rcu_periph_clock_enable(RCU_GPIOB);
    gpio_bit_reset(GPIOA, GPIO_PIN_11 | ALARM_GREEN_PIN);
    gpio_bit_set(GPIOB, ALARM_RED_PIN | ALARM_YELLOW_PIN);
    gpio_mode_set(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  GPIO_PIN_11 | ALARM_GREEN_PIN);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ,
                            GPIO_PIN_11 | ALARM_GREEN_PIN);
    gpio_mode_set(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, ALARM_RED_PIN | ALARM_YELLOW_PIN);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ,
                            ALARM_RED_PIN | ALARM_YELLOW_PIN);
    gpio_mode_set(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, ALARM_KEY_PIN);
    alarm_key_ready = 1U;
    serial_resume();
}

void serial_suspend(void)
{
    NVIC_DisableIRQ(USART0_IRQn);
    usart_interrupt_disable(USART0, USART_INT_RBNE);
    usart_disable(USART0);
    rcu_periph_clock_disable(RCU_USART0);
    NVIC_ClearPendingIRQ(USART0_IRQn);
    rx_tail = rx_head;

    /* TX 保持空闲高电平，RX 禁用数字输入，关闭串口唤醒源。 */
    gpio_bit_set(GPIOA, GPIO_PIN_9);
    gpio_mode_set(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_PIN_9);
    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO_PIN_10);
}

void serial_resume(void)
{
    rcu_periph_clock_enable(RCU_USART0);
    /* PA9 选择 AF1 复用为 USART0_TX，推挽输出。 */
    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_9);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_9);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_9);

    /* PA10 选择 AF1 复用为 USART0_RX，上拉保持空闲高电平。 */
    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_10);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_10);
    usart_deinit(USART0);
    /* 115200、8N1：8 位数据、1 位停止位、无校验。 */
    usart_word_length_set(USART0, USART_WL_8BIT);
    usart_stop_bit_set(USART0, USART_STB_1BIT);
    usart_parity_config(USART0, USART_PM_NONE);
    usart_baudrate_set(USART0, 115200U);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE); /* 开启压力数据发送。 */
    usart_receive_config(USART0, USART_RECEIVE_ENABLE); /* 开启报警命令接收。 */
    usart_enable(USART0);
    /* 同时开启外设接收中断源和 NVIC 中断入口，收到字节才会进入 ISR。
       USART0 优先级 1，低于本工程优先级 0 的 SysTick。 */
    usart_interrupt_enable(USART0, USART_INT_RBNE);
    NVIC_SetPriority(USART0_IRQn, 1U);
    NVIC_EnableIRQ(USART0_IRQn);
}

/* 上行帧布局与点序见 serial.h。frame 由 main.c 缩放为 0～255。
   只有 USART0 的最后一个停止位发出后才返回成功。 */
uint8_t serial_send_frame(const uint16_t frame[MATRIX_ROWS][MATRIX_COLS], uint16_t sequence)
{
    uint8_t row;
    uint8_t col;
    uint8_t source_row;
    uint8_t crc = 0U;
    uint16_t i;
    uint16_t value;
    uint8_t temporary;

    /* 偏移 0～5：固定帧头与协议字节；不是接收端的报警命令。 */
    g_serial_tx_packet[0] = 0x5AU;
    g_serial_tx_packet[1] = 0xA5U;
    g_serial_tx_packet[2] = 0x01U;
    g_serial_tx_packet[3] = 0x10U;
    g_serial_tx_packet[4] = 0x03U;
    g_serial_tx_packet[5] = 0x02U;
    /* 偏移 6～7：16 位流水号，大端发送，即高字节在前。 */
    g_serial_tx_packet[6] = (uint8_t)(sequence >> 8);
    g_serial_tx_packet[7] = (uint8_t)sequence;
    /* 偏移 8～9：00 9B，与固定总帧长 155 对应。 */
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
    /* 保留上述逐行倒序，再整体反转后 64 点（整帧偏移 74～137）。
       此步骤后，后 8 行反转行序、恢复原列序。 */
    for(i = 0U; i < 32U; ++i) {
        temporary = g_serial_tx_packet[74U + i];
        g_serial_tx_packet[74U + i] = g_serial_tx_packet[137U - i];
        g_serial_tx_packet[137U - i] = temporary;
    }
    /* 最新要求：在当前结果上，仅将后 8 行各自的 8 个点再次倒序。
       前 64 点保持当前顺序，后 8 行的行序保持上一步结果。 */
    for(row = 8U; row < MATRIX_ROWS; ++row) {
        i = (uint16_t)(10U + row * MATRIX_COLS);
        for(col = 0U; col < MATRIX_COLS / 2U; ++col) {
            temporary = g_serial_tx_packet[i + col];
            g_serial_tx_packet[i + col] = g_serial_tx_packet[i + MATRIX_COLS - 1U - col];
            g_serial_tx_packet[i + MATRIX_COLS - 1U - col] = temporary;
        }
    }
    /* 上述重排后的最终点序：行 Y7～Y0、Y15～Y8，每行列 HC7～HC0。
       有效 ADC 值在 main.c 右移 4 位；超时点编码 00，与有效零值无法区分。 */
    /* 增加了按键配网功能。将 PA12 按键状态写到偏移 138，供上位机配网逻辑读取。
       消抖后按下为 01，释放为 00。组帧时取一次快照，并纳入 CRC8。 */
    g_serial_tx_packet[138] = alarm_key_pressed;
    /* 偏移 139～153 仍为保留区，全零。 */
    for(i = 139U; i < 154U; ++i) {
        g_serial_tx_packet[i] = 0U;
    }
    /* 偏移 0～153 全部参与 CRC8，包含 5A A5 帧头。 */
    for(i = 0U; i < 154U; ++i) {
        crc = crc8_byte(crc, g_serial_tx_packet[i]);
    }
    g_serial_tx_packet[154] = crc;

    /* 仅发送这 155 个二进制字节，不添加文本、换行或终止符。 */
    for(i = 0U; i < SERIAL_MATRIX_FRAME_LEN; ++i) {
        while(RESET == usart_flag_get(USART0, USART_FLAG_TBE)) {
            if(!power_supply_ok()) {
                return 0U;
            }
        }
        if(!power_supply_ok()) {
            return 0U;
        }
        usart_data_transmit(USART0, g_serial_tx_packet[i]);
    }
    /* 等最后一个停止位发出后才推进流水号。 */
    while(RESET == usart_flag_get(USART0, USART_FLAG_TC)) {
        if(!power_supply_ok()) {
            return 0U;
        }
    }
    return 1U;
}
