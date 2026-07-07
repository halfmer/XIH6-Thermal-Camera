# README_8 - VoSPI 提帧率回退到稳定基线

日期：2026-07-07

## 1. 当前结论

前两次提帧率尝试都失败：

- `fresh_mask` 发布门槛太严，导致长期 `no frame`。
- 条件 resync 虽然一度能冲到约 `2.8 FPS`，但会出现错误画面和长期低 FPS。
- 撤掉 `fresh_mask` 后仍然 `no frame`，说明条件 resync 本身也不能保留。

本轮已回退到 README_7 的稳定基线：失败就立即 `185 ms` VoSPI resync，不再做“攒 segment 时跳过 resync”的优化。

## 2. 当前代码状态

### 2.1 stream 模式恢复为失败即 resync

文件：`Core/Src/main.c`

```c
if (Lepton_Stream_Active())
{
    if (Lepton_Capture_Frame())
        Lepton_Stream_SendFrame();
    else
        Lepton_VoSPI_Resync();
    LED_TURN(1);
    continue;
}
```

已移除：

- `stream_no_progress`
- `stream_last_mask`
- `LEP_STREAM_NO_PROGRESS_RESYNC_LIMIT`
- `stream_resync` 条件诊断

### 2.2 VoSPI retry 恢复为 HAL_Delay(1ms)

文件：`Drivers/PER/LEPTON/lepton.c`

```c
#define LEPTON_VOSPI_FIRST_DISCARD_WAIT_MS 1U
#define LEPTON_VOSPI_RETRY_WAIT_MS         1U
```

所有 VoSPI retry/discard 短等待恢复为：

```c
HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
```

不再使用 `delay_us(250)` 或 `delay_us(1000)` 参与 VoSPI retry。

### 2.3 发布逻辑恢复为 README_7 基线

保留 persistent shelf：

```c
seg == 4 && (vospi_cached_mask & 0x0F) == 0x0F
```

不清 `vospi_cached_mask`，不使用 `fresh_mask`。

## 3. 构建验证

Keil 命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme8_stable_revert.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=71710 RO-data=9410 RW-data=40 ZI-data=131024
Build Time Elapsed: 00:00:03
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 05:00:31
size: 228398 B
MD5 : 315D8DC9F8655E420457AB7A0DB2A698
```

这个 MD5 与 README_7 记录的 1.5Mbps 稳定基线一致。

## 4. Step 1：只加诊断，不改行为

本步骤按“一次只改一个点”执行：只在 stream 模式加入 USART1 低频统计，不改变：

- `Drivers/PER/LEPTON/lepton.c` 的 VoSPI retry/resync 时序。
- `seg == 4 && mask == 0x0F` 的发布逻辑。
- UART4 二进制帧格式。
- Qt 上位机解析逻辑。

新增位置：`Core/Src/main.c`

- `Lepton_Capture_Frame()` 前后用 `HAL_GetTick()` 统计采集耗时。
- `Lepton_Stream_SendFrame()` 前后统计 UART4 发送耗时。
- stream 模式下每约 `5s` 从 USART1 打印一行：

```text
[STRM_DIAG] 5xxxms cap_ok=... cap_fail=... resync=... send_ok=... send_fail=... cap_avg=...ms cap_max=...ms send_avg=...ms send_max=...ms fid=... reason=... mask=0x.. ok=./././. seen=./././. reads=... discard=... desync=... badseg=... spierr=...
```

字段判读：

- `cap_ok/send_ok` 持续增加：STM32 已经在采集并向 UART4 发完整帧。
- `cap_fail/resync` 很高、`cap_ok` 很少：问题仍在 VoSPI 同步/采集侧。
- `send_fail > 0`：UART4 发送侧有阻塞或错误。
- `send_avg` 正常应接近 UART4 传一帧的时间；`1.5Mbps` 下 `38415B` 约 `256ms`。
- 如果 `cap_ok/send_ok` 都正常但 Qt 画面仍乱，下一步优先查上位机缓存/校验/显示链路。

构建命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme8_step1_diag.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=72158 RO-data=9662 RW-data=40 ZI-data=131056
Build Time Elapsed: 00:00:04
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 05:16:03
size: 230378 B
MD5 : 07AAEE85219C98CE149380C65C423986
```

本版本烧录后，先不要继续改 FPS。只观察：

