# README_9 - DMA 后视频流错误帧与全绿帧分析

日期：2026-07-07

## 1. 当前现象

基于 README_8 Step 3 的 UART4 TX DMA + Ping-Pong 版本：

- Qt 显示帧率约 `2 FPS`。
- 大约 `100` 帧里有 `7~8` 帧协议错误。
- 约一半显示帧是全绿/无效画面，另一半画面正常。
- 当前还没有进入 8-bit 灰度协议，仍是 `type=0x01` RAW16，`38400B payload`，`38415B/frame`。

本阶段结论：这不是单纯“Qt 显示错了”，而是至少有两个问题叠加：

1. **协议错误帧**：高置信度指向 STM32 发送/组包侧，不像 Qt 解析或 CH340 随机丢字节。
2. **全绿无效帧**：更像 VoSPI 组帧/发布内容不稳定，Qt 目前缺少内容质量门限，只要 checksum 通过就显示。

## 2. 串口日志证据

读取日志：

```text
ESP_UART_Host/ESP_UART_Windows/deploy/serial_diag.log
time: 2026-07-07 06:24:16
```

统计结果：

```text
frame_ok = 247
checksum_bad = 16
bad_fids = 15,31,47,63,79,95,111,127,143,159,175,191,207,223,239,255
bad_mod16 = 15
```

所有坏帧都有相同特征：

```text
gotBE=0
gotLE=0
nextSync=38415
syncDelta=0
```

关键判读：

- `syncDelta=0`：下一帧 `AA 55` 正好在理论帧长 `38415` 的位置。
- 这不像 CH340/FIFO 丢字节；如果丢字节，`nextSync` 通常会小于 `38415`。
- 这也不像 Qt 分包问题；Qt parser 能跨任意 chunk 拼帧，且下一帧能立即恢复。
- `gotBE=0 gotLE=0`：Qt 在帧尾收到的 checksum 字节就是 `00 00`。
- 因此协议错误更像 STM32 发出了完整长度的帧，但 checksum 两字节异常为 0。

典型坏帧：

```text
checksum_bad fid=255 gotBE=0 gotLE=0 expected=17001 expectedNoSync=16746
buf=40847 total=38415 consume=38415 nextSync=38415 syncDelta=0
```

## 3. 与 STM32 代码的对应关系

当前 STM32 DMA 发送路径：

```c
status = HAL_UART_Transmit_DMA(stream_huart, buf,
                               (uint16_t)LEPTON_STREAM_FRAME_LEN);
...
Lepton_Stream_DebugPrint(fid, sum, status);
```

`Lepton_Stream_DebugPrint()` 的打印门限：

```c
uint32_t total = stream_tx_ok_count + stream_tx_fail_count;

if ((total > 1U) && ((total & 0x0FU) != 0U))
    return;
```

也就是：每 `16` 次发送会打印一次 `[STRM]`。

当前坏帧恰好全部是：

```text
fid % 16 == 15
```

这和 `[STRM]` 每 16 帧打印一次高度相关。虽然 `[STRM]` 走的是 USART1，不应该直接混入 UART4 二进制流，但它在 `HAL_UART_Transmit_DMA()` 启动后立即执行，会引入额外的阻塞、格式化、栈使用和中断时序扰动。当前证据足够说明：**下一步必须先去掉 stream 热路径里的每 16 帧 USART1 debug 打印，再复测 checksum_bad 是否消失。**

这一步不应同时改 8-bit 灰度、压缩或 Qt 协议。

## 4. Qt 侧检查结果

检查文件：

```text
ESP_UART_Host/ESP_UART_Windows/frameparser.cpp
ESP_UART_Host/ESP_UART_Windows/serialworker.cpp
ESP_UART_Host/ESP_UART_Windows/mainwindow.cpp
ESP_UART_Host/ESP_UART_Windows/thermalwidget.cpp
ESP_UART_Host/ESP_UART_Windows/colormap.cpp
```

已运行现有自测：

