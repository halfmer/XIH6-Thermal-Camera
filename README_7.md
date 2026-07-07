# README_7 - 1.5 Mbps 固件烧录与复测前置检查

日期：2026-07-07
前置：README_6 已把热成像串口链路从 2 Mbps 降到 1.5 Mbps，并同步了 STM32 固件与 Qt 上位机默认波特率。

状态更新：后续已完成固件烧录和上位机复测。当前系统已经可以稳定显示 Lepton 3.5 的 `160x120` RAW16 热成像画面，实测帧率约 `1 FPS`。本 README 前半部分保留烧录/枚举问题的历史过程，后半部分以 §8 之后的“当前成功状态”为准。

## 1. 本轮目标

按 README_6 的结论继续推进：

1. 关闭正在运行的上位机，解除 `deploy/esp_uart.exe` 文件锁。
2. 烧录 `MDK-ARM/XIH6_V2/XIH6_V2.hex`（1.5 Mbps 版）。
3. 启动新上位机，选择 1500000 bps，对 CH340 串口复测热成像流。

## 2. 上位机进程与 EXE 状态

检查结果：

```text
Get-Process esp_uart -> 未发现 esp_uart.exe 进程
deploy/esp_uart.exe 独占打开测试 -> UNLOCKED
```

因此 `deploy/esp_uart.exe` 当前未被上位机锁定，可以覆盖。

当前三个上位机产物已同步：

```text
build_qt_fix/esp_uart.exe  MD5 FF955831E1C57FAB49F8E64EF62E3D03
build/esp_uart.exe         MD5 FF955831E1C57FAB49F8E64EF62E3D03
deploy/esp_uart.exe        MD5 FF955831E1C57FAB49F8E64EF62E3D03
```

## 3. 固件产物状态

README_6 已完成重新构建，本轮再次确认使用该产物：

```text
MDK-ARM/XIH6_V2/XIH6_V2.hex
time: 2026-07-07 01:35:52
MD5 : 315D8DC9F8655E420457AB7A0DB2A698
```

对应源码状态：

- `Drivers/PER/LEPTON/lepton_stream.h`: `LEPTON_STREAM_BAUD = 1500000UL`
- `Core/Src/main.c`: 启动打印已改为 `UART4/CH340 1.5Mbps binary only`
- `ESP_UART_Host/ESP_UART_Windows/mainwindow.cpp`: 默认波特率 `1500000`

## 4. Keil 烧录尝试

使用工程：

```text
project: MDK-ARM/XIH6_V2.uvprojx
target : XIH6_V2
tool   : E:/keil_v5/UV4/UV4.exe
```

### 4.1 沙箱内尝试

命令进入下载流程后 120 秒未返回，日志只写到：

```text
Load "XIH6_V2\\XIH6_V2.axf"
```

随后确认没有 `UV4.exe` 残留进程。

### 4.2 沙箱外受控尝试

重新以沙箱外权限执行 `UV4 -f`，90 秒超时保护，实际 13 秒返回：

```text
UV4_EXIT=2
Load "XIH6_V2\\XIH6_V2.axf"
Internal DLL Error
Error: Flash Download failed  -  Target DLL has been cancelled
Flash Load finished at 02:14:00
```

结论：本轮烧录没有成功，当前板子上不能假设已经运行 1.5 Mbps 固件。

## 5. 调试器与串口枚举

沙箱外查询到的相关设备：

```text
USB Composite Device: USB\\VID_C251&PID_F001\\CZ_2023_6688
HID interface        : HID\\VID_C251&PID_F001&MI_02\\...
Virtual COM          : COM5, USB 串行设备, USB\\VID_C251&PID_F001&MI_00\\...
```

没有枚举到：

- ST-LINK
- J-Link 硬件
- CH340/CH341 (`VID_1A86`)
- DAPLink/mbed 可拖拽 HEX 的 U 盘盘符

本机可用下载工具检查：

```text
jlink.exe             -> 存在于 D:/bin/jlink.exe
openocd.exe           -> 未找到
pyocd                 -> 未找到
probe-rs              -> 未找到
STM32_Programmer_CLI  -> 未找到
```

当前设备更像 CMSIS-DAP/DAPLink 类探针，但没有暴露 mass-storage 烧录盘。

## 6. Keil 配置观察

工程文件里有一个不一致点：

```text
MDK-ARM/XIH6_V2.uvoptx:
  pMon = BIN\\CMSIS_AGDI.dll
  TargetDriverDllRegistry 有 CMSIS_AGDI 配置

MDK-ARM/XIH6_V2.uvprojx:
  DriverSelection = 4101
  Flash2 = BIN\\UL2V8M.DLL
```

也就是说，选项文件看起来偏 CMSIS-DAP，但工程 Flash2 仍指向 ULINK V8-M DLL。Keil 命令行下载失败点在 Target DLL 层，后续应优先检查 Keil Debug/Utilities 配置是否正确选择当前 `VID_C251&PID_F001` 对应的 CMSIS-DAP/ULINKplus 设备。

