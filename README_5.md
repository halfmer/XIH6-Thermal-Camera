# README_5 - USART1 调试日志与 UART4 热成像二进制流拆分

日期：2026-07-05

前置：README_4 记录了把 CH340C/UART4 提到 2 Mbps 并承载热成像 RAW16 帧流。本篇修正一个实际调试问题：UART4 同时输出 `[LEP] OK c_raw=...` 文本和热成像二进制帧，会污染上位机帧解析。因此本次先只改 STM32 固件，把调试日志切回 USART1，UART4/CH340 保持 2 Mbps 二进制专用。

## 1. 当前串口职责

| 串口 | 波特率 | 引脚 | 当前职责 |
|---|---:|---|---|
| USART1 | 115200 | PA9=TX, PA10=RX | 调试日志：启动 banner、SD、LEPTON CCI/VoSPI 诊断、`[LEP] OK c_raw=...` |
| UART4 | 2000000 | PA0/PA1，运行时开启 CR2.SWAP | CH340C 到 PC，上位机热成像 RAW16 二进制帧流；只接收 `'S'/'P'` 控制字节 |
| UART5 | 115200 | PB13=TX, PB12=RX | 预留 ESP32 链路，本次未改 |

注意：`Core/Src/usart.c` 里 `MX_UART4_Init()` 仍是 115200，这是 CubeMX 初始值；实际运行时 `Lepton_Stream_Init(&huart4)` 会覆盖为 2000000 并打开 RX/TX SWAP。

## 2. 本次 STM32 改动

修改 `Core/Src/main.c`：

- `It's mygo!!!!!` 启动文本：`huart4` -> `huart1`
- `[RST] ...` reset-cause banner：`huart4` -> `huart1`
- `SD_UART_Print()`：`huart4` -> `huart1`
- 删除 `SD_UART_Print()` 对 `Lepton_Stream_Active()` 的静音判断，因为日志已不走 UART4
- 启动提示改成：`UART4/CH340 2Mbps binary only`，明确调试日志在 USART1

修改 `Drivers/PER/LEPTON/lepton_stream.c`：

- 注释改为 UART4 是 host command + binary frame 通道，调试日志在 USART1，避免后续误判。

未改：

- `Lepton_Stream_Init()` 仍然配置 UART4 为 2 Mbps + CR2.SWAP + 1 字节中断接收
- `Lepton_Stream_SendFrame()` 的 AA55 RAW16 帧协议未改
- 上位机本轮不改，等固件确认后再处理

## 3. 为什么这样能保证 2 Mbps 热成像完整帧

README_4 的帧协议一帧长度为：

```text
13 B header + 38400 B payload + 2 B checksum = 38415 B
```

UART 8N1 下 2 Mbps 等效 200000 B/s，单帧发送约：

```text
38415 * 10 / 2000000 = 0.192 s
```

所以理论上 2 Mbps 可以承载约 5.2 fps 的裸传输。当前固件是阻塞发送加采集串行，实际目标约 3 fps。关键不是再压缩，而是保证 UART4 上没有任何文本插入二进制流。本次拆分后：

- UART4 上电后不再打印 `It's mygo!!!!!`
- UART4 不再打印 `[RST]`、`[SD]`、`[LEP] OK`、`VoSPI diag`
- UART4 只有上位机发送 `'S'` 后才开始输出 AA55 帧
- UART4 收到 `'P'` 后停止输出二进制帧
- USART1 独立输出调试日志，不会污染上位机解析

## 4. 上板检查方式

1. USART1 串口助手：115200 8N1，应看到启动日志和周期性的 `[LEP] OK c_raw=...`。
2. CH340/UART4 串口助手：2000000 8N1，上电默认应基本没有可读文本。
3. 在 CH340/UART4 手发 `S`：应出现连续二进制数据；手发 `P`：二进制停止。
4. 如果用 Qt 上位机，当前上位机仍按 README_4 逻辑自动发 `S/P`，理论上可以收到干净二进制帧；若解析仍失败，下一步再改上位机。

## 5. 风险和后续

- USART1 在底板上是否引出，要按实物连接确认；如果没引出，需要外接转接线或改到实际可用调试口。
- UART4 现在是热成像专用，不适合再用普通串口助手看启动日志。
- 若 2 Mbps 下上位机错误帧持续增长，先确认 UART4 上没有文本污染，再考虑两端同步降到 1.5 Mbps 或后续做 UART4 DMA 发送。

## 6. 上位机改动记录