```text
frameparser_selftest.exe      PASS
thermalwidget_selftest.exe    PASS
```

Qt parser 行为：

- 按 `AA 55 + header + payload + checksum` 解析。
- checksum 错误后会寻找下一帧头。
- 当前日志里下一帧能稳定恢复，说明 parser 重同步逻辑基本有效。

Qt 显示行为：

- `FrameParser` 只验证协议，不验证画面内容。
- `ThermalWidget` / `ColorMap` 会直接显示任何 checksum 通过的 RAW16 帧。
- 如果一帧 raw 值范围很小，`ColorMap::apply()` 的 `range = vmax - vmin` 会接近 0，图像会变成单色。

所以全绿帧不能直接归咎于 Qt 字节序或 parser。更准确地说：

- Qt 目前**没有拦截坏内容帧**。
- 但坏内容帧大概率是 STM32/VoSPI 侧送来的。

## 5. 全绿帧的优先怀疑点

README_8 Step 3 去掉了 UART4 阻塞发送，CPU 会更快回到 `Lepton_Capture_Frame()`。

当前 VoSPI 发布逻辑仍是 persistent shelf：

```c
if ((seg == LEPTON_SEG_CNT) &&
    ((vospi_cached_mask & 0x0FU) == 0x0FU))
{
    memcpy(lepton_raw_frame, lepton_assembly_frame, sizeof(lepton_raw_frame));
    return 1;
}
```

特点：

- segment 1~4 可以跨多次 `Lepton_Capture_Frame()` 保留。
- 发布后不清 `vospi_cached_mask`。
- DMA 版采集更频繁后，更容易出现“协议帧完整，但内容由不同时间的 segment 拼成”的情况。

这解释了为什么：

- checksum 可以通过：UART4 发出去的帧格式完整。
- 画面却全绿/无效：`lepton_raw_frame` 的内容本身就不适合作为一帧显示。

但这里还不能直接恢复之前失败的 `fresh_mask` 强门槛；之前它导致长期 `no frame`。下一步应该先加低成本内容统计，再决定怎么改发布策略。

## 6. 结论分层

### 6.1 checksum_bad

定责倾向：STM32 侧。

理由：

- 坏帧固定为 `fid % 16 == 15`，不是随机串口抖动。
- 坏帧 `syncDelta=0`，不是典型丢字节/插字节。
- 坏帧实收 checksum 为 `0x0000`。
- 与 STM32 每 16 帧 `[STRM]` debug 打印严格相关。

### 6.2 全绿无效帧

定责倾向：STM32/VoSPI 内容侧为主，Qt 缺少拦截为辅。

理由：

- Qt 自测覆盖了 RAW16 大端转换和色表显示。
- Qt 只要 checksum 通过就显示，没有内容质量判断。
- DMA 后采集频率变化，放大了 persistent segment shelf 的跨帧拼接风险。

## 7. 下一步最小修改顺序

### Step 9A：先去掉每 16 帧 `[STRM]` 热路径打印

只改 STM32：

- 不再在 `Lepton_Stream_SendFrame()` 的成功路径里调用 `Lepton_Stream_DebugPrint()`。
- 保留 `HAL_BUSY/HAL_ERROR` 时的错误计数。
- 如需 DMA 统计，后续并入 5s 一次的 `[STRM_DIAG]`，不要在每 16 帧热路径里 `snprintf + HAL_UART_Transmit`。

验证目标：

```text
checksum_bad 是否从约 6% 降为 0
坏帧是否仍固定 fid%16==15
Qt 是否仍有 gap=1
```

如果去掉 `[STRM]` 后 checksum_bad 消失，说明 DMA 发送本身可用，问题是诊断打印扰动。

### Step 9B：加入图像内容质量统计

先不改协议，只做诊断：

- STM32 在 RAW16 打包循环里顺手统计 `min/max/span/avg`。
- 只在 5s `[STRM_DIAG]` 打印最后一帧或累计的 `span_min/span_avg/span_max`。
- Qt 在 `onFrameReceived()` 里也统计 `min/max/span`，写入 `serial_diag.log`。

