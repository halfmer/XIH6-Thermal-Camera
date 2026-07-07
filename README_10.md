# README_10 - 多外设协调与 Lepton VoSPI/串口传输调度优化

日期：2026-07-07

最新状态已被 README_12 覆盖：当前重新恢复到“时间轮前”的 README_9 Step 9D 行为，USART1 为 `1500000` + TX DMA ring，UART4 保持 `1500000` RAW16 DMA + Ping-Pong。本文第 10 节的 `115200` 回退版仅作为中间调试记录保留。

## 1. 本阶段目标

原计划方向如下，后续实测已回退：

- 保留 README_9 Step 9B 的 UART4 RAW16 DMA + Ping-Pong。
- 保留 USART1 `1500000` + TX DMA ring。
- 保留 RAW 内容诊断 `raw=min/max span=N flat=X/Y`。
- 减少 SD、SHT40、OLED、空闲 Lepton 检测等外设对 VoSPI 和串口发送的影响。

本阶段不直接上 FreeRTOS，先做协作式时间轮。

## 2. FreeRTOS vs 时间轮选择

### 2.1 FreeRTOS 的问题

FreeRTOS 可以把任务拆得更清楚，但这个工程当前不适合一步到位：

- 需要重新规划 HAL tick、SysTick、NVIC 优先级、DMA 回调和任务通知。
- 需要给 SD、OLED、SHT40、Lepton、UART stream 分任务和栈。
- 需要处理 HAL 阻塞函数在任务里的优先级反转和互斥。
- 引入后调试变量变多，容易把当前已经能跑的 Lepton 链路打散。

FreeRTOS 适合后续“协议稳定、帧内容稳定”以后再做架构升级。

### 2.2 当前选择：协作式时间轮

本阶段选时间轮，原因：

- 改动小，只动 `main.c` 主循环调度。
- 不改变 CubeMX 外设初始化。
- 不改变 UART4/Qt 协议。
- 不改变 Lepton VoSPI 采集函数。
- 可以先把慢外设从热成像流模式里隔离出去。

核心原则：

```text
stream_active = 1:
    只跑 Lepton_Capture_Frame + Lepton_Stream_SendFrame + 5s 诊断 + 非阻塞 LED 心跳

stream_active = 0:
    用时间轮分时跑 SD 热插拔、空闲 Lepton 检测、SHT40/OLED 更新、LED 心跳
```

## 3. 修改内容

文件：

```text
Core/Src/main.c
```

新增周期：

```c
#define APP_LED_IDLE_PERIOD_MS       500U
#define APP_LED_STREAM_PERIOD_MS     250U
#define APP_SD_POLL_PERIOD_MS        500U
#define APP_SHT40_PERIOD_MS         1000U
#define APP_IDLE_LEPTON_PERIOD_MS   8000U
```

新增任务：

```text
APP_Heartbeat()
APP_SDHotplugTask()
APP_IdleLeptonTask()
APP_SHT40Task()
APP_RunIdleTimeWheel()
```

## 4. 关键行为变化

### 4.1 去掉主循环阻塞心跳

旧代码：

```c
LED_TURN(250);   // 每轮阻塞约 500ms
```

新代码：

```c
APP_Heartbeat(now_ms, APP_LED_IDLE_PERIOD_MS);
```

LED 仍会闪，但不再用 `delay_ms()` 阻塞主循环。

### 4.2 流模式取消 2ms 阻塞

旧代码：

```c
LED_TURN(1);     // stream 模式每帧后阻塞约 2ms
```

新代码：

```c
APP_Heartbeat(now, APP_LED_STREAM_PERIOD_MS);
```

VoSPI 成功率比 2ms 心跳更重要，所以流模式心跳必须非阻塞。

### 4.3 慢外设只在空闲模式跑

空闲时间轮里每个慢任务开始前都会检查：

```c
if (Lepton_Stream_Active())
    return;
```

并且 `APP_RunIdleTimeWheel()` 在每个慢任务之间也再次检查，避免 UART4 收到 `S` 后继续跑后续慢任务。

慢任务包括：

- SD 热插拔和 `SD_Card_Init()`
- 空闲 Lepton 检测 `Lepton_Capture_Frame()`
- SHT40 读取和 OLED 更新

## 5. 当前仍保留的限制

时间轮不是抢占式 RTOS，所以如果 `S` 命令刚好在某个慢任务已经开始后到达，当前慢任务仍会跑完再进入 stream 模式。

这个风险目前可接受：

- SHT40 固定约 10ms。
- OLED 更新较短。
- SD 插入初始化和空闲 Lepton 检测较慢，但它们只在 stream inactive 时调度。

如果后续仍发现启动 stream 时偶发延迟，再考虑：

- 空闲 Lepton 检测周期加长或默认关闭。
- SD 初始化拆成状态机。
- 最后再考虑 FreeRTOS。

## 6. 构建验证

