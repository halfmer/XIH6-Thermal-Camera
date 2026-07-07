# README_6 — 上位机无法解析热成像视频流：代码分析定位与修复

日期：2026-07-06
前置：README_5（USART1/UART4 职责拆分、checksum mismatch 初步排查、STM32 帧发布逻辑修复，HEX 2026-07-05 22:55）。

## 1. 问题现象

热成像(串口)模式下，Qt 上位机持续报：

```text
协议错误: Checksum mismatch: gotBE=... gotLE=... expected=... expectedNoSync=... diff=...
```

且四个 checksum 值每帧都对不上、数值乱跳，几乎无法出图。而离线自测（README_5 §9/§10：模拟 2 Mbps 分片、字节翻转/丢失/插入）全部通过，证明 STM32 组帧与 Qt FrameParser 的协议逻辑自洽。矛盾点：**为什么逻辑自洽的两端，接上真实串口就帧帧损坏？**

## 2. 代码分析方法

本轮不再改协议、不再猜链路，而是对整条数据链路做三类静态分析：

1. **数据流审计**：从 STM32 `Lepton_Stream_SendFrame()` 逐字节追到 Qt `ThermalWidget::displayFrame()`，核对每一跳的缓冲、并发与所有权。
2. **时序/资源预算**：对 2 Mbps（= 200 B/ms）的持续流，计算每个环节的时间预算和缓冲余量，找出"卡多久就丢数据"的硬上限。
3. **失效模式匹配**：把每个候选缺陷的理论症状与实测现象（帧帧损坏、数值乱跳、离线自测通过）对比，排除不符项。

### 2.1 发送侧（STM32）审计结论：干净

| 检查项 | 结论 |
|---|---|
| UART4 内核时钟 | `usart.c` MspInit 选 `D2PCLK1` = 120 MHz，2 Mbps = 120M/60，**0% 波特率误差** |
| 发送方式 | `HAL_UART_Transmit()` 阻塞轮询 TDR 空标志，逐字节由硬件节拍推出——**软件慢只会拖慢发送，不可能产生错字节** |
| 发送缓冲并发 | `stream_buf` 组帧完成后才发送；发送期间主循环阻塞、无中断改写它；`lepton_raw_frame` 只在 `Lepton_Capture_Frame()` 里更新，与发送严格串行 |
| RX 中断干扰 | UART4 RX 中断只在收到 'S'/'P' 字节时触发，与 TX 无共享状态 |
| `[STRM]` 诊断 | 走 USART1，不进 UART4 二进制流 |

**结论：MCU 推到 CH340C 引脚上的字节流是完整正确的。损坏只能发生在 CH340C→USB→Windows 驱动→Qt 这段，其中软件可控的是 Qt 接收路径。**

### 2.2 接收侧（Qt）审计发现

按严重度排列：

**P1【根因】串口收发跑在 GUI 主线程，内核缓冲余量仅 ~20-40 ms，主线程一卡就静默丢字节。**

- 数据路径：`readyRead`（主线程信号）→ `readData()` → `readAll()`。在 Windows 上，Qt 重新挂起内核异步读请求的动作也发生在主线程事件循环里。
- 内核/CH340 驱动的接收缓冲只有几 KB。2 Mbps 下 200 B/ms，**主线程任何超过 20~40 ms 的停顿（重绘、日志、写盘、弹窗、拖动窗口）都会让内核缓冲溢出**。
- **Qt 6 已删除 `OverrunError` 枚举**——溢出丢字节完全静默，errorOccurred 不会告诉你。
- 恰恰每收到一帧好帧，主线程要做最重的活：38.4 KB 字节序转换 + ColorMap 生成 QImage + 全窗口 `SmoothPixmapTransform` 重绘 + 状态栏刷新 + 诊断日志 + 周期性磁盘写。**处理上一帧的开销砸进下一帧的传输窗口 → 下一帧丢字节 → checksum 必错**。
- 这同时解释了三个观测：离线自测通过（没有真实内核缓冲约束）、实机帧帧损坏（每帧都撞上重绘窗口）、错误值乱跳（丢失位置随机）。

**P2【致命可用性】`handleSerialError` 对任何错误直接 `close()` 串口。**

- `setSerialThermalStream()` 里的 `waitForBytesWritten(50)` 超时会触发 `TimeoutError` —— 旧代码收到它就把串口关了，视频流"莫名断流"。
- 高吞吐下偶发的 `ReadError` 同样被当成致命错误直接断开。
- 错误文本只 append 到"串口调试"页的 QTextEdit，热成像页什么都看不到。

