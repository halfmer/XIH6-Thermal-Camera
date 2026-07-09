# XIH6 — 自制 FLIR Lepton 3.5 热成像系统

[![MCU](https://img.shields.io/badge/MCU-STM32H743XIH6-blue)](https://www.st.com/en/microcontrollers-microprocessors/stm32h743xi.html)
[![Sensor](https://img.shields.io/badge/Sensor-FLIR%20Lepton%203.5-red)](https://www.flir.com/products/lepton/)
[![Framework](https://img.shields.io/badge/Qt-6.8-green)](https://www.qt.io/)
[![Android](https://img.shields.io/badge/Android-Kotlin-brightgreen)](https://developer.android.com/)
[![ESP32](https://img.shields.io/badge/Bridge-ESP32--S3-lightgrey)](https://www.espressif.com/)

一份两周从零完成的自制红外热成像系统：从一块 STM32H743 核心板起手，打通 FLIR Lepton 3.5 VoSPI 采集 → ESP32 Wi-Fi 桥 → Qt/Android 双端显示的完整链路。帧完整性优先于一切——宁要 2FPS 完整帧，也不妥协一帧的撕裂。

本工程为 **2026 年全国大学生嵌入式芯片与系统设计竞赛** 参赛作品（终因时间不足遗憾未完成全部功能验证）。

## 实物展示

| 整机外观 | PCB + 核心板 | 串口 Shell 诊断 |
|:---:|:---:|:---:|
| ![整机](演示图片/001_all.jpg) | ![PCB](演示图片/004_pcb_shell.jpg) | ![Shell](演示图片/003_shell.jpg) |

| Qt 桌面端 | Android 手机端 |
|:---:|:---:|
| ![Qt上位机](演示图片/005_qt.jpg) | ![Android](演示图片/006_app.jpg) |

## 系统架构

```text
                          ┌─ SHT40 温湿度
                          ├─ MQ-2  烟雾/LPG
  FLIR LEPTON 3.5         ├─ MQ-135 空气质量
       │                   ├─ OLED 0.96" (SW I2C)
  VoSPI (SPI4, 24MHz)     ├─ SD 卡 (SDMMC2+DMA)
       │                   ├─ 火灾 LED (PJ15, AO3400 NMOS)
       ▼                   └─ 蜂鸣器 (PG9, NPN 低边)
  ┌──────────────┐              │
  │ STM32H743XIH6 │              │
  │   裸机超级循环  │              │
  │ + 协作时间轮   │              │
  └──────┬───────┘              │
         │                      │
    ┌────┴────┐                 │
    │         │                 │
  UART4     SPI5 从机           │
  (CH340C)  (DRDY握手)          │
  1.5Mbps   10MHz               │
    │         │                 │
    ▼         ▼                 │
  Qt 串口   ESP32-S3            │
  上位机    ──TCP──► Qt WiFi    │
                       Android  │
```

## 核心设计原则

| 优先级 | 原则 | 实现 |
|:---|:---|:---|
| 1 | 帧完整性 > 连续性 > 延时 > 帧率 | Staging 完整帧缓冲，未完成帧永不对外发布 |
| 2 | 零画面撕裂 | 双缓冲 + checksum 门禁 + Qt 段缓存重组 |
| 3 | 完整帧优先 | `Lepton_Capture_Frame()` 返回 1 才发帧、才渲染 |
| 4 | 每帧可追溯 | 帧头含 ID + 时间戳 + 载荷长度 + Checksum |

详见 [`AGENTS.md`](AGENTS.md) 中的协议规格和开发规范。

## 技术亮点

### STM32 固件 (`Core/`, `Drivers/PER/`)
- **VoSPI 帧采集**：DMA 驱动的 SPI4 从机，segment 同步、丢包检测、错误恢复
- **协作时间轮**：视频路径无条件优先（不进轮），背景任务按时隙分片——不阻塞、不饿死、不撕裂
- **MQ 传感器 ppm 量化**：标准 `Rs/R0` 幂律模型，三态状态机（预热→标定→运行），迟滞报警
- **LEPTON 过温报警**：全帧扫描 ≥100°C 热点 → RGB 灯闪烁(2.5Hz) + 蜂鸣器断续鸣叫
- **运行时防 CubeMX 回退**：SPI4/SPI5/UART4/ADC 采样时间全部运行时重配，Cube 重新生成不受影响
- **AA55 二进制帧协议**：Qt/Android 统一，温度用 TLinear centikelvin（0.01 K/LSB）

### Qt 桌面端 (`ESP_UART_Host/`)
- 串口 + WiFi 双模式
- Checksum 校验 + 帧段缓存重组消撕裂
- 百分位色图（自动范围拉伸）
- 线程安全：网络线程永不自碰 UI

### Android 客户端 (`Android_studio_project/`)
- Kotlin + Material Design 3
- TCP Server 模式（ESP32 主动拨入）
- 与 Qt 端协议完全对齐——一帧两解

### ESP32-S3 桥
- SPI 主机拉流 + TCP 透传
- DRDY 握手（500ms 超时自愈）
- STM32 侧运行时链路热切换（串口 ↔ TCP）

## 工程结构

```text
XIH6_V3/
├── Core/                     STM32 CubeMX 生成的应用层
│   ├── Inc/
│   └── Src/main.c            主循环 + 时间轮调度
├── Drivers/PER/              外设驱动层
│   ├── LEPTON/               VoSPI + CCI 驱动
│   ├── FIRE/                 MQ-2/135 ppm + LEPTON 过温
│   ├── SHT40/                温湿度传感器
│   ├── OLED/                 SW I2C OLED 驱动
│   └── SD_Card/              SDMMC2 + DMA + FatFs
├── MDK-ARM/                  Keil MDK 工程
│   └── XIH6_V2.uvprojx
├── ESP_UART_Host/            Qt6 桌面端源码
│   ├── ESP_UART/             Linux/跨平台版
│   └── ESP_UART_Windows/     Windows 版（串口 + TCP）
├── Android_studio_project/   Android 客户端
├── PCB/                      立创 EDA 工程
├── 演示图片/                  实物照片
├── README_2.md ~ README_15.md  开发日志（13 篇全程战报）
├── README.md                 本文件
├── XIH6_V2.ioc               STM32CubeMX 工程
└── .gitignore
```

## 快速上手

### 固件编译
1. 用 STM32CubeMX 打开 `XIH6_V2.ioc`，生成代码（会覆盖 `Core/`）
2. 用 Keil MDK 打开 `MDK-ARM/XIH6_V2.uvprojx`
3. 编译 Target `XIH6_V2`（ARM Compiler 6），0 Error 0 Warning
4. 产物：`MDK-ARM/XIH6_V2/XIH6_V2.hex`

### Qt 桌面端
```bash
cd ESP_UART_Host/ESP_UART_Windows
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=<Qt6安装路径>
cmake --build build
```

### Android 客户端
用 Android Studio 打开 `Android_studio_project/`，Sync Gradle → Build APK。

## 技术遗留与已知边界

- **CCI (I2C4) 未通**（硬件层面搁置）——LEPTON 全程运行在出厂默认配置，恰好 TLinear 默认开启使测温可行；FFC/AGC/增益不可控
- **帧率 ~2FPS**（目标 8–12FPS），瓶颈在 VoSPI 采集节奏与 CH340C FIFO 容量
- **Android 端到端联调未完成**（协议层已对齐）
- **MQ 阈值待现场标定**（当前为数据手册参考值）

详细技术路线、踩坑记录与判例见 [`README_15.md`](README_15.md)（终章回顾）及 [`README_2.md`](README_2.md) ~ [`README_14.md`](README_14.md)。

## 许可

MIT License © 2026 halfmer

---

*两周凌晨，从零开始。没有一帧是假的。*