判断：

- 全绿帧如果 `span` 很小，说明收到的是扁平/无效内容。
- 如果 STM32 统计正常、Qt 统计异常，才转向 Qt 接收/转换。
- 如果两边统计都异常，回到 VoSPI 组帧。

### Step 9C：再处理 VoSPI 发布策略

可选方向：

- 给 segment shelf 加 generation 标记，要求 1/2/3/4 来自同一轮更新再发布。
- 发布后清 mask，但放宽等待窗口，避免回到长期 `no frame`。
- 对明显无效帧不发送，只计入 `frame_drop_quality`。

这里不能直接复用之前失败的 `fresh_mask` 强门槛。

### Step 9D：最后再改 8-bit 灰度传输

只有在满足以下条件后再进入：

- `checksum_bad == 0` 或接近 0。
- Qt 不再显示全绿无效帧。
- STM32/Qt 两端 min/max/span 统计能互相对上。

然后再新增 `type=0x02` 8-bit 灰度帧，把 payload 从 `38400B` 降到 `19200B`。

## 8. 防复发约束

以后改高速视频流时必须分三类门禁：

1. 协议门禁：`checksum_bad`、`syncDelta`、`gap`。
2. 内容门禁：`min/max/span/avg`，不能只看 checksum。
3. 时序门禁：热路径不能插入 `sprintf`、阻塞 UART 打印、长循环诊断。

特别约束：

- 不要在 `HAL_UART_Transmit_DMA()` 后立即做周期性阻塞日志。
- 不要把“协议帧通过”当作“图像正确”。
- 不要在 checksum 和内容质量都未稳定前改 8-bit/压缩协议。

## 9. 本轮状态

本 README 只做分析记录，尚未生成新的固件 HEX。

当前仍停留在 README_8 Step 3 测试固件：

```text
MD5 : 2F515B2EEED65390F0919213FF462E79
```

下一步建议先做 Step 9A：移除每 16 帧 `[STRM]` 打印并重新构建验证。

## 10. Step 9A 修复：移除热路径阻塞日志

状态：已修改 STM32 并重新构建，等待上板复测。

### 10.1 修改内容

文件：`Drivers/PER/LEPTON/lepton_stream.c`

保持不变：

- UART4 TX DMA + Ping-Pong 核心方案不变。
- RAW16 协议不变，仍为 `type=0x01`、`38400B payload`、`38415B/frame`。
- Qt 上位机不变。
- VoSPI 采集和发布逻辑不变。

本次只做两个小修复：

1. 成功启动 `HAL_UART_Transmit_DMA()` 后，不再调用 `Lepton_Stream_DebugPrint()`。
2. `stream_tx_busy != 0` 丢帧时也不打印 USART1 日志，因为此时 UART4 DMA 正在发送上一帧。

修改后只有 `HAL_UART_Transmit_DMA()` 启动失败时才会走 `[STRM]` 错误打印：

```c
if (stream_tx_busy != 0U)
{
    stream_tx_fail_count++;
    stream_dma_busy_drop_count++;
    /* UART4 DMA is active here; do not block on USART1 diagnostics. */
    return HAL_BUSY;
}

status = HAL_UART_Transmit_DMA(stream_huart, buf,
                               (uint16_t)LEPTON_STREAM_FRAME_LEN);
if (status == HAL_OK)
{
    stream_frame_id++;
    stream_build_index ^= 1U;
    stream_tx_ok_count++;
}
else
{
    stream_tx_busy = 0U;
    stream_tx_index = 0xFFU;
    stream_tx_fail_count++;
    if (status == HAL_ERROR)
        stream_dma_error_count++;
    Lepton_Stream_DebugPrint(fid, sum, status);
}
```

同时把 ping-pong buffer stride 补齐到 32 字节边界：

