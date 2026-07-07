# README_12 - 基线固化与优化 Step1：UART4 TX DMA

日期：2026-07-07

## 1. 背景：问题闭环

README_11 记录的"启动死机/启动慢/no frame"问题已全部闭环：

1. **彻底死机** → 根因=1KB 栈溢出（README_11 §4），8KB 栈修复；
2. **启动慢 + no frame** → 用户排查确认**部分为硬件接线问题，已修复**；软件侧同时把固件对齐到 `XIH6_V3_1FPS` 金标准快照（README_11 §7.4-7.5），消除全部手工恢复引入的不确定性。

对齐的等价性证明（数学级）：把栈临时改回 `0x400` 构建，产物 MD5 与金标准完全一致：

```text
本工程构建(Stack=0x400) : 315D8DC9F8655E420457AB7A0DB2A698
XIH6_V3_1FPS 快照产物    : 315D8DC9F8655E420457AB7A0DB2A698   ← 逐字节一致
```

## 2. git 基线（今后一切优化的回退锚点）

```text
84a944a 优化Step1: UART4 TX DMA + Ping-Pong        (HEX MD5 8088DB79)  ← 当前, 待上板
0324bba 基线+8KB栈                                  (HEX MD5 90AF7930)  ← 已知好行为 + 栈保险
91cf1f8 对齐 _1FPS 金标准复现点                     (HEX MD5 315D8DC9)  ← 与金标准逐字节一致
4455a8c Step A: 栈 1KB→8KB                          (HEX MD5 F0304834)
f216099 初始提交: Step9D 手工恢复现场(死机版)       (HEX MD5 55B9AED8)
```

任何一步出问题：`git checkout <hash> -- .` 即可精确回退，HEX 随源码入库可直接烧录对账。

## 3. 优化 Step1：UART4 TX DMA + Ping-Pong

### 3.1 动机与历史教训

阻塞发送一帧占用 CPU ~256ms（38415B @1.5Mbps），期间完全无法采集 VoSPI；README_8 Step3 已实测 DMA 化可到 ~2FPS。本次基于 `_1FPS` 干净基线重新实现，**一次性内置两条已验证教训**：

- README_8 Step3 结构：双缓冲 ping-pong、DMA1_Stream1、busy 即丢帧（不阻塞采集循环）；
- README_9 Step9A 判决：**成功启动 DMA 后零打印**（当年每 16 帧的 [STRM] 阻塞打印造成 6% 周期性 checksum=0x0000 坏帧）；busy drop 同样零打印；`[STRM]` 仅在 `HAL_UART_Transmit_DMA` 启动失败时输出。

### 3.2 修改内容（本步只动 3 个文件，main.c 零改动）

```text
Drivers/PER/LEPTON/lepton_stream.c
  - stream_buf: 单缓冲 38415B → stream_buf[2][38432] __attribute__((aligned(32)))
  - 新增 hdma_uart4_tx (DMA1_Stream1 / DMA_REQUEST_UART4_TX / NORMAL / prio5)
  - SendFrame: busy→fail++/drop++/return HAL_BUSY(静默); 打包→CleanDCache(条件)→Transmit_DMA
               →OK: fid++/build_index翻转/ok++; 失败: 清busy+计数+[STRM]错误打印(阻塞@115200,异常路径安全)
  - 新增 HAL_UART_TxCpltCallback: UART4 发送完成清 busy, cplt++
  - ErrorCallback: gState==READY 时释放 ping-pong busy, 重挂 'S'/'P' 接收
Core/Src/stm32h7xx_it.c   + extern hdma_uart4_tx, + DMA1_Stream1_IRQHandler
Core/Inc/stm32h7xx_it.h   + DMA1_Stream1_IRQHandler 声明 (USER CODE EFP 区)
```

保持不变：AA55 RAW16 协议、`LEPTON_STREAM_BAUD=1500000`、VoSPI 采集/发布逻辑（persistent shelf）、USART1=115200 阻塞日志、Qt 上位机。

### 3.3 构建产物

```text
"XIH6_V2\XIH6_V2.axf" - 0 Error(s), 0 Warning(s).
Program Size: Code=74846 RO-data=9454 RW-data=40 ZI-data=176768
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex
MD5 : 8088DB794EFF2B304149517887E5BBDD
log : MDK-ARM/build_step1_uart4_dma.log
map : stream_buf=0x24016460/76864, hdma_uart4_tx=0x24000c88  (AXI SRAM, DMA1 可达)
```

### 3.4 上板验证清单

1. 烧录 `MD5=8088DB794EFF2B304149517887E5BBDD`。
2. **USART1 串口助手 = 115200**（本版调试口不是 1.5M）；UART4 上位机 = 1500000。
3. 按序确认：
   - 上电启动速度与 `_1FPS` 版一致（LED 按原节奏起翻、启动日志正常）；
   - idle 模式每 ~8s 一次 `[LEP] OK c_raw=... c=...C`（接线修复后应稳定 OK）；
   - Qt 发 `S` 开流：画面正确、FPS 观察是否从 ~1 升到 ~2；
   - `serial_diag.log` 中 `checksum_bad` 必须为 0（若出现周期性坏帧 = Step9A 类时序问题回潮，立即回退 `0324bba`）；
   - USART1 不应出现 `[STRM]` 行（它只在 DMA 启动失败时打印，出现即说明发送侧有异常）。

### 3.5 判定与下一步

- **通过**（画面正确 + checksum_bad=0 + FPS≈2）→ 本版成为新基线，进入 Step2；
- **失败** → `git checkout 0324bba -- .` 回退重建，问题记录后再议。

后续路线（沿 README_9 §7 顺序，每步一个 commit + 上板）：

```text
Step2: 内容质量统计(STM32/Qt 双端 min/max/span 对账, 只加诊断不改行为)
       → 若接线修复后全绿帧已消失, 此步可简化或跳过
Step3: VoSPI 采集成功率优化(视 Step2 数据决定方向)
Step4: 8-bit 灰度帧类型(payload 38400→19200) 或压缩(tools/lepton_compress_sim.py 已验证 row12-mix)
       → 仅在协议+内容双稳定后进入
```

## 4. 防复发红线（继承 README_11 §5，长期有效）

1. 栈红线：>128B 日志缓冲禁止放栈上；评估栈时按"主线程最深 + 最深中断链"计算（现 8KB 有余量）。
2. 时序红线：UART4 DMA 启动后的热路径禁止任何阻塞打印。
3. 版本红线：每个可烧录状态一个 git commit（HEX 入库），不再存在"无法回退"的状态。
4. 单变量红线：一次只改一个功能点，烧录验证后再叠加。