1. USART1 是否每约 `5s` 出现 `[STRM_DIAG]`。
2. `cap_ok/cap_fail/resync` 的比例。
3. `send_ok/send_fail/send_avg` 是否符合 `1.5Mbps` 阻塞发送预期。
4. Qt 端 `serial_diag.log` 是否仍有 checksum mismatch 或视频错误。

## 5. Step 2：压缩估算尝试已回滚

目标：评估“减少 UART4 阻塞发送时间”是否值得继续做，但不立即改变上位机协议。

状态：失败，已回滚到 Step 1 诊断固件。

上板现象：

- 灯不翻转。
- USART1 没有输出。
- 因为启动阶段在主循环前就应打印 `It's mygo!!!!!`、`[RST]`、`[STREAM]`，所以这版不能继续作为有效测试固件。

处理：

- 已删除 STM32 运行期压缩估算代码。
- 已删除 `[STRM_DIAG]` 中的 `cmp_n/row12/eff/pack/rawrow/span/fspan/cmp_ms` 字段。
- `Drivers/PER/LEPTON/lepton_stream.h/.c` 已恢复到 Step 1 接口。
- `Core/Src/main.c` 已恢复到 Step 1 的采集/发送诊断。
- PC 仿真脚本 `tools/lepton_compress_sim.py` 保留，它不参与固件构建，不影响板子运行。

当前有效构建：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 05:51:22
size: 230378 B
MD5 : 07AAEE85219C98CE149380C65C423986
```

构建结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=72158 RO-data=9662 RW-data=40 ZI-data=131056
Build Time Elapsed: 00:00:03
```

根因判断：

- 编译通过不代表运行安全；Step 2 在 stream 诊断路径加入了更长 `sprintf` 和额外遍历估算，缺少上板前的最小化门禁。
- 虽然这些代码理论上只在 stream 模式后执行，但上板已经表现为启动链路不可用，必须按失败版本处理，不再继续叠加修改。
- 真正下一步不能再把“压缩估算”和“运行固件”绑在一起，应先做更保守的开关式验证：默认关闭，收到明确命令后只打印一次短诊断。

以下保留 Step 2 的离线思路，只作为失败记录，不代表当前固件包含这些逻辑。

本步骤仍然不改变：

- UART4 发送的 `type=0x01` RAW16 帧。
- `payload_len=38400` 的现有协议。
- Qt `FrameParser`。
- VoSPI 采集、resync、segment 发布逻辑。

### 5.1 逻辑链路

当前稳定链路：

```text
Lepton VoSPI -> lepton_raw_frame[120][160] -> RAW16打包 -> UART4/CH340 -> Qt FrameParser -> ThermalWidget
```

本步骤只在 RAW16 发送完成后增加旁路估算：

```text
lepton_raw_frame[120][160] -> row12-mix长度估算 -> USART1 [STRM_DIAG]
```

也就是说，Qt 仍然收到原始 RAW16；压缩估算只出现在 USART1 文本日志里。

### 5.2 row12-mix 候选格式

先选择固定、可回退、低风险的 `row12-mix`：

- 每帧先有 `15B` 行模式位图，覆盖 `120` 行。
- 若某行 `row_max - row_min <= 4095`，该行编码为：
  - `row_min`：2B
  - `160` 个 12-bit delta：`160 * 12 / 8 = 240B`
  - 合计 `242B/行`
- 若某行跨度超过 `4095`，该行保留 RAW16：
  - `160 * 2 = 320B/行`
- 如果估算长度不小于 RAW16，未来真正发送时应直接保留 RAW16。

典型全行可压缩时：

```text
15 + 120 * 242 = 29055 B
38400 -> 29055，约 75.7%
```

### 5.3 PC 仿真验证

新增脚本：

```text
tools/lepton_compress_sim.py
```

运行：

```powershell
python tools\lepton_compress_sim.py
```

本轮结果：

```text
row12_mix round-trip validation
frame=160x120 raw=38400B flags=15B
case              len    ratio   packed raw
flat              29055   0.757    120   0
smooth            29055   0.757    120   0
hotspot           29055   0.757    120   0
mixed_rows        29991   0.781    108  12
wide_span         38415   1.000      0 120
boundary_4095     29055   0.757    120   0
boundary_4096     38415   1.000      0 120
random_noise      38415   1.000      0 120
PASS: row12_mix decode output exactly matches every input frame
```

