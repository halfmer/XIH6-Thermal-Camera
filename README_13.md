# README_13 - 传输模式切换与三端通信改动记录

## 0. 承接 README_12（阶段基线）

进入本篇时的工程状态（README_12 §7）：

```text
git 基线  : 3084e4f  采集路径=2FPS金标准 persistent shelf (HEX MD5 C87FBCC8)
采集结论  : fresh 门禁三次尝试全部回退, 运动拼接帧由 Qt 撕裂门禁拦截
串口链路  : UART4/CH340 @1.5M AA55 RAW16 + DMA ping-pong (Step9A 零打印铁律)
调试口    : USART1 @115200 阻塞日志
Qt        : esp_uart, 三模式(串口调试/热成像串口/WiFi热成像), WiFi=TCP server:8888
```

本篇在此基线上新增 **ESP32-S3 无线链路（STM32 SPI5 从机 → ESP32 → WiFi TCP）**
与 **UART4/TCP 双链路运行时切换**。AA55 帧协议逐字节不变（AGENTS.md 兼容性要求）。

已定案的硬件事实（源自 ESP32与STM32引脚连接.txt + 底板原理图 PDF）：

```text
PK0 =SPI5_SCK <-IO37   PJ11=SPI5_MISO ->IO36   PH5=SPI5_NSS(硬件CS) <-IO47
PG3 =DRDY 输出 ->IO39   PG2 =就绪线 输入 <-IO38   PJ7=ESP32 IO0(开漏) 烧录时序
禁用: PJ3(IO21)/PH6(IO48) 与 LEPTON_RST/PWDN 共网络; IO0 是 ESP32 strap 脚
备胎: UART5(PB13/PB12) <-> ESP32 UART0 (保留未用, ESP32 控制台口)
```

## 1. 本轮改动目标

本阶段优先处理 Qt、ESP32、STM32 之间的热成像视频流传输模式切换。

目标是在不破坏完整帧传输原则的前提下，实现两种传输路径：

1. STM32 通过 UART4 发送热成像视频流。
2. STM32 通过 ESP32 发送 TCP 协议热成像视频流。

ESP32 烧录完成并正常工作后，系统默认优先使用 TCP 视频流传输。

上位机 Qt 增加一个按钮：

```text
切换传输模式
```

点击按钮后，Qt 自动通过串口发送一次切换指令。该指令经 UART4 到 STM32，STM32 完成传输模式切换后，通过 USART1 返回：

```text
change over
```

随后当新模式下完整视频帧传输成功后，STM32 继续通过 USART1 返回：

```text
ok
```

其中：

- `change over` 表示传输模式已经切换完成。
- `ok` 表示当前模式下视频流传输成功。

## 2. 当前理解的通信关系

### 2.1 STM32 与 Qt

Qt 侧需要具备两个能力：

1. 接收热成像视频流。
2. 发送传输模式切换命令。

上位机发送切换命令路径暂定为：

```text
Qt -> 串口 -> STM32 UART4
```

STM32 状态反馈路径暂定为：

```text
STM32 USART1 -> Qt/调试串口
```

状态反馈内容：

```text
change over\r\n
ok\r\n
```

### 2.2 STM32 与 ESP32

ESP32 继续作为网络转发节点，原则上不做热成像图像处理。

ESP32 主要职责：

1. 接收 STM32 发送的完整热成像帧。
2. 通过 TCP 转发给 Qt 或 Android。
3. 在烧录完成并启动后，默认准备 TCP 传输链路。
4. 使用额外空余引脚与 STM32 建立 1-wire 轻量状态通信。

1-wire 不建议承载视频数据，只用于轻量状态或握手，例如：

```text
ESP_READY
TCP_READY
TCP_CLIENT_CONNECTED
TCP_STREAM_ACTIVE
```

### 2.3 STM32 内部传输路径

STM32 内部维护一个当前传输模式：

```c
typedef enum {
    THERMAL_TX_UART4 = 0,
    THERMAL_TX_ESP32_TCP = 1,
} ThermalTxMode;
```

系统启动后：

1. 如果 ESP32 已 ready，并且 TCP 通道可用，默认进入 `THERMAL_TX_ESP32_TCP`。
2. 如果 ESP32 不可用，则回退到 `THERMAL_TX_UART4`。
3. 收到 Qt 切换命令后，在完整帧边界切换模式。