```c
#define LEPTON_STREAM_BUF_STRIDE (((LEPTON_STREAM_FRAME_LEN + 31U) / 32U) * 32U)
static uint8_t stream_buf[2][LEPTON_STREAM_BUF_STRIDE] __attribute__((aligned(32)));
```

这个不改变实际发送长度，`HAL_UART_Transmit_DMA()` 仍只发送 `LEPTON_STREAM_FRAME_LEN = 38415` 字节。补齐 stride 的目的只是让两个 DMA buffer 的起始地址都按 cache line 对齐，避免以后开启 DCache 时产生尾部 cache line 共享风险。

### 10.2 构建验证

Keil 命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme9_step9a_no_hot_strm.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=75294 RO-data=9706 RW-data=40 ZI-data=169664
Build Time Elapsed: 00:00:03
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 06:34:08
size: 239378 B
MD5 : 94EDB7351E758B650ABB375D0B545C7E
```

map 关键点：

```text
stream_buf 0x240164a0 size=76864
```

`76864 = 2 * 38432`，说明两个 ping-pong buffer 都使用 32 字节对齐 stride。

### 10.3 上板复测重点

烧录 `MD5=94EDB7351E758B650ABB375D0B545C7E` 后，先只看协议错误是否消失：

```text
checksum_bad 是否还存在
如果存在，是否仍然 fid % 16 == 15
gotBE/gotLE 是否还固定为 0
syncDelta 是否仍为 0
frame_ok 后是否仍出现 gap=1
```

预期：

- 如果 `checksum_bad` 消失或明显下降，说明 README_8 Step 3 的 DMA 方案本身可保留，之前的周期性坏帧由热路径 USART1 阻塞日志触发。
- 如果 `checksum_bad` 仍固定为 `fid % 16 == 15`，则继续查 16 帧周期相关的其它变量。
- 如果 `checksum_bad` 消失但全绿帧仍存在，进入 Step 9B：加 STM32/Qt 两端 `min/max/span` 内容统计。

## 11. Step 9B 修复：USART1 DMA 化 + RAW 扁平帧门禁

状态：已修改 STM32 并重新构建，等待上板复测。

用户复测 Step 9A 后反馈：

```text
错误帧倒是没有了，就是全绿帧还在
```

因此本步保持 UART4 DMA + Ping-Pong 核心方案不变，不改 Qt 协议，先处理两个点：

1. USART1 调试口也改为 `1500000` 波特率 + TX DMA，避免高频日志再阻塞 VoSPI/UART4。
2. STM32 在 RAW16 打包时统计 `min/max/span`，对 `span < 16` 的明显扁平帧直接不发送，避免 Qt 收到 checksum 正确但内容无效的全绿帧。

### 11.1 USART1 当前配置

文件：

```text
Core/Src/usart.c
Core/Src/dma.c
Core/Src/main.c
Drivers/PER/LEPTON/lepton_stream.c
```

配置结果：

```text
USART1_TX = PA9
USART1_RX = PA10
BaudRate  = 1500000
DMA TX    = DMA1_Stream0 / DMA_REQUEST_USART1_TX / Normal mode
IRQ prio  = USART1_IRQn 6, DMA1_Stream0_IRQn 6
GPIO speed= GPIO_SPEED_FREQ_VERY_HIGH
```

`SD_UART_Print()` 现在不再直接 `HAL_UART_Transmit()`，而是：

```text
stack/local string -> 8192B static ring buffer -> HAL_UART_Transmit_DMA(&huart1, ...)
```

同时 `HAL_UART_TxCpltCallback()` / `HAL_UART_ErrorCallback()` 已分发 USART1 回调，用于推进环形缓冲区和统计失败次数。

### 11.2 全绿帧处理逻辑

`Lepton_Stream_SendFrame()` 在发送 UART4 前顺手扫描 `lepton_raw_frame`：

```text
raw_min = min(pixel)
raw_max = max(pixel)
raw_span = raw_max - raw_min
```

新增门限：

```c
#define LEPTON_STREAM_FLAT_SPAN_DROP 16U
```

如果 `raw_span < 16`：

- 认为这是明显扁平/无效内容帧。
- 不启动 UART4 DMA。
- 不增加 `frame_id`，Qt 不会看到 gap。
- 只累计 `flat` 诊断计数。

5 秒一次的 USART1 诊断新增字段：

```text
[STRM_DIAG] ... raw=min/max span=N flat=X/Y uart1Drop=A uart1Fail=B ...
```

判读：

- `flat` 增加、Qt 全绿消失：说明全绿来自 STM32/VoSPI 送出的扁平内容，当前门禁有效。
- `flat` 增加、Qt 画面帧率下降：说明之前有一部分 FPS 是无效帧，不应计入有效画面。
- `flat` 不增加但 Qt 仍全绿：说明全绿不是简单扁平帧，需要继续看 Qt 显示缩放或非扁平异常内容。
- `uart1Drop/uart1Fail` 非 0：说明 USART1 日志环或 DMA 仍有压力，需要减少日志或扩大 ring。

### 11.3 构建验证

Keil 命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme9_step9b_usart1_dma_flatgate.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=87174 RO-data=9838 RW-data=40 ZI-data=177856
Build Time Elapsed: 00:00:03
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 06:51:47
size: 273128 B
MD5 : BDF8B0E4C1DA0B78115EDACE15505365
```