结论：

- 编码/解码对所有测试帧 bit-exact。
- `4095` 边界可压，`4096` 自动回退。
- 宽温差/随机数据不会错误压缩，只是收益为 0。

### 5.4 已撤销的 STM32 诊断改动

曾经新增、现已删除：

- `Drivers/PER/LEPTON/lepton_stream.h`
  - `Lepton_Stream_CompressDiag_t`
  - `Lepton_Stream_EstimateCompression(...)`
- `Drivers/PER/LEPTON/lepton_stream.c`
  - 遍历 `lepton_raw_frame`，计算 row12-mix 估算长度。
- `Core/Src/main.c`
  - 成功采集并完成原 RAW16 发送后记录估算值。
  - 每约 `5s` 合并进 `[STRM_DIAG]`。

新增字段：

```text
cmp_n=样本数
row12=min/avg/max估算payload字节
eff=未来会选择的有效payload字节，等于 min(row12,38400)
pack=最后一帧可12bit编码的行数
rawrow=最后一帧回退RAW16的行数
span=最后一帧最大单行跨度
fspan=最后一帧整帧跨度
cmp_ms=估算耗时 avg/max，HAL tick 毫秒粒度
```

判读：

- `cmp_n` 应接近 `send_ok`。
- `row12_avg` 若长期明显低于 `38400`，压缩协议值得进入下一步。
- `pack` 越接近 `120`，row12 越适合当前热像画面。
- `rawrow` 经常很高，说明 row12 不够，应先评估 delta8/分块方案。
- `cmp_ms` 若明显上升，说明估算本身开始干扰采集节奏。

失败构建命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme8_step2_cmpdiag.log
```

失败构建结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=72830 RO-data=9750 RW-data=48 ZI-data=131080
Build Time Elapsed: 00:00:03
```