用户确认 STM32 固件行为后，继续修改 `ESP_UART_Host/ESP_UART_Windows`：

- `mainwindow.cpp/.h` 新增 `setSerialThermalStream(bool enabled)`，统一管理 UART4 的 `'S'/'P'` 控制。
- 切换到“热成像(串口)”或打开串口时，先 `FrameParser::reset()`，清空串口输入残留，再发送 `'S'`。
- 离开热成像串口模式或关闭串口时发送 `'P'`，等待写出，并清空输入残留。
- 在“热成像(串口)”模式下禁止普通发送区向串口写文本，避免人为把字节插进 UART4 视频链路。
- `热成像传输协议.md` 修正 STM32 当前链路为 `UART4/CH340C @ 2000000`，USART1 只做调试日志；帧总长修正为 `38415 B`。

当前设计意图：

- USART1：调试日志、`[LEP] OK c_raw=...`
- UART4/CH340：只在上位机发 `'S'` 后输出 AA55 RAW16 二进制帧，发 `'P'` 后停止
- 上位机 FrameParser 只吃 UART4 的二进制帧，不再混入启动日志或 LEPTON 文本诊断

## 7. Qt 路径与构建验证

按 Qt CMake skill 检查后，`ESP_UART_Windows/CMakeLists.txt` 本身符合 Qt 6 单目标 Widgets 工程写法：

- `find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets SerialPort Network)`
- `qt_standard_project_setup(REQUIRES 6.8)`
- `qt_add_executable(esp_uart ...)`
- `target_link_libraries(... PRIVATE Qt6::Widgets Qt6::SerialPort Qt6::Network)`

实际验证发现：Qt/CMake 工具链在中文路径 `ESP_UART上位机/...` 下会直接崩溃，退出码 `-1073740791`，没有正常 CMake 错误输出。因此已将目录改名为：

```text
ESP_UART_Host/
```

新的验证命令：

```bat
E:\Qt\Qt_Creato_All\6.10.1\mingw_64\bin\qt-cmake.bat -S . -B build_qt_ascii -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=E:/Qt/Qt_Creato_All/Tools/mingw1310_64/bin/mingw32-make.exe -DCMAKE_CXX_COMPILER=E:/Qt/Qt_Creato_All/Tools/mingw1310_64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release
E:\Qt\Cmake\bin\cmake.exe --build build_qt_ascii --config Release
```

验证结果：

- CMake configure：通过
- CMake build：通过，`[100%] Built target esp_uart`
- 新产物已同步到 `ESP_UART_Host/ESP_UART_Windows/build/esp_uart.exe`
- 新产物已同步到 `ESP_UART_Host/ESP_UART_Windows/deploy/esp_uart.exe`

`WrapVulkanHeaders` 缺失只是 Qt 配置探测提示，本工程是 Widgets + SerialPort + Network，不依赖 Vulkan。

补充问题：直接运行 `build_qt_ascii/esp_uart.exe` 曾报：

```text
无法定位程序输入点 _Z20qResourceFeatureZlibv 于动态链接库
```

原因是 `build_qt_ascii` 初始只有 exe，没有本地 Qt DLL。Windows 会从 PATH 或其他目录加载不匹配的 `Qt6Core.dll`，导致入口点不一致。已执行 `windeployqt` 给 `build_qt_ascii`、`build`、`deploy` 三个目录部署匹配的 Qt 6.10.1 运行库，并给 `build_qt_ascii` 补 `qt.conf`：

```text
[Paths]
Prefix = .
Plugins = .
```

验证：从 `build_qt_ascii` 启动 exe 后 2 秒仍正常运行，说明动态库入口和 Qt 平台插件加载已经通过。

## 8. Checksum mismatch 调试

现象：热成像(串口)模式持续显示：

```text
协议错误: Checksum mismatch: got ... expected ...
```

USART1 调试日志正常，只能说明 MCU 主循环、Lepton 采集和调试口正常；它不能证明 UART4/CH340 的 2 Mbps 二进制链路无误。持续 checksum mismatch 且数值变化，优先判断为 UART4 实际字节流损坏或两端 checksum 规则不一致。

本次处理：