## 3. 必须保持的项目原则

本次改动不能破坏原项目的核心要求：

1. STM32 只输出完整热成像帧。
2. Qt 只显示完整热成像帧。
3. ESP32 不修改帧内容，不做图像处理。
4. 不允许因为模式切换导致半帧输出。
5. 不允许为了提高帧率牺牲帧完整性。
6. 不允许新增驱动直接抢占已有外设资源。

模式切换时必须遵守：

```text
等待当前帧完成 -> 停止当前输出通道 -> 切换模式 -> 启动新输出通道 -> 成功发送完整帧 -> 返回 ok
```

## 4. STM32 侧建议改动

### 4.1 新增轻量传输管理器

建议新增：

```text
Core/Inc/thermal_tx_manager.h
Core/Src/thermal_tx_manager.c
```

职责：

1. 维护当前热成像传输模式。
2. 接收 UART4 切换命令。
3. 在完整帧边界完成模式切换。
4. 将完整帧分发给 UART4 或 ESP32 TCP 通道。
5. 通过 USART1 输出 `change over` 和 `ok`。

建议接口：

```c
void ThermalTxManager_Init(void);
void ThermalTxManager_Poll(void);
void ThermalTxManager_RequestToggle(void);
void ThermalTxManager_OnFrameReady(const uint8_t *frame, uint32_t length);
ThermalTxMode ThermalTxManager_GetMode(void);
```

### 4.2 外设驱动边界

不要把所有逻辑塞进 `main.c`。

建议拆分边界：

| 模块 | 职责 |
|---|---|
| `lepton_driver` | VoSPI 解析、分段同步、完整帧组装 |
| `thermal_tx_manager` | 传输模式管理、帧分发、切换状态机 |
| `uart4_cmd` | 接收并解析 Qt 切换命令 |
| `usart1_status` | 输出 `change over` / `ok` / 错误状态 |
| `esp32_link` | STM32 与 ESP32 的数据和状态连接 |
| `onewire_link` | ESP32 与 STM32 的轻量 ready/active 状态 |

### 4.3 不使用复杂 RTOS，但保留调度思想

本阶段不引入 FreeRTOS。

采用轻量轮询 + 中断/DMA 标志位方式：

```text
主循环只调度状态机
中断只收数据或置标志
DMA 完成回调只置完成标志
业务逻辑放到 manager 层处理
```

主循环建议结构：

```c
while (1)
{
    LeptonDriver_Poll();
    Uart4Cmd_Poll();
    Esp32Link_Poll();
    OneWireLink_Poll();
    ThermalTxManager_Poll();
}
```

这样可以避免新增驱动长期阻塞 CPU 或占用其他外设。

### 4.4 UART4 切换命令

建议 Qt 发送固定文本命令：

```text
MODE_TOGGLE\n
```

STM32 UART4 收到后：

1. 设置 `mode_change_requested` 标志。
2. 不立即打断当前帧。
3. 等待当前帧发送完成。
4. 切换传输模式。
5. USART1 发送 `change over\r\n`。
6. 新模式成功发送完整帧后 USART1 发送 `ok\r\n`。

### 4.5 USART1 状态返回

USART1 暂定只发送状态文本，不承载视频帧。

基础状态：

```text
change over
ok
error: esp32 not ready
error: tx busy
error: frame invalid
```

其中 `ok` 只在完整帧成功送入当前传输通道后发送。

## 5. ESP32 侧建议改动

ESP32 侧保持职责简单：

1. 默认准备 TCP server 或 TCP client，按当前工程既有设计决定。
2. 接收 STM32 传来的完整热成像帧。
3. 原样通过 TCP 发给 Qt。
4. 不做热成像帧解析以外的图像处理。
5. 通过 1-wire 或 GPIO 状态线告诉 STM32：

```text
ESP32 已启动
TCP 已就绪
TCP 客户端已连接
TCP 视频流正在发送
```

ESP32 烧录完成后，每次启动都应默认准备 TCP 传输模式。

## 6. Qt 上位机建议改动

### 6.1 新增按钮

在 Qt 主界面新增按钮：

```text
切换传输模式
```

按钮点击行为：

```text
串口发送 MODE_TOGGLE\n
```

### 6.2 状态解析

Qt 需要监听 STM32 USART1 返回文本。

收到：

```text
change over
```

表示模式切换完成。

收到：

```text
ok
```

