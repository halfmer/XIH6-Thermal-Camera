# README_4 — Lepton 3.5 热成像串口流传输（CH340C @ 2 Mbps）

日期：2026-07-05
前置：README_2（Lepton GPIO 根因修复 + CCI/VoSPI/TLinear 驱动）、README_3。
本篇记录：原理图核对结论、STM32 → PC 串口视频流方案、固件与 Qt 上位机改动、上板测试步骤。

---

## 1. 背景与决策

VoSPI 采集链路打通后，需要把 160×120@16bit 的 TLinear 帧送到 PC 显示。
本板 **未引出 STM32 的 USB**，PC 物理链路只有 CH340C。经带宽核算，
CH340C 拉到上限 2 Mbps 可以承载约 3~5 fps 的无损 RAW16 流，方案成立，
不需要 SDRAM、不需要 USB，协议直接对齐现有 Qt 上位机（ESP_UART_Windows）。

## 2. 原理图核对结论（ESP32连接STM32和CH340C连接.pdf，XIH6 底板 3 V1.0）

| 连接 | 结论 |
|---|---|
| CH340C.TXD (pin2) → **PA0**，CH340C.RXD (pin3) ← **PA1** | PC 链路走 **UART4**，不是固件原来打日志的 USART1 |
| USART1 (PA9/PA10) | 在这块底板上**未引出**——旧固件日志实际发进了黑洞 |
| PA0 的 AF8 复用 = UART4_**TX**，但 CH340C.TXD（输出）接到 PA0 | 方向对调，**必须开 UART4 的 CR2.SWAP**（H7 硬件支持），SWAP 后 PA0=RX、PA1=TX |
| CH340C 的 DTR#/RTS#/CTS# | 全部悬空，不存在"打开串口误触复位"问题 |
| ESP32-S3 的 TXD0/RXD0 ↔ ST_TX/ST_RX ↔ **UART5 (PB13/PB12)** | 后续 STM32↔ESP32 链路板上预留的是 UART，不是 SPI |
| PA0/PA1 与 ADC | 无冲突：adc.c 未配置 PA0/PA1 GPIO（H743 的 ADC 走独立 `_C` pad） |

## 3. 链路预算

- 帧长：13 B 帧头 + 38400 B 像素 + 2 B 校验 = **38415 B**
- UART4 内核时钟 = D2PCLK1 = 120 MHz，2 Mbps = 120M/60，**波特率误差 0%**
- 2 Mbps、8N1（10 bit/字节）→ 单帧发送 **192 ms**
- 当前实现（阻塞发送、采集与发送串行）：**约 3.3 fps**；Lepton 独特帧率 8.7 fps，
  发送期间错过的 VoSPI 帧由下次 `Lepton_Capture_Frame()` 的重同步逻辑自然丢弃
- CH340C 为内置时钟版本，2 Mbps 官方支持；若实测坏帧率偏高，两端同步降 1.5 Mbps（≈2.8 fps）

## 4. 传输协议（以 Qt 端 FrameParser 为唯一标准）

协议实现对齐 `ESP_UART_Host/ESP_UART_Windows/frameparser.h`，多字节字段一律**大端**：

```
[AA 55]         同步字
[01]            type：raw 16-bit thermal
[u16 BE]        frame_id（滚动计数）
[u16 BE]        width  = 160 (0x00A0)
[u16 BE]        height = 120 (0x0078)
[u32 BE]        pixel_len = 38400 (0x00009600)
[38400 B]       像素，每像素 u16 BE，TLinear centikelvin（0.01 K/LSB）
[u16 BE]        checksum：从 0xAA 起到 payload 末尾逐字节 8-bit 累加进 uint16
```

要点：
- `lepton_raw_frame` 中已是本机小端数值（VoSPI 字节序在 `Lepton_VoSPI_CommitSegment` 组装时转换），
  发送时逐像素高字节在前写入——**不要再做 __REV16 之类的整体翻转**
- 温度换算在 Qt 端：`raw * 0.01 - 273.15`（colormap.cpp），与固件 TLinear 配置天然一致
- 早前讨论过的 `5A A5 + CRC16` 私有帧结构**作废**，两端统一到上表

### 命令握手（PC → MCU，单字节）

| 命令 | 作用 |
|---|---|
| `'S'` (0x53) | 进入流模式：日志静音，主循环连续 采集→发帧 |
| `'P'` (0x50) | 退出流模式：回到日志/调试模式（OLED、SHT40、SD 检测恢复） |

MCU 上电默认**日志模式**（bring-up 日志不受影响）；2 Mbps 下线路噪声导致的
ORE/FE 由 `HAL_UART_ErrorCallback` 清标志并重挂接收，保证 'P' 永远有效。