- STM32：`Lepton_Stream_Init()` 之后强制把 UART4 的 PA0/PA1 重新配置为 `GPIO_SPEED_FREQ_VERY_HIGH`。CubeMX 生成的 `usart.c` 默认是 `GPIO_SPEED_FREQ_LOW`，2 Mbps 下边沿裕量不足，容易出现 payload 随机错字节。
- Qt：`FrameParser` 的错误信息扩展为 `gotBE/gotLE/expected/expectedNoSync/diff`，并兼容 checksum BE/LE、包含/不包含 sync 的旧规则。若新上位机仍报 mismatch，观察这几个值：
  - `gotBE == expected`：当前协议完全匹配，应出图。
  - `gotBE == expectedNoSync` 或 `gotLE == expected`：说明之前是 checksum 范围/大小端不一致，新 parser 会兼容显示。
  - 四个值都对不上且每帧变化：基本是 UART4 2 Mbps 链路有错字节/丢字节，继续查 GPIO 速度、线长、GND、CH340C 品质，或两端降到 1.5 Mbps 验证。

验证状态：

- STM32 Keil rebuild：`0 Error(s), 0 Warning(s)`，新 HEX 已生成。
- Qt 新构建目录：`ESP_UART_Host/ESP_UART_Windows/build_qt_fix/esp_uart.exe`
- 推荐测试产物：`ESP_UART_Host/ESP_UART_Windows/deploy/esp_uart.exe`

## 9. STM32 发送与 Qt 接收自洽性仿真

针对“可能 STM32 发送和上位机接收逻辑不自洽”的假设，增加了 Qt 侧自测目标：

```text
ESP_UART_Host/ESP_UART_Windows/build_qt_fix/frameparser_selftest.exe
```

自测按 STM32 `Lepton_Stream_SendFrame()` 的逻辑构造完整 RAW16 帧：

- sync = `AA 55`
- type = `01`
- frame_id/width/height/payload_len 全部 big-endian
- payload = `160 * 120 * 2 = 38400 B`
- checksum = 从 sync 到 payload 末尾逐字节累加，uint16 big-endian 写入

覆盖场景：

- 3 帧一次性送入 parser
- 1 字节一片模拟最极端串口分片
- 1 到 997 字节随机分片
- 前面带垃圾字节
- payload 内部故意放入 `AA 55`
- payload 单字节翻转
- payload 单字节丢失

结果：

```text
frameparser_selftest.exe -> selftest_exit=0
```

结论：

- 在字节流完整的情况下，STM32 当前组帧规则和 Qt `FrameParser` 的缓存/解析逻辑是自洽的。
- 串口 `readAll()` 把一帧拆成很多块不会导致解析失败；parser 的 `m_buffer` 会正确攒够 `38415 B` 再校验。
- payload 内部出现 `AA 55` 不会误触发重同步，因为 parser 在 `ReadingPayload` 状态按固定 payload 长度读取。
- 当人为制造字节翻转或丢字节时，会出现 checksum mismatch，且 `gotBE/gotLE/expected/expectedNoSync/diff` 会变化。这和当前实机现象一致，说明优先怀疑 UART4/CH340 2 Mbps 链路字节错误/丢失。

同时优化了 Qt `frameparser.cpp` 的错误恢复：

- checksum mismatch 后不再盲目固定丢弃 `38415 B`。
- parser 会在预计帧边界附近寻找下一个 `AA 55`，找到后跳到下一个同步头重新解析。
- 这不会修好已经损坏的帧，但可以避免丢 1 字节后把下一帧同步头吞掉，减少连续错误。

当前 Qt 主程序已重新构建：

```text
ESP_UART_Host/ESP_UART_Windows/build_qt_fix/esp_uart.exe
```

注意：调试时 `deploy/esp_uart.exe` 可能仍在运行，Windows 会锁定 exe；若要把新版同步到 `deploy/`，先关闭正在运行的上位机。

## 10. 2 Mbps 串口流模型与显示链路复测

针对“可能上位机接收到串口后没有正确储存和解析”的怀疑，继续增强了 Qt 自测。

### 10.1 Parser 的 2 Mbps 串口模型

`frameparser_selftest.exe` 现在不只是一次性喂完整帧，还按 UART 8N1 的实际吞吐模拟：

```text
2 Mbps / 10 bit = 200000 byte/s
38415 byte/frame ~= 192 ms/frame
```

测试方式：

- 生成与 STM32 `Lepton_Stream_SendFrame()` 完全一致的 38415 B RAW16 帧。
- 按 0.1 ms 到 20 ms 的随机 readyRead 间隔切片，换算为每次约 20 B 到 4000 B 的接收批次。
- 连续 100 个随机 seed 反复测试干净链路。
- 注入 1 字节翻转，检查能报 checksum mismatch 且后续帧能继续解析。
- 注入 1 字节丢失，检查能报错并重新同步到后续合法帧。
- 注入 1 字节插入，检查能报错并重新同步到后续合法帧。