表示当前视频流传输成功。

Qt 不能在点击按钮后立即假设切换成功，必须等 STM32 返回状态。

### 6.3 显示安全

Qt 继续保持双缓冲显示：

```text
接收完整帧 -> 校验完整帧 -> swap 到显示缓冲 -> UI 更新
```

不能因为模式切换直接刷新半帧。

## 7. 推荐状态机

STM32 传输管理器建议状态：

```text
IDLE
UART4_STREAMING
ESP32_TCP_STREAMING
CHANGE_REQUESTED
WAIT_FRAME_END
SWITCHING
WAIT_FIRST_FRAME_OK
ERROR
```

状态流：

```text
启动
 -> 检查 ESP32 ready
 -> TCP ready ? ESP32_TCP_STREAMING : UART4_STREAMING

收到 MODE_TOGGLE
 -> CHANGE_REQUESTED
 -> WAIT_FRAME_END
 -> SWITCHING
 -> 发送 change over
 -> WAIT_FIRST_FRAME_OK
 -> 首帧发送成功
 -> 发送 ok
 -> 当前 STREAMING 状态
```

## 8. 协议建议

### 8.1 控制命令

Qt 到 STM32：

```text
MODE_TOGGLE\n
MODE_UART4\n
MODE_TCP\n
QUERY_MODE\n
```

第一阶段可以只实现：

```text
MODE_TOGGLE\n
```

### 8.2 STM32 状态返回

STM32 到 Qt/调试串口：

```text
change over\r\n
ok\r\n
mode: uart4\r\n
mode: tcp\r\n
error: <reason>\r\n
```

第一阶段必须实现：

```text
change over\r\n
ok\r\n
```

## 9. 待确认问题（已全部落实答案）

文档起草时的疑问，现按工程实况定案：

1. Qt 工程 = **Qt Widgets**（ESP_UART_Host/ESP_UART_Windows，Qt 6.10.1 mingw）。
2. Qt **已有串口模块**（SerialWorker 线程）+ TCP server（WiFi热成像 8888）。
3. UART4 = 视频 TX + 命令 RX 全双工共用（§13.3 约束成立）。
4. USART1 = 调试/状态口 @115200（`change over`/`ok` 从这里出）。
5. STM32→ESP32 视频通道 = **SPI5 从机 TX-only + DMA + DRDY/CS 握手**（§0 引脚表）。
6. ESP32 TCP = **client**，主动连 Qt server `IP:8888`。
7. 就绪线 = **ESP32 IO38 → STM32 PG2**（高=TCP 已连接，语义按 §13.4 收紧）。
8. `ok` = **切换后首帧成功送入通道时发一次**（含上电首帧），不每帧刷屏。

## 10. 下一步修改顺序

建议按以下顺序修改：

1. 先改 Qt：增加按钮，点击后发送 `MODE_TOGGLE\n`。
2. 再改 Qt：解析 `change over` 和 `ok` 状态。
3. 再改 STM32：新增 `thermal_tx_manager`，只做状态机和模式变量。
4. 再改 STM32：新增或整理 UART4 命令解析。
5. 再改 STM32：整理 USART1 状态输出。
6. 再改 STM32：接入 ESP32 ready / TCP ready 状态。
7. 再改 ESP32：确认启动后默认 TCP ready，并通过 1-wire/GPIO 通知 STM32。
8. 最后做整链路验证：UART4 模式、TCP 模式、模式切换、断开恢复。

## 11. 验证标准

本阶段完成后，应能验证：

1. ESP32 烧录并启动后，STM32 默认选择 TCP 视频流。
2. Qt 点击 `切换传输模式` 后，STM32 能收到 UART4 命令。
3. STM32 不在半帧中途切换模式。
4. 切换完成后 USART1 输出 `change over`。
5. 新模式下完整帧发送成功后 USART1 输出 `ok`。
6. Qt 不显示半帧。
7. TCP 模式和 UART4 模式都能保持完整帧传输。
8. 新增驱动不会阻塞 LEPTON 采集、视频发送或串口接收。

## 12. 当前结论

本阶段不要急于优化帧率。

优先完成：

```text
Qt 按钮 -> UART4 命令 -> STM32 模式切换 -> USART1 状态返回 -> 完整帧继续传输
```

只要这个闭环稳定，后续再继续优化 ESP32 TCP 吞吐、STM32 DMA、缓冲队列和长期运行稳定性。