## 5. 固件改动（MDK 工程 XIH6_V2）

新增：
- `Drivers/PER/LEPTON/lepton_stream.h` — 协议常量与 API
- `Drivers/PER/LEPTON/lepton_stream.c` — UART4 运行时重配（2M + SWAP + NVIC + 1 字节命令接收）、
  AA55 构帧、阻塞发送、'S'/'P' 回调、RX 错误自恢复

修改：
- `Core/Src/main.c` — 全部 PC 输出从 huart1 切到 **huart4**；`USER CODE 2` 开头调
  `Lepton_Stream_Init(&huart4)`（先于首条日志，banner 即 2M）；主循环顶部加流模式分支
  （`continue` 跳过 LED_TURN(250)/SHT40/OLED/SD 检测，否则每圈 500ms 阻塞把帧率锁死在 1.5 fps）；
  `SD_UART_Print` 流模式静音，文本永不落入帧间
- `Core/Src/stm32h7xx_it.c` — 补 `UART4_IRQHandler`（CubeMX 未曾生成）+ `extern huart4`
- `MDK-ARM/XIH6_V2.uvprojx` — LEPTON 组加入 lepton_stream.c/.h

未动：
- `Core/Src/usart.c` — MX_UART4_Init 保持 115200 原样，由 `Lepton_Stream_Init` 运行时覆盖
  （同 `Lepton_SPI_Config` 覆盖 SPI4 的先例，CubeMX 重新生成不会回退 2M/SWAP）

## 6. Qt 上位机改动（仅 ESP_UART_Windows，ESP_UART 目录未动）

`mainwindow.cpp` 四处：
1. `populateBaudRates()` 默认值 921600 → **2000000**（下拉里的 3000000 CH340C 不支持，勿选）
2. `openSerialPort()` 成功后若处于"热成像(串口)"模式自动发 `'S'`
3. `closeSerialPort()` 关闭前发 `'P'`（并 `waitForBytesWritten`），MCU 回日志模式
4. `switchMode()` 串口开启时随页面切换发 `'S'`/`'P'`：热成像页=流，其余页=日志

## 7. 上板测试步骤

1. Keil 编译（应零错误）、烧录
2. 串口助手以 **2,000,000 bps** 打开 CH340 口（⚠ 从此 115200 只会看到乱码）：
   应看到 `It's mygo!!!!!` → `[RST] ...` → `[STREAM] CH340 link 2Mbps: 'S' = start...`
   → `[SD]`/`[LEP]` 日志，每 ~8 s 一条 `[LEP] OK c_raw=... c=...C`
3. 手发 `S`：窗口开始刷二进制乱码（即帧流，正常）；手发 `P`：回到可读日志
4. 关掉串口助手，开 Qt 上位机：波特率默认已是 2000000，切"热成像(串口)"模式，
   打开串口即自动开流出图；点击画面测温、状态栏帧率应在 **3 fps 上下**
5. 验收指标：连续跑 10 分钟，Qt 状态栏"错误帧"计数不增长（偶发 1~2 帧可接受，
   持续增长则两端同步降 1.5M 复测）

## 8. 故障排查速查

| 现象 | 首查 |
|---|---|
| 2M 下全是乱码、无一条完整日志 | 波特率没设对；或 SWAP 未生效（量 PA1 应有 TX 输出） |
| 日志正常但发 'S' 无反应 | UART4 RX 方向问题（SWAP/接线）；`UART4_IRQHandler` 是否编译进去 |
| Qt 报"Bad header"偶发 | 正常——切流瞬间的残留文本被扫描跳过，持续出现才需要查 |
| 错误帧持续增长 | CH340C 2M 裕量不足：两端降 1.5M；检查 USB 线质量 |
| 流模式 OLED 不刷新、插 SD 无反应 | 设计如此，发 'P' 恢复 |

## 9. 后续路线（未实施）

1. **UART 发送 DMA 化 + 乒乓缓冲**：发送与采集重叠，帧率 3.3 → 5.2 fps（受串口带宽顶死）。
   注意 UART4 尚无 DMA 通道（CubeMX 需分配 stream），且 DMA 缓冲不能落 DTCM
2. **ESP32-S3 无线链路**：板上走 UART5 (PB12/PB13) ↔ ESP32 UART0，透传同一 AA55 协议，
   ESP32 收满一帧经 Wi-Fi TCP 发给 Qt（上位机"WiFi热成像"模式已就绪，监听 8888）。
   注意 ESP32 UART0 默认是其日志口，需改 console 或换 UART1
3. **换 CH343P**（6 Mbps）可跑满 Lepton 8.7 fps 全帧率，协议零改动