本次还修正了坏帧恢复策略：

- checksum mismatch 后不只找裸 `AA 55`。
- 改为查找 `AA 55 + 合法 header`：type、width、height、payload_len 都必须合理。
- 这样 payload 里偶然出现 `AA 55` 不会被误当成下一帧头。

验证结果：

```text
frameparser_selftest_exit=0
thermalwidget_selftest_exit=0
deploy/esp_uart.exe 已同步为 build_qt_fix/esp_uart.exe
```

结论：在字节流完整时，2 Mbps 的分片节奏不会导致 parser 错。若实机仍然 `gotBE/gotLE/expected/expectedNoSync/diff` 全乱跳，更符合 UART4/CH340 链路上出现字节翻转、丢字节或插字节。

### 10.2 显示数据链路自测

针对“收到正确数据但可能无法显示”的怀疑，增加/调整了显示链路自测：

```text
thermalwidget_selftest.exe
```

测试内容：

- 构造一帧 160x120 的 big-endian RAW16 payload。
- 按 `MainWindow::onFrameReceived()` 的逻辑转成主机端 `quint16`。
- 检查首尾像素、平均值、最小值、最大值。
- 调用 `ColorMap::apply()` 生成 QImage。
- 检查 QImage 尺寸为 160x120，且采样颜色数量足够，证明不是空白图。

验证结果：

```text
thermalwidget_selftest_exit=0
```

源码检查补充：

- `MainWindow::onFrameReceived()` 会把 payload 的 big-endian u16 转成主机端 `quint16`。
- `ThermalWidget::displayFrame()` 会立刻把传入 rawData `memcpy` 到 `m_rawData`，并立刻生成 `m_image`，因此局部 `hostData` 指针不会悬空。
- 当前能确认：只要 parser 发出 `frameReady`，显示数据路径具备生成非空热图的能力。

限制：Windows/Qt 的 offscreen QWidget render 在当前命令行环境会卡住，所以本轮没有用离屏窗口截图验证 `paintEvent()`，而是验证了显示前的数据转换和 `ColorMap` 成图链路。

## 11. 串口诊断日志与 STM32 发送侧对照

针对实机仍然 checksum 参数乱跳的问题，新增独立诊断日志，避免只看界面最后一条错误。

### 11.1 Qt 上位机日志文件

新版 Qt 在 exe 同目录生成：

```text
serial_diag.log
```

规则：

- 最大保存 `1 MiB`。
- 超过后从文件前面按行丢弃旧内容，新内容继续追加。
- 打开串口成功时清空旧日志并重新开始。
- 单纯关闭串口不会清空日志，会保留最后一次会话内容，方便检查。
- 日志先在内存滚动，约 250 ms 合并写盘；关闭串口和程序退出会强制落盘。

记录内容：

- `open ...`：串口号、波特率、数据位、停止位、校验、DTR/RTS、当前模式。
- `rx_chunk ...`：每次 `readAll()` 的分片长度、累计 RX 字节数、分片内 `AA55` 数量、头尾十六进制采样。
- `frame_ok ...`：成功帧的 `frame_id`、payload 长度、尺寸、是否跳帧。
- `parse_error ...`：界面看到的协议错误。
- `parser checksum_bad ...`：gotBE/gotLE/expected/expectedNoSync/diff、parser 缓冲大小、预计帧长、丢弃长度、下一帧同步位置、帧头/帧尾采样。
- `parser bad_header ...`：坏 header 的 type/width/height/len 和头部采样。

重点看法：

- 如果 `rx_chunk` 长度长期很小且很多，PC 侧调度压力较大，但 parser 已验证能处理分片。
- 如果 `frame_ok fid` 与 STM32 USART1 的 `[STRM] fid` 对不上，中间有字节丢失或 parser 还没恢复。
- 如果 `checksum_bad` 里 `nextSync` 经常很快出现，说明坏帧后能重新同步，但当前帧内容已损坏。
- 如果 `head` 不是 `aa 55 01 ... 00 a0 00 78 00 00 96 00`，说明同步点附近已有错位或混入其他数据。

### 11.2 STM32 USART1 发送侧诊断

STM32 不往 UART4 插入任何文本，UART4 仍然只发二进制热成像帧。

新增 USART1 诊断输出：

```text
[STRM] fid=123 len=38415 sum=xxxxx st=0 ok=16 fail=0 S=1 P=0 uartErr=0
```