## 13. 逻辑闭合检查

### 13.1 已闭合的主流程

当前设计主闭环为：

```text
Qt 点击切换按钮
 -> Qt 通过串口发送 MODE_TOGGLE\n
 -> STM32 UART4 接收命令
 -> STM32 记录切换请求
 -> 等待当前完整帧发送结束
 -> STM32 切换热成像输出模式
 -> STM32 USART1 发送 change over
 -> STM32 在新模式下发送下一帧完整热成像数据
 -> 新模式首帧发送成功
 -> STM32 USART1 发送 ok
 -> Qt/调试端确认视频流恢复
```

该闭环满足：

1. 切换动作有触发源。
2. 切换命令有接收路径。
3. 切换过程不打断半帧。
4. 切换完成有 `change over` 确认。
5. 新链路成功传输完整帧后有 `ok` 确认。

### 13.2 需要明确的通道角色

为避免后续实现混乱，通道角色固定如下：

| 通道 | 方向 | 职责 |
|---|---|---|
| UART4 RX | Qt/ESP32 -> STM32 | 接收 `MODE_TOGGLE\n` 等控制命令 |
| UART4 TX | STM32 -> Qt/串口接收端 | UART 模式下发送热成像数据 |
| USART1 TX | STM32 -> Qt/调试串口 | 发送 `change over`、`ok`、错误状态 |
| STM32 -> ESP32 数据通道 | STM32 -> ESP32 | TCP 模式下发送完整热成像帧给 ESP32 |
| 1-wire/GPIO | ESP32 -> STM32 | ESP32 ready / TCP ready / client connected 状态 |

USART1 不直接控制 UART4。USART1 只作为状态输出通道。

真正触发切换的是 UART4 收到的控制命令。STM32 内部的 `thermal_tx_manager` 根据该命令控制当前输出路径。

### 13.3 UART4 同时用于命令和视频时的约束

如果 UART4 TX 用于 STM32 向上位机发送热成像视频，UART4 RX 仍然可以接收 Qt 发来的切换命令，因为 UART 是全双工。

但必须遵守：

1. UART4 RX 只解析控制命令。
2. UART4 TX 只发送视频帧，不混入 `ok`、`change over` 等文本状态。
3. USART1 负责状态文本输出。
4. UART4 视频帧必须有帧头、长度和 CRC，避免控制文本与视频数据边界混淆。

### 13.4 TCP 默认模式闭合条件

ESP32 烧录完成后默认准备 TCP，但 STM32 不能只是假设 TCP 可用。

STM32 启动时应按以下条件选择默认模式：

```text
ESP32_READY == true
AND TCP_READY == true
AND TCP_CLIENT_CONNECTED == true
```

满足以上条件时默认进入：

```text
THERMAL_TX_ESP32_TCP
```

否则进入：

```text
THERMAL_TX_UART4
```

这样可以避免 ESP32 未连接上位机时 STM32 把视频帧发送到无效 TCP 链路。

### 13.5 `ok` 的语义

`ok` 不建议每帧都发送。

建议语义固定为：

```text
当前模式切换后，第一帧完整热成像数据成功送入目标传输通道。
```

这样 `ok` 表示一次切换闭环完成，而不是持续刷屏状态。

如果后续需要长期心跳，应新增独立状态，例如：

```text
stream alive
```

不要复用 `ok`。

### 13.6 尚未闭合但必须在代码阶段解决的问题

以下点文档已提出，但代码实现前仍需确认：

1. STM32 到 ESP32 的视频数据通道到底是 SPI、UART 还是已有自定义总线。
2. 1-wire/GPIO 的具体引脚、有效电平和超时策略。
3. Qt 的切换命令是走独立串口，还是和视频 UART4 接收端共用同一个串口设备。
4. TCP 模式下 ESP32 是 TCP server 还是 TCP client。
5. STM32 如何判断 ESP32 TCP 首帧发送成功，是依赖 ESP32 ACK、GPIO 状态，还是 STM32 侧发送完成即可。

其中第 5 点最关键。

如果 STM32 无法从 ESP32 获得 TCP 发送成功反馈，那么 STM32 的 `ok` 只能表示：

```text
STM32 已成功把完整帧交给 ESP32。
```

不能表示：

```text
Qt 已经成功收到该完整帧。
```

