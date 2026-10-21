# GD32E230K8T6 16×8 压力矩阵 Keil 工程

打开 [Keil/PressureMatrix.uvprojx](Keil/PressureMatrix.uvprojx) 即可查看工程。项目自带兆易创新 GD32E23x 固件库 V2.5.0 的头文件、驱动、启动文件和 CMSIS 文件；编译、下载仍需在 Keil MDK 中安装 `GigaDevice.GD32E23x_DFP` 1.0.2 设备包。固件库来自 [GigaDevice 官方仓库](https://github.com/GigaDevice-GD32-MCU/GD32E23x_Firmware_Library)，设备包见 [官方 GD32 下载页](https://www.gd32mcu.com/en/download?kw=GD32E23x)。所需 Arm Compiler CMSIS 头文件补自 [Arm CMSIS_5 官方仓库](https://github.com/ARM-software/CMSIS_5)。第三方许可保留在 `Vendor/LICENSE`、`Vendor/CMSIS/LICENSE.TXT` 及各源文件中。

## Keil 编译环境

GD32E230K8T6 使用 Cortex-M23。工程设定为 Arm Compiler 6.13.1 和 `GigaDevice.GD32E23x_DFP` 1.0.2；在 Keil 中必须安装支持 Cortex-M23 的 Arm Compiler 6 和对应器件包，再于 **Options for Target → Target → Code Generation → ARM Compiler** 选中已安装的 Arm Compiler 6。若此处显示 **Use default compiler version**，当前 MDK 5.21a 会回退到 ARMCC 5.06，产生 C3903U。推荐升级到 MDK 5.27 或更新版本，并通过 Pack Installer 安装 GD32E23x DFP 1.0.2。如果下拉框中只有 Arm Compiler 6.5，也不能用于此芯片，需安装更新的 Arm Compiler 6。

本机现已安装 MDK 5.43a、Arm Compiler 6.24 和 GD32E23x DFP 1.0.2。工程已用 Arm Compiler 6.24 编译通过。若编译日志仍显示旧版 ARMCC 5.06，请关闭旧版 µVision，并从 `C:\Keil_v5\UV4\UV4.exe` 启动新版。**不要把工程 CPU 改成 Cortex-M0/M3 来绕过错误**，这与实际芯片内核不符。

## 已实现的功能

- 系统时钟采用内部 IRC8M，经 PLL 到 72 MHz；原理图未显示外部高速晶振，因此不依赖 HXTAL。
- 上电进入程序后将 PB1 配置为推挽输出，指示灯先亮 500 ms、再灭 500 ms，持续循环，每秒闪一次；PB1 引脚及低电平点亮的极性按用户实物接线确认。
- 74HC595 级联输出每次仅选通一行；切行时先关闭全部行并等待 10 ms，再选通目标行并等待 10 ms。
- ADC1～ADC8 按用户确认的顺序接 PA0～PA7，对应 GD32 ADC_IN0～ADC_IN7。ADC 为 12 位、12 MHz 时钟、55.5 周期采样时间，逐列软件触发采样。
- 每扫完 16 行，更新 12 位原始帧 `g_matrix_frame[16][8]` 和 0～255 显示帧 `g_matrix_frame_255[16][8]`，然后增加 `g_matrix_frame_number`。有效显示值由原始值右移 4 位得到，即 `0～4095 → 0～255`；ADC 超时在两份数组中都保留 `65535` 标记，并增加 `g_matrix_adc_errors`。
- USART0 TX（PA9，`U0_TX`）以 115200 8N1 发送数据。默认把 0～255 显示帧打包为 155 字节的 CRC8 实时矩阵数据包。Keil Watch 仍可查看两份数组，上位机可用 `tools/monitor.py` 接收。

## 原理图引脚映射

| 信号 | MCU 引脚 | 用途 |
| --- | --- | --- |
| `595_EN` | PA15 | 74HC595 `OE#`，低有效 |
| `595_RES` | PB3 | 74HC595 `MR#`，低有效 |
| `595_CLK` | PB4 | 移位时钟 `SHCP` |
| `595_RCK` | PB5 | 输出锁存时钟 `STCP` |
| `595_DAT` | PB7 | 第一片 74HC595 串行数据 `DS` |
| `ADC1`～`ADC8` | PA0～PA7 | 列 1～列 8 的 ADC 输入 |
| `U0_TX` | PA9 | 串口帧输出 |
| 上电指示灯 | PB1 | 低电平点亮；亮 500 ms、灭 500 ms，循环闪烁 |

两片 595 的输出位号与行号关系已写在 `App/matrix.c` 的 `row_bit_position` 表中：第一片 U1 的 Q0～Q7 对应 ROW1～ROW8；第二片 U3 的 Q7～Q0 对应 ROW9～ROW16。扫描时把 16 位数据最高位先送出，锁存后再采样。

## 使用方法

1. 按“Keil 编译环境”安装支持 Cortex-M23 的 MDK、Arm Compiler 6 和 GD32E23x DFP 1.0.2，再打开 `Keil/PressureMatrix.uvprojx`。
2. 选择 `GD32E230K8T6_PressureMatrix` 目标，编译并通过 SWD 下载。
3. 接好压力垫的 16 条行线与 8 条列线，按 PCB 电源入口供电，并确认板上 `VCC` 为 3.3 V。调试器中观察 `g_matrix_frame_255` 可看 0～255 显示值；`g_matrix_frame` 保留 0～4095 的原始 ADC 值。
4. 如需看串口数据，将板上 `U0_TX`（PA9）接 3.3 V USB 转串口模块的 RX，并将两者 GND 相连。上位机选择相应 COM 口，设置 115200、8 数据位、无校验、1 停止位。在 PC 端运行：

   ```powershell
   python -m pip install pyserial
   python tools/monitor.py COM5
   ```

   将 `COM5` 换成实际端口。默认接收 0～255 数据包并以十进制矩阵显示；若输出滚动太快，可加 `--interval 1`，每秒最多显示一帧，后台仍持续接收并校验所有帧。串口信号是 3.3 V TTL 电平，不是 RS-232 电平；只接收时不需要连接 MCU 的 RX。普通串口助手若以文本方式显示默认二进制数据包，会看到乱码，这是正常的。

## 上位机数据包格式

当前固件只发送文档规定的矩阵二进制协议，不再提供输出格式切换。每包固定 **155 字节**，发送周期为 **500 ms**：

| 字段 | 长度 | 内容 |
| --- | ---: | --- |
| 帧头 | 2 字节 | `5A A5` |
| 版本/INFO | 2 字节 | `01 10` |
| CMD | 2 字节 | `03 02`，实时数据 |
| SEQ | 2 字节 | 16 位流水号，大端，从 0 递增 |
| 整包长度 | 2 字节 | `00 9B`（十进制 155） |
| 点位值 | 128 字节 | 全部 128 点分 16 行，每行 8 点分别倒序，行序不变；每点 0～255 |
| 保留字节 | 16 字节 | 偏移 138～153 全部为 `00`，不携带错误位图 |
| CRC8 | 1 字节 | 多项式 `07`，初值 `00`；覆盖偏移 0～153 的全部字节 |

ADC 超时点发送值为 0，超时计数可在 Keil Watch 的 `g_matrix_adc_errors` 中查看。`tools/monitor.py` 校验 CRC8 和全零保留区，并将包行还原为扫描行，供 GUI 使用。SEQ 使用独立的 16 位发送计数器，从 0 开始，每发送一包加 1，65535 后回到 0，不随扫描帧数跳变。SEQ 为 0 的全零帧为 `5A A5 01 10 03 02 00 00 00 9B`，后跟 144 个 `00`，末尾 CRC8 为 `17`。先前约 334～338 的 12 位原始基线，打包后约为 20～21；这仍是电压采样码值，尚未换算为压力。

## 电脑端热力图与数字显示

`tools/matrix_viewer.py` 提供 16×8 实时窗口，复用 `monitor.py` 的帧头查找和 CRC 校验。它接收当前文档规定的 155 字节矩阵包。当前电脑桌面已有打包好的 `PressureMatrixViewer.exe`，可直接双击使用，无需另装 Python；源码版需要 Python、`pyserial` 和 Python 自带的 Tkinter。关闭占用串口的其他软件后，双击桌面 EXE 或工程中的 `Start_Matrix_Viewer.cmd`，也可运行：

```powershell
& "C:\Users\13659\AppData\Local\Programs\Python\Python313\python.exe" "D:\dacx\GD32E230K8T6\PressureMatrix_Keil\tools\matrix_viewer.py"
```

窗口中选择 `COM5` 并点“连接”；可切换“热力图”和“数字表格”，默认每秒刷新一次。热力图数字仍是 0～255 码值；“颜色上限”控制颜色范围，不改变采样值。空载时可点“记录当前零点”，勾选“显示相对零点的增量”后观察按压带来的变化。本协议的串口数据不包含错误位图，ADC 超时点显示为 0。单片机每 500 ms 发送一包；窗口只控制显示频率。

按最新要求，全部 128 字节点值分成 16 行，每行 8 点分别倒序，16 行的先后顺序不变。例如原点 1～8 改为 8～1，9～16 改为 16～9，依此类推至 128～121。最终行序仍为 Y7～Y0、Y8～Y15，每行改为 HC7～HC0；不再整体倒序后 64 点。帧头、流水号、长度和 16 字节保留区不因点值重排而改变，CRC8 按重排后的数据计算，整帧仍为 155 字节。

Keil Watch 可查看 `g_serial_tx_packet[155]`，其中偏移 0～9 应为 `5A A5 01 10 03 02 SEQ_H SEQ_L 00 9B`。此缓冲区就是交给 USART0 的完整二进制包，发送时不附加换行、文本或终止符。普通串口助手请使用 HEX 接收；十六进制显示中的两个字符代表 1 字节。

## 上板后需要确认

这份工程按给定原理图及已确认的 PA0～PA7 连接编写。固件读取的是原始 ADC 值，不直接换算牛顿。R16=30 kΩ、R22=2.7 kΩ 且 VCC=3.3 V 时，理论 `VBIAS ≈ 3.3 × 2.7 / (30 + 2.7) ≈ 0.272 V`，无压力时各列输出应接近这一基线；按当前跨阻连接，选中点压力增大时输出预计升高。请用单点逐行测试核对 595 输出与实际 ROW 顺序，再检查轻压、重压是否使运放输出落入 0～3.3 V 范围；反馈电阻、压力垫电阻范围和实际空载基线需由实物确认。

## 空载基线的行切换延时实验

当前固件切行时先关闭全部行并等待 10 ms，再选通目标行并等待 10 ms；每个 ADC 通道先丢弃 3 次转换，再取第 4 次作为有效值。这样可以给压力膜、列运放和 ADC 输入足够的放电及稳定时间。每帧扫描时间会明显增加，需要重新编译并烧录后在实物上观察假点变化。

比较时将垫子平放在同一位置，尽量消除折痕导致的局部接触，避免直射阳光变化；分别烧录两版，启动后等待读数稳定，再在原有上位机中记录当前零点。比较同一批空载点的原始值及相对零点增量，尤其关注上位机 ROW09～ROW13 的整行抬高现象。若 3 ms 明显改善，说明换行后未充分稳定可能是成因之一；若无明显变化，应优先检查垫子自身空载接触、VBIAS/电源稳定性及模拟前端，而不是继续盲目增加延时。该实验尚未在实物上验证效果。

本机没有连接目标板，下载、实际电压和扫描结果仍需在板上验证。
