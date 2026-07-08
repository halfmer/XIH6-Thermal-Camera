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

## 4. Qt 上位机：撕裂帧门禁（整帧原子显示）

### 4.1 需求与根因

用户需求：**收到完整的 160x120 画面再完整显示，宁可延迟也不要割裂感。**

分析结论：Qt 的 FrameParser 本来就是整帧 checksum 校验后才交付、ThermalWidget 也是整帧刷新——"割裂"不是 Qt 半帧刷新，而是 **STM32 persistent-shelf 发布语义**：一帧的 4 个 segment（各 30 行）可能来自不同采集轮次，checksum 完全合法但内容在 30/60/90 行边界处时间错位；画面运动时呈现横向割裂。STM32 侧改 fresh 语义曾致长期 no frame（README_8 §1），故治法放 Qt 端：**显示前做内容级撕裂检测，撕裂帧不上屏**。

### 4.2 实现（commit b3f4c47, esp_uart MD5 9C0A77EC）

```text
framegate.h/cpp (新)     纯函数 FrameGate::analyze():
                         对 3 个 segment 边界(29|30,59|60,89|90)计算平均绝对行差,
                         与帧内非边界行差的中位数比较;
                         判撕裂条件 = 边界差 > max(80 counts, 基线×8)
                         (80 counts=0.8K 绝对下限防噪声误判; ×8 比率防自然渐变误判)
thermalwidget.h/cpp      displayFrame 改 bool 返回; 撕裂→不刷新保持上一帧;
                         连续拒 5 帧后强制放行一帧(防真实热边缘恰在边界上导致画面永久冻结);
                         tornFrames() 计数 / setTearGateEnabled() 开关
mainwindow.cpp           拒帧写 serial_diag.log("frame_torn fid=... torn_total=...");
                         状态栏新增 "撕裂拒显: N"
CMakeLists.txt           esp_uart 与 thermalwidget_selftest 加入 framegate
tests/thermalwidget_selftest.cpp
                         +5 用例: 平滑渐变放行 / 59|60与29|30撕裂拦截 /
                         非边界强水平边缘(45|46)不误杀 / TearReport 字段正确
```

验证：`thermalwidget_selftest` 与 `frameparser_selftest` 均 exit=0；协议、串口、STM32 固件零改动。

### 4.3 使用与判读

- `build_qt_fix/esp_uart.exe`（MD5 `9C0A77EC...`）为新版；**deploy/ 同步需先关闭正在运行的 esp_uart.exe**（同 README_6 的文件锁问题）。
- 上板观察：画面应不再出现横向割裂，代价是运动剧烈时刷新率下降（拒帧期间保持上一帧）——这正是"宁可延迟"的预期行为。
- `serial_diag.log` 的 `frame_torn` 行数 = 被拦截的拼接帧数量；若 `torn_total` 占比很高（>50%），说明 STM32 侧 shelf 拼接严重，届时再评估固件侧 fresh-generation 发布语义（接线修复后 badseg 应已大降，成功率可能足够支撑 fresh 语义）。
- 若某静止场景被持续误拦（理论上不应发生，有防冻结兜底），可临时 `setTearGateEnabled(false)` 对比。

## 5. 优化 Step2：fresh-generation 发布门禁（运动掉帧修复）与 no frame 回归修复

### 5.1 Step2 初版（commit 70eddf8, HEX MD5 0E080167）——上板失败：一直 no frame

针对"运动时 2FPS→0.3FPS"根因（persistent shelf 跨时间拼接帧被 Qt 撕裂门禁拦截），
固件侧加 fresh-generation 门禁：段号回卷/Resync 时 `vospi_gen++`，commit 记
`vospi_seg_gen[seg]`，发布要求 4 段同代。**上板实测：一直 no frame。**

### 5.2 根因定位

`Lepton_Capture_Frame` 的 60 包循环把**段内 discard 包判为 desync，整段作废**。
但段内 discard 是主机读快于包生成（~440µs/包 @SPI 7.5MHz）的**正常流控现象**，
不是失步。旧 shelf 语义下段作废只损失一段、shelf 慢慢攒满仍可发布；fresh 门禁
要求**连续 4 段同代成功**，段成功率 p 一打折 p⁴ 直接崩塌 → 3000 包 guard 耗尽 →
长期 no frame。**同一机制正是当年 fresh_mask 失败（README_8 §1）的真因**——
不是 fresh 语义本身错，是段内 discard 误判拖低了段成功率。

### 5.3 修复（commit f1380bf, HEX MD5 B94FCF90A6D94ED8487741D54D68F87F）

```text
lepton.c  60包循环: discard 包不再判 desync, HAL_Delay(1) 重读同包,
          上限 LEPTON_VOSPI_INTRA_DISCARD_MAX=8 次, 超限才按原 desync 处理
lepton.c  发布块防饿死: 单次 capture 内 stale 拦截达
          LEPTON_VOSPI_STALE_FORCE_PUBLISH=4 次即放行拼接帧
          (退化为旧 shelf 行为, 最坏不劣于 Step1 基线, 不可能比 no frame 差)
lepton.h  Lepton_Diag_t 新增 uint16_t vospi_stale_block
main.c    诊断行新增 stale=N
构建: 0E/0W, Code=75006, log=build_step2b_noframe_fix.log
```