若需要 `ok` 严格代表 Qt 已收到完整帧，则需要增加 Qt -> ESP32 -> STM32 的 ACK 回传路径。

## 14. 已落地的代码改动

本轮已开始进入代码实现，不再只是记录计划。

### 14.1 Qt 上位机

已修改：

```text
ESP_UART_Host/ESP_UART_Windows/mainwindow.h
ESP_UART_Host/ESP_UART_Windows/mainwindow.cpp
ESP_UART_Host/ESP_UART/mainwindow.h
ESP_UART_Host/ESP_UART/mainwindow.cpp
```

已完成：

1. 在热成像页面工具栏增加 `切换传输模式` 按钮。
2. 按钮点击后通过当前已打开的 STM32 控制串口发送：

```text
MODE_TOGGLE\n
```

3. WiFi 热成像模式下保留串口设置区域，用于打开 UART4 控制串口。
4. WiFi 模式下串口收到的数据不再送进热成像帧解析器，避免 USART/UART 状态文本污染 TCP 视频帧解析。
5. Qt 会识别串口文本状态：

```text
change over
ok
error: ...
```

并更新状态栏。

6. 修正 WiFi 模式下打开/关闭控制串口时自动发送 `P` 的问题。现在只有 `热成像(串口)` 模式会自动发送 `S/P` 控制 UART4 视频流，WiFi 控制串口不会自动停掉 STM32 视频流。
7. 旧版 `ESP_UART` 上位机也已同步增加 `切换传输模式` 按钮，避免运行旧工程时看不到 UI 变化。
8. 旧版 `ESP_UART` 已补充 `1500000` 波特率并设为默认值，匹配 STM32 当前 UART4 视频流配置。

### 14.2 STM32

已修改：

```text
Drivers/PER/LEPTON/lepton_stream.h
Drivers/PER/LEPTON/lepton_stream.c
```

已完成：

1. UART4 命令接收从单字节扩展为同时支持单字节和文本命令。
2. 新增支持：

```text
MODE_TOGGLE\n
MODE_UART4\n
MODE_TCP\n
```

并保留原有单字节命令：

```text
S / P / T / U / W / B / R
```

3. 传输模式切换改为在主循环 `Lepton_Stream_Poll()` 中执行。
4. 切换时不再 abort 正在发送的 UART4/SPI5 DMA。若当前帧正在发送，切换请求会等待 `stream_tx_busy == 0` 后再执行，避免半帧切换。
5. 切换完成后 USART1 输出严格文本：

```text
change over
```

6. 当前模式下第一帧完整送入传输通道并完成 DMA 后，USART1 输出：

```text
ok
```

7. ESP32 ready 状态线 PG2 高电平时，STM32 自动优先切到 SPI5 -> ESP32 -> TCP 路径。

### 14.3 ESP32

已修改：

```text
esp32/sketch_jul08a/sketch_jul08a.ino
```

已完成：

1. 新增 ready 状态线：

```text
ESP32 IO38 -> STM32 PG2
```

2. ESP32 启动时 ready 拉低。
3. WiFi 未连接、TCP 未连接或 TCP 发送失败时 ready 拉低。
4. TCP 连接 Qt 成功后 ready 拉高，STM32 看到该状态后可默认进入 TCP 视频传输路径。

### 14.4 当前验证状态

已做：

1. 静态检查确认 Qt 按钮、STM32 命令解析、ESP32 ready 线引用路径存在。
2. 静态检查确认 STM32 传输切换不会在 `Lepton_Stream_ApplyLinkSwitch()` 中 abort 当前 DMA。
3. 静态检查确认 Qt WiFi 模式不会把串口状态文本送入热成像帧解析器。

未完成：

1. Keil `UV4.exe` 可找到，但命令行构建没有生成日志文件，暂未能确认 STM32 实际编译结果。
2. ESP32 Arduino 编译未执行，当前环境未发现 `arduino-cli`。

已完成 Qt 构建：

```text
ESP_UART_Host/ESP_UART_Windows/build-codex/esp_uart.exe
ESP_UART_Host/ESP_UART/build-codex/esp_uart.exe
```

构建环境：

```text
Qt:    E:\Qt\Qt_Creato_All\6.10.1\mingw_64
CMake: E:\Qt\Cmake\bin\cmake.exe
Ninja: E:\Qt\Qt_Creato_All\Tools\Ninja\ninja.exe
MinGW: E:\Qt\Qt_Creato_All\Tools\mingw1310_64
```