**P3【放大器】`appendSerialDiag` 的 1 MiB 滚动窗口，写满后每条日志两次 ~1 MB memmove。**

- 稳态（约 85 秒后写满）下每条日志触发 `remove(0,n)` + `prepend(marker)`，各搬移约 1 MB。
- rx_chunk 每秒 25~50 条 + 错误日志若干 → **每秒上百 MB 的纯内存搬运在主线程**。
- 错误越多 → 日志越多 → 越卡 → 丢更多字节 → 更多错误。**恶性循环放大器**。

**P4【放大器】`writeSerialDiagFile` 每 250 ms 全量 Truncate 重写最多 1 MB → 持续 4 MB/s 磁盘写在主线程**（遇到机械盘/杀毒钩子时单次可达几十 ms）。

**P5【常态开销】`logSerialChunk` 对每个 chunk 都做 AA55 扫描 + head/tail `toHex()` 格式化 + 多段 `QString::arg`。**

**P6【次要】调试模式 hex 显示的 `m_debugFlushTimer` 是 singleShot，`readData` 每个 chunk 都 `start()` 重置 → 高速流下永不超时，显示饿死、`m_debugBuf` 无限增长。**

**P7【次要】`onParseError` 每个坏帧都 QLabel `setText`+`show`，错误风暴时附加重绘负载。**

**P8【诊断盲区】checksum_bad 日志有 `nextSync` 和 `total` 但没直接给出差值，人工判"丢字节还是错字节"要心算。**

## 3. 修复内容

### 3.1 核心：串口收发移入独立工作线程（修 P1、P2）

新增 `serialworker.h/.cpp`：

- `QSerialPort` 作为 `SerialWorker` 的子对象随其 `moveToThread(m_serialThread)`，open/close/write/'S'/'P'/DTR/RTS 全部经 `QMetaObject::invokeMethod` 队列投递到工作线程执行。
- 工作线程的 `readyRead` 直连 `readAll()`，立即抽干内核缓冲，把数据用 queued signal（QByteArray 隐式共享，零拷贝）发给主线程。**内核缓冲只需覆盖工作线程的微小延迟；主线程卡顿时数据积压在进程内事件队列（弹性、不丢），而不是内核固定缓冲（刚性、静默丢）**。
- 错误处理重写：`TimeoutError`（来自 waitFor* 超时）直接忽略；`ReadError/WriteError` 等瞬态错误只上报不关闭；只有 `ResourceError`（设备拔出）/`DeviceNotFoundError`/`PermissionError`/`OpenError` 才关闭串口。错误信息写入 serial_diag.log 并显示在状态栏（热成像页也能看到）。
- `waitForBytesWritten` 只在工作线程里调用（'P' 停流排空、关闭前排空），GUI 永不阻塞。

`mainwindow.h/.cpp` 相应重构：`m_serialPort` 成员移除，改为 `m_serialThread + m_serialWorker + m_serialOpen` 状态镜像；`readData()` 改为 `onSerialBytes(QByteArray)`；新增 `onSerialOpened/onSerialClosed/onSerialPortError` 回调。UI 行为（按钮、状态栏、模式切换、'S'/'P' 时机）保持不变。

### 3.2 诊断系统轻量化（修 P3、P4、P5、P8）

- **内存窗口摊销**：超过 1 MiB 时一次性砍掉前一半（对齐行边界），摊销后每字节日志只搬移 O(1) 次；废除每条 prepend marker，改为砍半时 append 一条 `trimmed_old_bytes_total`。
- **磁盘写增量化**：新增 `m_serialDiagPending` 待写尾巴，`writeSerialDiagFile` 以 Append 模式只写增量；文件超 4 MiB 时 rotate（用内存窗口重写一次）；刷盘间隔 250 ms → 1000 ms；开串口时删旧文件重新开始。
- **chunk 日志采样**：前 32 条全记（保留开流时刻的现场），之后每 32 条记 1 条；不采样时连 AA55 扫描都跳过。
- **frameparser 诊断加 `syncDelta`**：`nextSync - totalLen`，负值=链路丢字节、正值=多字节，一眼定性。

### 3.3 次要修复（P6、P7）

- `m_debugFlushTimer->start()` 加 `isActive()` 保护，hex 显示不再饿死。
- 协议错误标签节流到最多 4 次/秒刷新（诊断日志仍逐条记录，不丢信息）。

### 3.4 本轮不动的部分

- **STM32 固件零改动**：22:55 HEX（README_5 §14 帧发布修复版）尚未上板验证，不引入新变量。发送侧审计已证明它不是字节损坏来源。
- FrameParser 状态机、AA55 协议、checksum 规则：不变（自测已锁定其正确性）。