含义：

- `fid`：STM32 本次发出的帧号，和上位机 `frame_ok fid` 对照。
- `len`：固定应为 `38415`。
- `sum`：STM32 发送前计算出的 checksum。
- `st`：`HAL_UART_Transmit()` 返回值，`0` 为 `HAL_OK`。
- `ok/fail`：UART4 发送成功/失败计数。
- `S/P`：收到上位机开始/停止命令次数。
- `uartErr`：UART4 RX 侧 HAL 错误回调次数，主要用于判断控制字节方向是否有噪声/错误。

输出频率：

- 正常情况下每 16 帧输出一次。
- UART4 发送失败时立即输出。

### 11.3 本轮构建状态

Qt：

```text
frameparser=0 thermal=0
新版 exe: ESP_UART_Host/ESP_UART_Windows/build_qt_fix/esp_uart.exe
```

注意：当前 `deploy/esp_uart.exe` 仍在运行，Windows 锁定文件，所以本轮暂未覆盖 deploy。关闭正在运行的上位机后，再把 `build_qt_fix/esp_uart.exe` 同步到 deploy。

STM32：

```text
Keil rebuild: 0 Error(s), 0 Warning(s)
HEX: MDK-ARM/XIH6_V2/XIH6_V2.hex
```

## 12. VoSPI 帧缓存复查：不需要 SDRAM，但需要完整帧 staging

针对“是不是没有足够数组保存正确帧，导致视频流有问题”的怀疑，重新对照了当前 STM32 Lepton VoSPI 驱动和 `Arduino平台Lepton红外热成像VoSPI驱动详解.md`。

结论：

- Lepton 3.5 一帧 RAW16 是 `160 * 120 * 2 = 38400 B`。
- H743 工程不需要 SDRAM。修改前 `XIH6_V2.map` 显示 RW/ZI 总量约 `90.49 KiB`，使用的是 `0x24000000` AXI SRAM，容量 `512 KiB`。
- 原代码已经有 `lepton_raw_frame[120][160]` 约 `38400 B`、`seg_payload[60][160]` 约 `9600 B`、`stream_buf[38415]` 约 `38415 B`。
- 真正风险不是“完全没有帧数组”，而是 `lepton_raw_frame` 同时承担了 segment 拼接缓存和已发布帧缓存，且 `vospi_cached_mask` 成功发布后没有清零。这样后续可能只收到新的 segment 4，就把旧的 segment 1/2/3 加新的 segment 4 当成完整帧发给上位机。

Arduino 文档可借鉴的是：

- 必须有至少一整帧缓存。
- VoSPI 必须按完整帧边界发布，不能读到一点就认为是图像。
- 上层显示/传输应只消费“已确认完整”的帧。

但文档里“连续读 38400 字节”和“32 字节 packet/1200 packet”的描述不能直接照搬到 Lepton 3.5 标准 VoSPI。本工程当前采用的是更符合 Lepton 3.x 的结构：

```text
1 packet = 164 B = 2 B ID + 2 B CRC + 160 B payload
1 segment = 60 packets = 30 image rows
1 frame = 4 segments = 160 x 120 RAW16
segment id only valid in packet 20 ID[0] high nibble
```

本次 STM32 修改 `Drivers/PER/LEPTON/lepton.c`：

- 新增 `lepton_assembly_frame[120][160]`，约 `38400 B`，放在内部 AXI SRAM。
- `Lepton_VoSPI_CommitSegment()` 改为写入 staging frame，不再直接改 `lepton_raw_frame`。
- 允许像旧版本一样跨多次 `Lepton_Capture_Frame()` 攒 segment shelf；staging frame 中 segment 1/2/3/4 都至少有一份有效数据后，只在 segment 4 到来时 `memcpy()` 到 `lepton_raw_frame` 发布。
- `Lepton_Stream_SendFrame()` 仍然只读取 `lepton_raw_frame`，所以 UART4 发送的一定是最后一次确认完整的帧。
- `Lepton_VoSPI_Resync()` 会清空当前 segment 组帧状态，避免 resync 前的残留 segment 参与下一帧。

内存影响和构建验证：

```text
新增 staging frame: 38400 B
Keil rebuild: 0 Error(s), 0 Warning(s)
Program Size: Code=71710 RO-data=9406 RW-data=40 ZI-data=131024
Total RW Size: 131064 B = 127.99 KiB
AXI SRAM: 512 KiB
HEX: MDK-ARM/XIH6_V2/XIH6_V2.hex
```