本轮未直接改 `.uvprojx/.uvoptx` 下载器配置，避免把工程调试器设置改坏。

## 7. 串口运行状态检查

打开 COM5@115200，监听 6 秒：

```text
NO_SERIAL_OUTPUT
```

说明当前 COM5 没有输出 USART1 调试日志；可能原因包括：

- 目标板没有运行/没上电/被下载器保持复位；
- COM5 是调试器虚拟串口，但没有接到 STM32 USART1；
- 当前板子未接入 README_6 中用于热成像流的 CH340 串口；
- 固件仍是旧版本或未启动到打印阶段。

## 8. 当前成功状态

前面 §4～§7 记录的是烧录/枚举阶段的历史问题；该问题后续已经越过。当前进度以本节为准：

- 固件已运行到 1.5 Mbps 热成像链路版本。
- CH340/UART4 热成像串口已经可用，上位机能收到并解析完整帧。
- Qt 上位机已能显示 Lepton 3.5 `160x120` 热成像画面。
- 当前帧率约 `1 FPS`，画面链路已经从“协议错误/无法出图”推进到“可连续显示低帧率视频流”。

当前链路：

```text
Lepton 3.5 VoSPI
  -> STM32H743 SPI4 采集 segment
  -> lepton_assembly_frame staging
  -> lepton_raw_frame 发布帧
  -> UART4/CH340 @ 1500000 bps
  -> Qt SerialWorker 线程接收
  -> FrameParser 校验 AA55 RAW16 帧
  -> ThermalWidget 显示 160x120 热图
```

关键参数：

```text
分辨率       : 160 x 120
像素格式     : RAW16 / u16 BE / TLinear centikelvin
单帧 payload : 38400 B
单帧总长度   : 38415 B
串口波特率   : 1500000 bps
串口发送耗时 : 38415 * 10 / 1500000 ~= 256 ms
实测画面帧率 : 约 1 FPS
```

当前 1 FPS 的瓶颈不再是 Qt parser 协议，也不是单帧串口带宽本身；主要来自 Lepton VoSPI 采集端仍需要靠 segment shelf/resync 攒齐帧，采集不是每轮都能稳定一次成功。按当前代码注释，串口 1.5 Mbps 理论可承载约 `3.9 FPS` 的 RAW16 帧，现阶段实际帧率由 VoSPI 采集成功率决定。

## 9. 当前代码基线

STM32：

- `Drivers/PER/LEPTON/lepton_stream.h`: `LEPTON_STREAM_BAUD = 1500000UL`。
- `Drivers/PER/LEPTON/lepton.c`: 使用 `lepton_assembly_frame[120][160]` 做完整帧 staging。
- `Lepton_VoSPI_CommitSegment()` 只写 staging frame，不直接改发布帧。
- `vospi_cached_mask` 攒满后不清零；后续 segment 会刷新 staging shelf。
- 只有 `seg == 4 && mask == 0x0F` 时才 `memcpy()` 到 `lepton_raw_frame` 发布。
- `Lepton_Stream_SendFrame()` 从 `lepton_raw_frame` 打包 AA55 RAW16 帧并经 UART4 发出。

Qt 上位机：

- 默认波特率为 `1500000`。
- `SerialWorker` 独立线程读取串口，避免 GUI 主线程阻塞导致丢字节。
- 串口数据按约 `8 KiB` 或 `15 ms` 聚合后投递给主线程，降低 CH340 32B 碎片风暴带来的事件压力。
- `FrameParser` 按固定 `38415 B` 帧长和 checksum 校验解析。
- `ThermalWidget` 已能显示 `160x120` 热图，并显示当前 FPS。

## 10. 当前判决和下一步

当前阶段判决：

- `160x120 @ ~1 FPS` 热成像视频流已经跑通。
- 1.5 Mbps 方案比 2 Mbps 更符合 CH340C 实测能力，当前不建议回退到 2 Mbps。
- `mask` 发布语义是当前成功基线，后续不要再改成“成功后清零”或“强制 1->2->3->4 连续顺序”。

下一步建议只围绕提帧率和稳定性做小步验证：

1. 保留当前版本作为稳定基线，先保存一份 USART1 日志和 `serial_diag.log`。
2. 统计 1 FPS 状态下 `[LEP] VoSPI okdiag` 的 `mask/seg/badseg/desync`，确认瓶颈主要是 VoSPI segment 失败率。
3. 若要提帧率，优先优化 VoSPI 采集时序和同步策略，而不是先改上位机协议。
4. UART 侧暂时维持 `1500000 bps`；只有确认 `serial_diag.log` 长时间 `frame_ok gap=0` 后，再考虑是否试探更高波特率或 DMA 发送。