### 5.4 上板验证清单（本版）

1. 烧录 `B94FCF90A6D94ED8487741D54D68F87F`；USART1=115200，UART4=1.5M。
2. `[LEP] OK` 应恢复出图（最坏退化为旧拼接行为）。
3. 诊断行判读：
   - `stale=` 持续为 0 且出图 → fresh 门禁全命中，运动割裂应消失；
   - `stale=` 偶发 1~3 → fresh 大部分命中，可接受；
   - `stale=` 持续 ≥4（防饿死频繁触发）→ 段成功率仍低，下一步调
     INTRA_DISCARD_MAX 或改 µs 级重试节奏；
   - `desync=` 应比 0E080167 版明显下降。
4. 运动场景：Qt FPS 不应再掉到 0.x；`frame_torn` 占比应大降。
5. 失败回退：`git checkout 84a944a -- .`（Step1 DMA 版）。

### 5.5 B94FCF90 仍 no frame——真根因：seg=0 哑段被当失败处理

上板 B94FCF90 仍 no frame。用 pyserial 直抓 USART1 完整诊断行
（`uart_noframe_B94FCF90.log`）：

```text
reason=1(guard) reads=3000 valid=2667 discard=333 pkt0=52 desync=11
badseg=38 stale=0 mask=0x0F seen=5/6/3/0 segs=1/2/0/0 bad0=38
```

判读推翻 §5.2 的"discard 误判"主因假设（那只是次要因素）：

1. **`bad0=38/52`（73%）= Lepton 3.5 正常哑段**——传感器内部 26Hz、导出限
   8.7Hz，每 3 个帧槽 2 个以 seg=0 输出，规范要求主机静默读完丢弃；
2. 旧代码把哑段当 `bad_seg` 失败：`HAL_Delay(1)`+重猎 pkt0，打断包级同步，
   紧随哑段之后的**真段**大量 desync（14 真段仅 ~3 完成，`desync=10`），
   seg4 每 capture 至多完成 1 次 → fresh 门禁永无机会通过；
3. `stale` 防饿死计数每次 capture 被 DiagReset 清零，阈值 4 永不可达 →
   防饿死失效，退化路径也断了。

**修复（commit 646e257, HEX MD5 4B1A95822967468A1F91C678030E488F, 0E/0W）**：
①seg=0 仅计 bad0，继续读完 60 包，不 fail 不延时；②防饿死改用跨 capture
静态 `vospi_stale_streak`，任何发布（fresh 或强制）清零。

上板判读：`segs=` 四路完成计数应大幅上升（真段成功率 ~25%→接近 100%）、
`desync` 应降到个位、出图恢复且 fresh 命中（`stale` 低位徘徊）。
失败回退 `git checkout 84a944a -- .`。

### 5.6 4B1A9582 时不时 no frame——判决：整体回退 2FPS 金标准（commit 3084e4f, HEX MD5 C87FBCC8）

哑段修复版实测（`uart_4B1A9582.log`）：`seen=2/3/4/3` 四路均衡（哑段跳过生效），
但 `desync=25~27`、失败点 `exp=46`（段中部）且收包号跳变——**段内 discard 重试环
的 HAL_Delay(1)×N 累积延时超过 Lepton 段窗口，传感器跳段**，比 2FPS 版（对段内
discard 立即放弃重猎）反而更差。

与 `XIH6_V3_2FPS` 快照 diff 定责：
- `lepton_stream.c/h` 逐字节一致、`main.c` 仅 `stale=` 打印差异 →
  **上位机与传输协议无任何改动，no frame 全部源于 lepton.c 采集路径的 Step2 改动**；
- Qt 侧只有显示层撕裂门禁（b3f4c47），不可能造成 STM32 采集层 no frame。

fresh 门禁两轮修补（f1380bf、646e257）均引入新问题，按单变量红线**整体回退**：
lepton.c 直接取自 2FPS 快照（0E/0W，Code=74870），运动割裂交 Qt 撕裂门禁处理。
教训入红线：**采集时序问题禁止理论修补，必须先抓完整诊断行定责再动手**；fresh
发布语义若再评估，改动方向应是"无延时跳过哑段"而非"段内加延时重试"。

## 6. 防复发红线（继承 README_11 §5，长期有效）

1. 栈红线：>128B 日志缓冲禁止放栈上；评估栈时按"主线程最深 + 最深中断链"计算（现 8KB 有余量）。
2. 时序红线：UART4 DMA 启动后的热路径禁止任何阻塞打印。
3. 版本红线：每个可烧录状态一个 git commit（HEX 入库），不再存在"无法回退"的状态。
4. 单变量红线：一次只改一个功能点，烧录验证后再叠加。