## 4. 构建与验证

```text
cmake --build build_qt_fix --config Release  -> [100%] Built target（0 错误）
frameparser_selftest.exe   -> exit=0（含 2 Mbps 分片模型、翻转/丢/插字节注入，共 100+ 随机 seed）
thermalwidget_selftest.exe -> exit=0
esp_uart.exe 已同步：build_qt_fix/ == deploy/ == build/（md5 一致）
```

## 5. 上板复测步骤与对账方法

1. 烧录 `MDK-ARM/XIH6_V2/XIH6_V2.hex`（22:55 版，若尚未烧录）。
2. USART1@115200 确认 `[LEP] OK`（mask 攒满后 seg4 触发）与 `[STRM] fid=... st=0 fail=0`。
3. 运行 `deploy/esp_uart.exe`，切"热成像(串口)"，2,000,000 bps 打开 CH340 串口。
4. **判读（关键改进：现在能定量对账）**——看 exe 同目录 `serial_diag.log`：
   - **出图且 `frame_ok` 连续、`gap=0`** → 修复生效，问题就是主线程丢字节。
   - **仍有 `checksum_bad`，看 `syncDelta`**：
     - `syncDelta < 0`（丢字节）且分布随机 → 剩余为 CH340C/USB/线缆物理层丢失。软件已排除，两端同步降 1.5 Mbps 或换 CH343P。
     - `syncDelta = 0`（长度正好、内容错）→ 电平边沿错字节，查线长/GND/USB 线质量，降速验证。
   - **对账**：会话结束时 `close_requested rx=<totalRx>` 应 ≈ `frame_ok 帧数 × 38415 + 坏帧残余`；与 USART1 侧 `[STRM] ok=<N>` 帧数对照，`N × 38415 - totalRx` 即链路净丢失字节数。
5. 顺带验证：拖动窗口、切模式、打开文件对话框时视频不应再崩帧（工作线程持续抽干内核缓冲）。

## 6. 后续方向(若物理层坐实丢字节)

1. 两端同步降 1.5 Mbps（STM32 `lepton_stream.h` 的 `LEPTON_STREAM_BAUD` 与上位机波特率下拉，各改一处）。
2. 换 CH343P（6 Mbps，官方驱动带更大缓冲），协议零改动。
3. STM32 侧 UART4 TX DMA 化（README_4 §9 路线），与本问题无关但可提帧率。

## 7. 实测判决（2026-07-06 20:20 会话，COM13 @ 2 Mbps）

线程化修复版上机实测 250 秒，`serial_diag.log`（1.3 MB）统计：

```text
frame_ok      = 1
checksum_bad  = 439        （零 bad_header，零 port_error）
syncDelta 分布（全部为负，无一正值）：
  -7 ×76   -6 ×63   -8 ×55   -5 ×46   -9 ×45   -10 ×37   -4 ×31 ...
  中位数 ≈ -7，主体范围 [-15, -1]，离群 -26/-40/-45/-62/-297
```

判读（对照 §5 判据）：

- **纯丢字节坐实**：syncDelta 全负 = 每帧 38415 B 平均丢 ~7 B（丢失率 ~0.018%）；
  没有任何插入或翻转类损坏（帧头样本完美：`aa 55 01 ... 00 a0 00 78 00 00 96 00`，
  payload ≈ 0x76xx = 30400 centikelvin ≈ 31 °C，数值健康）。
- **软件侧已排除**：工作线程正常抽干（rx_chunk 满速 178 KB/s 流入），port_error=0，
  parser 每次都精确重锁下一帧头（bad_header=0）。
- **丢失点定位：CH340C 芯片内部 UART→USB 方向 FIFO（~32 B）**。rx_chunk 全部是
  32 B 一片 —— 这是 CH340 的 USB bulk 端点尺寸。2 Mbps = 200 B/ms，USB bulk IN
  没有带宽保证，主机调度间隙超过 ~160 µs 芯片 FIFO 即满、丢尾巴。**PC 侧任何软件
  都无法修复芯片内溢出** —— 这正是 §5 预言的"syncDelta<0 随机分布 → 物理层"分支。
- 那 1 帧 frame_ok 是统计尾巴：帧传输 192 ms 内期望丢 ~5 次，P(一次不丢)=e^-5≈0.7%，
  与 1/440 观测一致。

### 7.1 执行既定退路：两端同步降 1.5 Mbps

当前 MCU 实际帧率 ~1.5 fps（fid 367 / 250 s，瓶颈在 VoSPI 采集不稳），
1.5 Mbps（=150 B/ms，FIFO 填充率 -25%）对实际帧率**零影响**：