### 11.4 上板复测要求

烧录 `MD5=BDF8B0E4C1DA0B78115EDACE15505365` 后：

1. USART1 串口助手波特率改为 `1500000`。
2. UART4 热成像上位机仍保持 `1500000`，协议仍是 RAW16 `type=0x01`。
3. 重点观察 `checksum_bad` 是否继续为 0。
4. 观察全绿帧出现时 `[STRM_DIAG] raw/span/flat` 的关系。
5. 如果全绿帧消失但 FPS 下降，以 `send_ok` 和 `flat` 分开看：有效 FPS 才是 `send_ok` 对应的画面。

## 12. Step 9C（已废弃）：启动期无灯/无串口的定位与打点

状态：已废弃。该方案会扰动启动链路，不建议烧录；以 Step 9D 的 `BDF8B0E4C1DA0B78115EDACE15505365` 为当前版本。

### 12.1 现象分析

用户反馈：

```text
灯不翻转，串口没输出，像是卡程序；过了好一会才有串口数据出来
```

源码对应关系：

- LED 翻转原本只在 `while(1)` 主循环里执行。
- 主循环之前会先跑完整个外设初始化、OLED 初始化、SD 检测、Lepton 初始化。
- `Lepton_Init()` 固定包含约 `1200ms` 的 Lepton 上电/复位等待。
- 如果 CCI 首次不 ACK，还会进入 I2C 全地址扫描和 bit-bang 诊断，原扫描超时较长，启动期可能被拖几秒。

所以“上电不翻灯”本身不等于程序死机；原代码在进入主循环前没有启动期心跳。

### 12.2 本次修改

新增启动阶段打点：

```text
[BOOT] <ms> usart1-dma-ready
[BOOT] <ms> cubemx-init-done
[BOOT] <ms> oled-init-begin
[BOOT] <ms> oled-init-done
[BOOT] <ms> lepton-init-begin
[BOOT] <ms> lepton-init-done dt=<ms>
[BOOT] <ms> enter-main-loop
```

每个 `BOOT_Mark()` 会：

- 通过 USART1 DMA 输出一行。
- 最多等待 `20ms` flush，确保短日志尽早出现在串口助手。
- 翻转一次 LED，作为启动阶段心跳。

新增 `SD_UART_Flush(timeout_ms)` 只用于启动打点，不改变 UART4 热成像协议，也不在视频热路径里阻塞。

### 12.3 Lepton CCI 失败路径提速

正常 Lepton 成功路径不变。

仅在 CCI 首次不 ACK 时，把诊断扫描超时缩短：