失败产物，不再烧录：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 05:40:50
size: 232538 B
MD5 : 55ADDE32E65446660668135340563A64
```

## 6. 下一步复测

烧录这个 HEX 后，只验证是否恢复 README_7 的现象：

- Qt 可以重新显示 `160x120` 画面。
- 帧率可以低，先不追求 2.8 FPS。
- USART1 不应长期只有 `no frame`。
- USART1 的 `[STRM_DIAG]` 应为 Step 1 短格式，不应出现 `cmp_n/row12/eff/pack/rawrow/span/fspan/cmp_ms` 字段。

如果这个 MD5 的固件仍然一直 `no frame`，那就不是当前提帧率代码残留导致的，而要检查：

- 若复测稳定基线：是否确实烧录了 `MD5=315D8DC9F8655E420457AB7A0DB2A698` 的 HEX。
- 若复测 Step 1 诊断版：是否确实烧录了 `MD5=07AAEE85219C98CE149380C65C423986` 的 HEX。
- 不要再复测 Step 2 压缩估算版 `MD5=55ADDE32E65446660668135340563A64`。
- 串口日志是否来自同一块正在运行的板子。
- 当前 Lepton 上电状态/MCLK/线缆/供电是否和 README_7 成功时一致。
- UART4 上位机是否处于热成像 stream 模式，USART1 是否还能打印启动诊断。

## 7. 复盘约束

后续再提 FPS 时，不能再同时改多个变量。顺序应为：

1. 从这个 MD5 稳定基线开始。
2. 只改一个点，例如 SPI4 分频或 UART4 DMA。
3. 烧录后先验证是否仍能出正确画面。
4. 再看 FPS，不以“协议帧通过”代替“图像正确”。

## 8. Step 3：UART4 TX DMA + Ping-Pong（当前测试版）

目标：先解决 UART4 RAW16 阻塞发送占用 CPU 的问题，不改变 VoSPI 采集、不改变 Qt 协议、不做压缩。

本步骤保持不变：

- UART4 波特率仍为 `1500000`。
- 帧类型仍为 `0x01` RAW16。
- payload 仍为 `38400B`，整帧仍为 `38415B`。
- Qt `FrameParser` 不需要修改。
- VoSPI retry/resync/segment 发布逻辑不修改。

### 8.1 修改内容

文件：`Drivers/PER/LEPTON/lepton_stream.c`

- 原来的单缓冲：

```c
static uint8_t stream_buf[LEPTON_STREAM_FRAME_LEN];
HAL_UART_Transmit(stream_huart, stream_buf, LEPTON_STREAM_FRAME_LEN, 400);
```

改为双缓冲 + DMA：

```c
static uint8_t stream_buf[2][LEPTON_STREAM_FRAME_LEN] __attribute__((aligned(32)));
HAL_UART_Transmit_DMA(stream_huart, buf, LEPTON_STREAM_FRAME_LEN);
```

- 新增 `hdma_uart4_tx`，使用 `DMA1_Stream1 / DMA_REQUEST_UART4_TX / DMA_NORMAL`。
- `Lepton_Stream_SendFrame()` 现在只负责：
  1. 把 `lepton_raw_frame` 打包进当前 ping-pong buffer。
  2. 清 DCache（仅在 DCache 已开启时执行；当前工程未开启 DCache）。
  3. 启动 UART4 TX DMA 后立即返回。
- 如果上一帧 DMA 仍在发送，则返回 `HAL_BUSY` 并计入 `drop`，不等待串口，避免重新打断 VoSPI。
- 新增 `HAL_UART_TxCpltCallback()`，UART4 DMA 发送完成后清 `busy` 并增加 `cplt`。

文件：`Core/Src/stm32h7xx_it.c`、`Core/Inc/stm32h7xx_it.h`

- 新增 `DMA1_Stream1_IRQHandler()`：

```c
void DMA1_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_uart4_tx);
}
```

### 8.2 新增串口判读字段

USART1 低频 `[STRM]` 增加：

```text
cplt=... drop=... dmaErr=... busy=... txi=...
```

判读：

- `ok`：DMA 成功接收发送请求的帧数，不代表线缆上已经发完。
- `cplt`：UART4 DMA/TC 完成回调次数，应持续追上 `ok`。
- `drop`：采集到新帧时 UART4 还在发送上一帧，因此丢弃当前帧。
- `dmaErr`：DMA 启动失败或 UART 错误导致 TX 状态恢复。
- `busy`：当前是否有 RAW16 帧正在后台发送。
- `txi`：当前 DMA 正在发送的 ping-pong buffer 编号。

注意：`[STRM_DIAG] send_avg/send_max` 从本步骤开始不再表示 UART 线缆传输耗时，而是 `Lepton_Stream_SendFrame()` 调用耗时，也就是“打包 + 启动 DMA”的耗时。RAW16 在 `1.5Mbps` 物理链路上仍约需要 `256ms/帧` 才能真正发完。

### 8.3 构建验证

Keil 命令：

```powershell
cd D:\stm32_project\XIH6_V3\MDK-ARM
& E:\keil_v5\UV4\uVision.com -r XIH6_V2.uvprojx -t XIH6_V2 -o build_readme8_step3_uart4_dma.log
```

结果：

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=75486 RO-data=9706 RW-data=40 ZI-data=169632
Build Time Elapsed: 00:00:04
```

产物：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 06:13:41
size: 239918 B
MD5 : 2F515B2EEED65390F0919213FF462E79
```

map 关键点：

```text
stream_buf    0x240164a0  size=76830  AXI SRAM
hdma_uart4_tx 0x24000cc8  size=120    AXI SRAM
```

### 8.4 上板验证顺序

烧录 `MD5=2F515B2EEED65390F0919213FF462E79` 后按这个顺序看：

1. USART1 仍应打印启动日志，LED 仍应在主循环翻转。
2. Qt 发送 `S` 后，USART1 应继续有 `[STRM_DIAG]` 和低频 `[STRM]`。
3. `[STRM] cplt` 应持续增加；如果 `ok` 增加但 `cplt` 不动，优先查 DMA1_Stream1 IRQ 或 UART4 IRQ。
4. 当前约 `1 FPS` 场景下，`drop` 理论上应很低；如果 `drop` 很高，说明 RAW16 串口带宽已被打满。
5. Qt 画面必须先保持正确，再谈 FPS。若协议 checksum 重新乱跳，先看 `dmaErr/drop/cplt`，不要直接进入 8-bit 灰度改协议。

下一步只有在本版本画面正确、`cplt` 正常后再做：新增 8-bit 灰度帧类型，把 payload 从 `38400B` 降到 `19200B`，并同步修改 Qt 解析。