- STM32：`lepton_stream.h` `LEPTON_STREAM_BAUD 2000000UL → 1500000UL`
  （UART4 内核 120 MHz / 80，0% 波特率误差）。Keil 重编译 **0 Error / 0 Warning**，
  新 HEX：`MDK-ARM/XIH6_V2/XIH6_V2.hex`（Program Size 与上版一致）。
- Qt：默认波特率下拉 2000000 → 1500000（`populateBaudRates`）。

### 7.2 顺带修复的接收端性能问题（32 B 碎片风暴）

实测暴露的次生问题：CH340 每 32 B 触发一次 readyRead（每秒 ~6000 次），旧实现
每片都跨线程发信号 + 刷新一次状态栏 QLabel —— 主线程被事件风暴轰炸。修复：

- `SerialWorker` 批量聚合：攒 ≥8 KB 或 15 ms 才向主线程发一批（事件率 6000/s → ~60/s）；
  open/'S'/'P'/close 时清空批缓冲，避免残留跨会话。
- `updateByteCounter` 状态栏刷新节流到 10 次/s（计数本身仍逐字节精确）。
- 打开串口时 `m_rxBytes/m_txBytes` 清零 —— totalRx 对账（§5）不再需要减初值
  （本次日志 idx=1 时 totalRx=4419420 就是上一会话的累计残留）。

### 7.3 复测预期（1.5 Mbps）

- `frame_ok` 连续、`gap=0`、状态栏错误帧不再增长 → CH340C 在 1.5M 有足够调度裕量，收工。
- 若 checksum_bad 仍高（syncDelta 仍负）→ 继续降 1 Mbps（120M/120 精确，~2.6 fps 上限，
  仍高于当前采集帧率）；再不行换 CH343P（1.5 KB 片内 FIFO，6 Mbps）。

## 8. 当前进度对齐（2026-07-07）

本文件仍是当前最新进度记录；`README_2.md`～`README_5.md` 只作为历史排查链路保留。

已核对当前代码与产物状态：

- 固件端已落在 1.5 Mbps：`Drivers/PER/LEPTON/lepton_stream.h` 中 `LEPTON_STREAM_BAUD = 1500000UL`，`Lepton_Stream_Init()` 会在运行时覆盖 CubeMX 生成的 UART4 115200 默认值，并保持 UART4 SWAP。
- 上位机端已落在 1.5 Mbps：`ESP_UART_Host/ESP_UART_Windows/mainwindow.cpp` 默认波特率为 `1500000`；`build_qt_fix/esp_uart.exe`、`build/esp_uart.exe` 与 `deploy/esp_uart.exe` 已重新构建并同步，时间同为 2026-07-07 01:35:21，大小同为 418936 B，MD5 同为 `FF955831E1C57FAB49F8E64EF62E3D03`。
- 上位机接收路径仍是 README_6 的线程化方案：`SerialWorker` 独立线程抽干串口，按 8 KiB 或 15 ms 聚合后再投递主线程；状态栏计数刷新节流到 10 Hz；诊断日志仍做采样与增量写。
- 上位机构建与自测已过：`cmake --build ESP_UART_Host/ESP_UART_Windows/build_qt_fix --config Release` 成功，`frameparser_selftest.exe` 与 `thermalwidget_selftest.exe` 均 exit=0。
- Keil 构建产物已重新生成：`MDK-ARM/build_keil.log` 显示 2026-07-07 01:35:52 重编译 `0 Error(s), 0 Warning(s)`，`MDK-ARM/XIH6_V2/XIH6_V2.hex` 时间为 2026-07-07 01:35:52，MD5 为 `315D8DC9F8655E420457AB7A0DB2A698`。
- 但烧录仍未闭环：已有三次 flash 日志失败，分别为 `RDDI-DAP Error`、`Internal DLL Error / Target DLL has been cancelled`；本次只重新构建，未再次成功烧录。因此当前板子上不应假设已经运行 1.5 Mbps 固件。
- 已同步修正并重新构建代码里残留的 2 Mbps 文案：固件启动打印、`lepton_stream` 注释、`SerialWorker` 默认参数/注释现在与 1.5 Mbps 当前状态一致。

下一步只剩一个判决动作：先解决 Keil/ST-LINK/J-Link 下载失败，把 1.5 Mbps HEX 真正烧进板子；再用 `deploy/esp_uart.exe` 以 1.5 Mbps 打开 CH340 串口，看 `serial_diag.log` 是否出现连续 `frame_ok` 且 `gap=0`。若仍然大量 `checksum_bad` 且 `syncDelta < 0`，按既定退路继续降到 1 Mbps，或更换 CH343P。