```c
HAL_I2C_IsDeviceReady(..., 2, 50)   // 0x2A 首次探测
HAL_I2C_IsDeviceReady(..., 1, 5)    // 0x08..0x77 全地址扫描
```

目的：如果上电瞬间 CCI 没准备好，不再让启动阶段被慢速扫描拖住太久。

### 12.4 构建验证

Keil 命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme9_step9c_boot_trace.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=88334 RO-data=9926 RW-data=40 ZI-data=177856
Build Time Elapsed: 00:00:03
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 07:14:02
size: 276638 B
MD5 : 8426D5F6F5E659ABE3C77C4D4FE404F9
```

map 确认：

```text
sd_uart_dma_ring = 0x24013f20, size=8192
stream_buf       = 0x240184a0, size=76864
```

两者都在 `0x240xxxxx` AXI SRAM，DMA1 可访问，不是 DTCM DMA 访问问题。

### 12.5 复测判读

烧录 `MD5=8426D5F6F5E659ABE3C77C4D4FE404F9` 后，USART1 串口助手仍为 `1500000`。

按最后一条 `[BOOT]` 判断卡点：

- 只看到 `usart1-dma-ready`：卡在 CubeMX 外设初始化之一。
- 到 `oled-init-begin` 后停住：卡在 OLED/SW_I2C。
- 到 `lepton-init-begin` 后停住很久：卡在 Lepton 上电/CCI 诊断。
- 看到 `lepton-init-done dt=...`：Lepton 初始化已退出，后续不是 Lepton init 卡死。
- 看到 `enter-main-loop`：已经进入主循环，LED 应开始按原逻辑翻转。

## 13. Step 9D：撤销 Step 9C 启动打点

状态：已修改 STM32 并重新构建。此版本用于替代 Step 9C，Step 9C 不建议继续烧录。

### 13.1 问题复盘

Step 9C 试图用 `[BOOT]` 串口打点和 LED 翻转定位启动卡点，但这个方案本身引入了扰动：

- 板上 LED 是低电平点亮，`HAL_GPIO_TogglePin()` 在启动阶段多次翻转，可能在进入 Lepton 长初始化前把 LED 翻到熄灭状态。
- `SD_UART_Flush()` 虽然带超时，但启动阶段强行等待 DMA 日志输出，不适合放进本来能工作的初始化链路。
- Lepton CCI 探测超时被缩短，这属于调试性改动，可能改变原本驱动成功的时序。

因此 Step 9C 的判断方法不够稳妥：它把“观察启动”的逻辑混进了启动链路，反而可能改变现象。

### 13.2 本次回退内容

撤销：

- `BOOT_Mark()`
- `SD_UART_Flush()`
- 所有 `[BOOT] ...` 启动期串口打点
- 启动期 `HAL_GPIO_TogglePin()` LED 翻转
- Step 9C 中缩短的 Lepton CCI `HAL_I2C_IsDeviceReady()` 超时

保留：

- USART1 `1500000` + TX DMA ring
- UART4 RAW16 DMA + ping-pong
- `raw=min/max span` 诊断
- `span < 16` 的扁平帧门禁

### 13.3 当前构建

Keil 命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme9_step9d_revert_boot_trace.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=87174 RO-data=9838 RW-data=40 ZI-data=177856
Build Time Elapsed: 00:00:03
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 07:25:45
size: 273128 B
MD5 : BDF8B0E4C1DA0B78115EDACE15505365
```

这个 MD5 与 Step 9B 一致，说明已经回到 Step 9B 代码状态。

### 13.4 当前判断

真正要继续看的不是启动打点，而是视频内容链路：

- 如果此版本能恢复快速启动，说明 Step 9C 的启动打点就是错误方向。
- 如果全绿帧仍存在，看 `[STRM_DIAG] raw=min/max span=N flat=X/Y`。
- 如果 `flat` 增加但 Qt 仍全绿，下一步应查 Qt 侧显示/色表。
- 如果 `flat` 不增加但仍全绿，说明不是简单扁平帧，需要查 VoSPI segment 拼帧内容一致性。