Keil 命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme10_timewheel.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=85710 RO-data=9838 RW-data=40 ZI-data=177888
Build Time Elapsed: 00:00:04
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 07:39:16
size: 268988 B
MD5 : AD67C9181D2D0940483145B428678FEC
```

map 确认：

```text
sd_uart_dma_ring = 0x24013f40, size=8192
stream_buf       = 0x240184c0, size=76864
```

两者仍在 `0x240xxxxx` AXI SRAM，DMA1 可访问。

## 7. 上板复测重点

烧录 `MD5=AD67C9181D2D0940483145B428678FEC` 后：

1. USART1 串口助手仍用 `1500000`。
2. UART4 热成像仍用 `1500000`，协议仍是 RAW16 `type=0x01`。
3. 先看启动是否恢复到 Step 9B 的速度，不能再出现 Step 9C 那种灯直接不亮。
4. 进入热成像 stream 后，看 `checksum_bad` 是否仍为 0。
5. 看 `send_ok/send_fail`、`cap_ok/cap_fail` 是否比之前更稳定。
6. 看 `flat=X/Y` 是否能解释全绿帧；如果 flat 增加但 Qt 仍全绿，下一步查 Qt 显示；如果 flat 不增加但仍全绿，下一步查 VoSPI segment 内容一致性。

## 8. 下一步建议

如果这个版本启动正常、checksum 仍稳定，下一阶段优先处理“非扁平但显示异常”的帧内容问题：

- 给 VoSPI segment shelf 加 generation/fresh 标记。
- 或者在 STM32/Qt 两侧同时记录 `min/max/span`，确认全绿帧到底是 STM32 内容异常还是 Qt 显示异常。

FreeRTOS 暂时不作为下一步，因为它会扩大变量面；等 VoSPI 内容正确性稳定后再评估是否值得引入。

## 9. 回退记录：时间轮版本不作为当前版本

状态：已按用户反馈回退到上一版。

用户上板反馈：

```text
还是不如上一版的，先退回上一版
```

因此撤销 `Core/Src/main.c` 中的时间轮改动：

- 删除 `APP_LED_* / APP_SD_* / APP_SHT40_* / APP_IDLE_LEPTON_*` 周期常量。
- 删除 `APP_Heartbeat()` / `APP_RunIdleTimeWheel()` 等时间轮函数。
- 恢复原主循环：
  - stream 模式仍 `Lepton_Capture_Frame()` + `Lepton_Stream_SendFrame()` + `LED_TURN(1)`。
  - idle 模式仍 `LED_TURN(250)`、SD 热插拔、每 16 轮空闲 Lepton 检测、SHT40/OLED 更新。

保留上一版已有内容：

- USART1 `1500000` + TX DMA ring。
- UART4 RAW16 DMA + ping-pong。
- RAW span/flat 诊断和扁平帧门禁。

回退后 Keil 构建：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=87174 RO-data=9838 RW-data=40 ZI-data=177856
Build Time Elapsed: 00:00:02
```

当前产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 07:56:35
size: 273128 B
MD5 : BDF8B0E4C1DA0B78115EDACE15505365
```

这个 MD5 与 README_9 Step 9D 一致。README_10 的 `AD67C918...` 时间轮版本不再建议烧录。

## 10. 回退记录：退回未改 USART1 波特率版本

状态：已按用户反馈继续回退。本节是当前建议烧录版本。

用户复测反馈：

```text
还不如没改USART1波特率那一版的，退回去
```

因此本次撤销 README_9 Step 9B 以后对 USART1 和内容门禁相关的改动，只保留已验证更稳定的 UART4 热成像 DMA 发送主方向：

- USART1 恢复 `115200`，仍作为调试日志串口使用。
- `SD_UART_Print()` 恢复为 `HAL_UART_Transmit(&huart1, ..., 100)` 阻塞发送，不再使用 USART1 DMA ring。
- 删除 USART1 TX DMA 的 CubeMX 残留初始化、DMA1_Stream0 IRQ 入口和 `hdma_usart1_tx` 句柄。
- `HAL_UART_TxCpltCallback()` / `HAL_UART_ErrorCallback()` 不再分发 USART1 DMA 回调，只处理 UART4 stream。
- 移除 `raw=min/max/span` 内容门禁和 flat 丢帧策略，避免把后续采集判断混进当前回退版本。
- UART4 热成像流保持 `1500000`，协议仍是 RAW16 `type=0x01`，发送路径仍为 DMA + Ping-Pong。

当前串口使用方式：

```text
USART1 调试日志：115200, 8N1
UART4  热成像流：1500000, 8N1, RAW16 binary frame
```

代码核对结果：

```text
Core/Src/usart.c:
  huart1.Init.BaudRate = 115200

Core/Src/main.c:
  SD_UART_Print() -> HAL_UART_Transmit(&huart1, ...)

Drivers/PER/LEPTON/lepton_stream.c:
  HAL_UART_Transmit_DMA(stream_huart, ...) 只用于 UART4 stream
  UART callbacks 只处理 stream_huart
```

复查关键字：`hdma_usart1_tx`、`DMA1_Stream0`、`DMA_REQUEST_USART1_TX`、`HAL_UART_Transmit_DMA(&huart1, ...)` 在 `Core/Src`、`Core/Inc`、`Drivers/PER/LEPTON` 中均无匹配。UART4 仍使用 `DMA1_Stream1`。

`XIH6_V2.ioc` 也已同步清理 USART1 TX DMA：`Dma.RequestsNb=0`，USART1 仅保留 PA9/PA10 普通异步串口引脚配置。

回退后 Keil 重编译：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=75350 RO-data=9698 RW-data=40 ZI-data=169536
Build Time Elapsed: 00:00:03
```

当前产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
MD5 : B09E58BF2C447D1CB6D4D162E7E3FA18
log : MDK-ARM/build_revert_pre_usart1_baud_clean.log
```

上板复测时不要再用 `1500000` 打开 USART1 调试串口，应改回 `115200`。UART4 热成像上位机仍保持 `1500000`。