所以当前不需要 SDRAM；比起引入 SDRAM/FMC 初始化风险，更合理的是先保证内部 SRAM 中的帧发布逻辑正确。

这项修改主要解决“边写边发导致的发布帧不稳定”问题，但保留当前硬件更容易成功的跨尝试攒 segment 策略。注意：为了恢复实测最好的行为，发布后不清 `mask`，后续 segment 会刷新 staging shelf，segment 4 到来时继续发布。它不会修复 UART4 物理链路字节损坏导致的 `Checksum mismatch`；如果新版固件后 `serial_diag.log` 仍然显示 checksum 参数乱跳，仍要继续按 UART4/CH340 2 Mbps 链路方向排查。

## 13. uart_log_new 回归分析：no frame 是缓存策略误杀

`uart_log_new.txt` 中的关键现象：

```text
valid=2633 discard=367 invalid=0 pkt0=60 desync=21 badseg=37 mask=0x00 seen=5/5/6/6 segs=1/1/2/2
valid=2659 discard=341 invalid=0 pkt0=61 desync=19 badseg=40 mask=0x00 seen=3/8/1/9 segs=1/0/1/3
valid=2673 discard=327 invalid=0 pkt0=61 desync=19 badseg=39 mask=0x00 seen=5/7/3/7 segs=2/0/1/2
```

判断：

- 不是 VoSPI 完全没有数据。`valid≈2600`、`pkt0≈60` 说明 SPI packet 流在跑。
- 不是单纯 UART4 上位机问题，因为这里还没到发送帧阶段。
- `badseg≈40` 且 `exp=20` 说明 packet 20 的 segment id 仍然经常读成 0，这是当前硬件/时序不稳定的老问题。
- 回归点是新版本把 `Lepton_VoSPI_Resync()` 改成清 assembly，并要求 segment 必须连续 `1->2->3->4`。但当前硬件实际需要像旧版本那样多轮攒 segment，所以 `mask` 一直被打回 `0x00`，直接 no frame。

修正：

- `Lepton_VoSPI_Resync()` 不再清 `vospi_cached_mask`。
- 去掉严格 `1->2->3->4` 连续顺序判断。
- 保留 `lepton_assembly_frame` staging，valid segment 写 staging。
- 恢复上上次效果的关键：发布后不清 `mask`，并且只在 `seg==4 && mask==0x0F` 时复制到 `lepton_raw_frame`。

预期新日志：

```text
no frame ... mask=0x09
no frame ... mask=0x0D
OK ... mask=0x0F
```

这恢复了旧版本“前几轮 no frame 不是失败，而是在攒 segment；攒满后后续 segment 4 到来就 OK”的行为，同时避免上位机发送读到正在被 segment commit 改写的 `lepton_raw_frame`。

## 14. 再次修正：恢复上上次最优 mask 语义

用户反馈新版“还不如上上次，原来 no frame 后次次 OK”。复查发现上一版仍有一个错误：

```c
if ((vospi_cached_mask & 0x0F) == 0x0F) {
    memcpy(lepton_raw_frame, lepton_assembly_frame, sizeof(lepton_raw_frame));
    vospi_cached_mask = 0;   // 问题点
    return 1;
}
```

这会导致每次成功发布后又从 0 开始攒四个 segment。当前硬件 VoSPI 时序本来就不稳定，重攒四段的代价很高，所以表现会比上上次差。

已恢复为更接近上上次最优版本的条件：

```c
if ((seg == LEPTON_SEG_CNT) && ((vospi_cached_mask & 0x0F) == 0x0F)) {
    memcpy(lepton_raw_frame, lepton_assembly_frame, sizeof(lepton_raw_frame));
    lepton_diag.vospi_got_mask = 0x0F;
    return 1;
}
```

关键点：

- `mask` 攒满后不清零。
- 后续 segment 1/2/3/4 会继续刷新 staging shelf。
- 只有 segment 4 到来时发布，恢复“攒满后 segment 4 触发 OK”的行为。
- 相比上上次，额外保留 `lepton_assembly_frame`，使 `lepton_raw_frame` 只保存已发布帧，不再被 segment commit 边写边改。

本次 Keil 构建：

```text
Keil rebuild: 0 Error(s), 0 Warning(s)
Program Size: Code=71710 RO-data=9406 RW-data=40 ZI-data=131024
Total RW Size: 131064 B = 127.99 KiB
HEX: MDK-ARM/XIH6_V2/XIH6_V2.hex
HEX time: 2026/7/5 22:55:07
```