两个构建目录已执行 `windeployqt`，可直接运行对应 `esp_uart.exe` 检查 UI。

因此本轮 Qt 代码已实际编译通过；STM32 和 ESP32 仍需要在对应工具链可用后做实际编译验证。

## 15. 代码评审结论（2026-07-09）

对 §14 落地代码的整体评审（lepton_stream.c/h、main.c、it.c/h、sketch_jul08a.ino、
Qt mainwindow）：

### 15.1 判定为正确/优良的设计

1. **帧边界切换**：`ApplyLinkSwitch()` 在 `stream_tx_busy!=0` 时直接返回、pending
   保持，由下一轮 Poll 重试——不 abort 在飞 DMA，半帧切换被结构性排除（§3 原则）。
2. **零打印铁律遵守**：`ok`/`change over` 都只在 `stream_tx_busy==0` 时经 USART1
   阻塞发送；DMA 回调内零打印（README_9 Step9A 教训内建）。
3. **驱动边界干净**：UART4 与 SPI5 各自独立的 GPIO/DMA/回调；命令解析
   （单字节 + MODE_* 文本双兼容）单独成函数；主循环只多一个 `Lepton_Stream_Poll()`
   ——符合"协作式轮询、中断只置标志"的 §4.3 形态，未侵占任何既有外设。
4. **就绪线语义正确收紧**：ESP32 端 READY=「TCP 已连上 Qt」而非「已开机」，
   WiFi 掉线/发送失败即拉低——正是 §13.4 要求的闭合条件。
5. **链路自愈闭环**：SPI5 从机 500ms 超时 abort（SendFrame 内）+ ESP32 读帧
   checksum 拦截 + CS/硬件 NSS 帧间免疫 + DRDY 电平（非边沿依赖）读取，
   单点故障最多损失一帧。
6. **ESP32 生产者-消费者**：双任务（SPI core1 / TCP core0）+ 3 缓冲 free/full
   队列 + 积压时弃旧保新，WiFi 抖动不再反压 SPI 侧。

### 15.2 本轮评审修正

- 删除 `lepton_stream.c` 中残留的 `#if (LEPTON_STREAM_LINK == ...)` 死开关
  （包裹 SPI 回调）及 `lepton_stream.h` 的 `LEPTON_STREAM_LINK` 编译期宏：
  链路已是纯运行时选择，宏若被改成 UART4 会静默删掉 SPI 回调 → SPI5 模式
  永久 busy。现在两条路径无条件编译，宏仅剩 ID 常量。

### 15.3 已知边角与遗留风险（按优先级）

1. **未构建未提交**：全部改动仍在工作区（含一次 IDE 手工构建 8D9A294C，无日志
   无 commit）——违反版本红线，需命令行重建 → 记 MD5 → commit 后才算落地。
2. **暂停态切换延迟**：SPI5 超时 abort 逻辑在 `SendFrame()` 内；若 'P' 暂停流
   且 SPI DMA 恰好 armed，busy 不会被释放，pending 切换会等到 'S' 恢复后才执行。
   改进方向：把超时释放挪进 `Poll()`（后续单变量改动）。
3. **单字节命令与文本命令共存约束**：以 `S/P/T/U/W/B/R` 开头的文本命令会被
   单字节路径截胡——新增文本命令必须避开这些首字母（现有 MODE_* 以 M 开头，安全）。
4. **ESP32 IO35~37 与 Octal PSRAM 模组冲突风险**（§0 引脚表）未上板排除；
   若 sketch 起不来则启用 UART5 备胎。
5. **`ok` 的语义边界**（§13.6 第 5 点）：SPI5 模式下 `ok` 表示"整帧已交给
   ESP32"，不表示 Qt 已收到——严格端到端确认需 ACK 回传，本阶段不做。

### 15.4 下一步（恢复版本红线）

```text
1. 命令行重建 (build_esp32_dual_link.log) -> 0E/0W -> 记 HEX MD5
2. git commit: lepton_stream.c/h + main.c + it.c/h + esp32/ + Qt mainwindow
   + README_12 §7 + README_13（本篇）
3. 上板: STM32 -> 30s 窗口烧 ESP32 -> 复位 -> USART1 看 change over/ok
   -> Qt WiFi热成像出图 -> 点按钮切换回 UART4 验证双向
```
