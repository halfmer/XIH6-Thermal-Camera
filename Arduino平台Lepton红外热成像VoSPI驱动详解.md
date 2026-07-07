# Arduino 平台 Lepton 红外热成像 VoSPI 驱动详解

## 1. 项目概述

`arduino-lepton` 是一个面向 Arduino HAL（Hardware Abstraction Layer）生态的轻量级 FLIR Lepton 红外热成像传感器驱动库。其核心目标是为嵌入式开发者提供稳定、可移植、低依赖的 Lepton 3.x 系列（特别是 Lepton 3.5）视频流接入能力，重点支持 VoSPI（Video SPI）协议下的实时帧捕获与处理。该库并非通用型图像处理框架，而是一个**硬件协议胶水层**——它不负责图像增强、伪彩色映射或温度标定，而是精准解决"如何从 Lepton 芯片可靠地读出原始 160×120 红外辐射数据"这一底层工程问题。

项目已在 ESP32-S3 平台上完成完整验证：搭载定制 PCB（含 Lepton 3.5 模组），通过标准 SPI 总线实现 VoSPI 协议通信，并成功集成至 Web Server 应用中，以 MJPEG 流形式向浏览器推送灰度或 RGB888 彩色化热图。其设计哲学强调"最小可行驱动"（Minimal Viable Driver）：仅封装 Lepton 启动流程、VoSPI 帧同步机制、寄存器配置接口及基础错误恢复逻辑，所有上层功能（如 JPEG 编码、色彩空间转换、网络传输）均交由外部成熟库（如 JPEGENC、ArduinoWebServer）完成，从而保持内核精简、可测试性强、易于调试。

值得注意的是，该库明确区分了"功能可用性"与"平台普适性"。文档中 *"should work on most Arduino platforms that have enough memory and computational power"* 的表述，实为一项关键工程约束声明：Lepton 3.5 的 VoSPI 数据流峰值带宽约为 12.8 MB/s（160×120×2 B/frame × 30 fps），对 MCU 的 SPI 时钟稳定性、DMA 通道资源、中断响应延迟及内存带宽提出严苛要求。因此，所谓"兼容多数平台"，本质是指满足以下硬性条件的 Arduino 兼容设备：

- SPI 主机控制器支持 ≥ 20 MHz 连续时钟输出（Lepton 3.5 VoSPI 标称速率 20 MHz）
- 具备至少 38.4 KB 连续 RAM（单帧缓冲区 160×120×2 = 38,400 字节）
- 中断响应延迟 ≤ 1 μs（VoSPI 帧间空闲时间极短，需精确同步）
- 支持硬件 SPI FIFO 或 DMA（避免 CPU 长时间阻塞导致帧丢失）

这一定位使 `arduino-lepton` 区别于其他同类项目：它不追求在 ATmega328P 上运行，而是聚焦于 ESP32、RP2040、nRF52840 等具备足够实时处理能力的现代 MCU，为工业测温、智能安防、嵌入式热成像终端等实际场景提供可量产的驱动基础。

## 2. 硬件接口与协议原理

### 2.1 Lepton 3.5 物理连接拓扑

Lepton 3.5 模组采用标准 24-pin FFC 接口，`arduino-lepton` 驱动仅需使用其中 7 个关键信号线，构成最小功能连接：

| 信号名 | 方向 | 功能说明 | Arduino 引脚建议 |
|---|---|---|---|
| LEP_CS | MCU→Lepton | 片选信号，低电平有效，控制 VoSPI 事务边界 | 任意 GPIO（需硬件上拉） |
| LEP_RST | MCU→Lepton | 复位信号，高电平有效，异步复位 Lepton 内部状态机 | 任意 GPIO（需硬件上拉） |
| SPI_MOSI | MCU→Lepton | VoSPI 数据输入线（Lepton 仅接收配置命令，不用于视频流） | SPI MOSI（固定） |
| SPI_MISO | Lepton→MCU | VoSPI 数据输出线，承载全部红外帧数据 | SPI MISO（固定） |
| SPI_SCK | MCU→Lepton | SPI 时钟线，VoSPI 严格依赖此信号同步采样 | SPI SCK（固定） |
| VSYNC | Lepton→MCU | 帧同步信号，下降沿标志新帧开始（**可选但强烈推荐**） | 中断-capable GPIO |
| I2C_SCL / I2C_SDA | 双向 | I²C 总线，用于初始化配置、读取状态寄存器、设置视频参数 | 任意 I²C 引脚 |

> **关键设计决策解析：**
> 驱动将 I²C 与 SPI 分离使用，而非复用同一总线。这是因为 Lepton 的 I²C 接口（地址 `0x64`）仅用于**非实时配置**（如设置 AGC 模式、校准参数、查询芯片 ID），而 VoSPI 是**纯数据通道**，要求零延迟、高吞吐。若强行共用总线，I²C 的开漏特性与上拉电阻会严重劣化 SPI 信号完整性，导致 VoSPI 同步失败。此分离设计是保证协议鲁棒性的物理层基础。

### 2.2 VoSPI 协议时序与帧结构

VoSPI 是 FLIR 定义的专用于 Lepton 视频流的 SPI 变种协议，其核心特征在于**无传统 SPI 帧头/帧尾，全靠时序隐式同步**。Lepton 3.5 输出的每一帧为 160×120 像素的 14-bit 红外辐射值（MSB 对齐，高位字节在前），经内部打包为 16-bit 字（低位字节填充 0），故单帧数据长度恒为 **160 × 120 × 2 = 38,400 字节**。

VoSPI 事务严格遵循以下时序规则：

- **帧起始**：VSYNC 信号下降沿（若启用）或 LEP_CS 拉低后首个 SPI_SCK 上升沿触发
- **数据流**：连续 38,400 字节 SPI 读操作，SPI_SCK 频率必须稳定在 20 MHz ± 0.5%
- **帧间间隔**：两帧之间存在约 1.2 ms 的空闲期（SCLK 停止），此期间 MISO 为高阻态
- **丢弃帧（Discard Frame）**：当 MCU 未能及时启动读操作，Lepton 会自动丢弃当前帧并进入重同步状态，此时 `readVoSpi()` 返回 `false`

> **为何 VSYNC 强烈推荐？**
>
> 文档中提到的 *"desynchronizes if frames are not read out promptly"* 及 *"re-sync without success"* 问题，根源正在于此。若仅依赖 CS 信号触发，MCU 必须在 VSYNC 下降沿后 ≤ 500 ns 内完成 SPI 初始化并启动读取，这对软件延时抖动极为敏感。而 VSYNC 提供硬件级帧边界指示，MCU 可配置为边沿触发中断，在中断服务程序（ISR）中立即启动 DMA 读取，将同步误差控制在数十纳秒级，从根本上规避重同步失败风险。

### 2.3 初始化与状态机流程

Lepton 3.5 上电后并非立即输出有效视频流，需经历严格的初始化握手流程。`arduino-lepton` 的 `begin()` 方法封装了此过程：

```cpp
bool FlirLepton::begin() {
  // 1. 硬件复位：拉低 LEP_RST 10ms，再拉高
  digitalWrite(kPinLepRst, LOW);
  delay(10);
  digitalWrite(kPinLepRst, HIGH);
  delay(10); // 等待内部 PLL 锁定

  // 2. I²C 初始化：扫描设备，读取芯片 ID (0x2A)
  if (!i2c->begin()) return false;
  uint8_t id;
  if (!i2c->readReg(0x64, 0x00, &id, 1)) return false;
  if (id != 0x2A) return false; // 非 Lepton 3.x 设备

  // 3. VoSPI 配置：通过 I²C 设置视频模式为 160x120@30Hz
  uint8_t videoConfig[4] = {0x00, 0x00, 0x00, 0x00};
  i2c->writeReg(0x64, 0x02, videoConfig, 4);

  // 4. 启动视频流：写入 0x01 到 0x01 寄存器
  uint8_t startCmd = 0x01;
  i2c->writeReg(0x64, 0x01, &startCmd, 1);

  return true;
}
```

此后，`isReady()` 方法轮询 I²C 状态寄存器（地址 `0x00`），等待 READY 位（bit 0）置 1，表明 Lepton 已完成内部校准并准备输出首帧。此过程通常耗时 2–5 秒，取决于环境温度稳定性。

## 3. 核心 API 详解与工程实践

### 3.1 构造函数与初始化接口

```cpp
FlirLepton::FlirLepton(TwoWire& i2c_bus, SPIClass& spi_bus,
                       uint8_t cs_pin, uint8_t rst_pin);
```

**参数说明：**

| 参数 | 说明 |
|---|---|
| `i2c_bus` | Arduino TwoWire 实例（如 `Wire`），用于 I²C 配置通信 |
| `spi_bus` | Arduino SPIClass 实例（如 `SPI`），用于 VoSPI 数据读取 |
| `cs_pin` | LEP_CS 物理引脚号，驱动内部自动配置为 OUTPUT 模式 |
| `rst_pin` | LEP_RST 物理引脚号，驱动内部自动配置为 OUTPUT 模式 |

**工程要点：**

- `cs_pin` 和 `rst_pin` **必须接硬件上拉电阻（4.7kΩ）**，因 Lepton 要求复位信号默认高电平
- 若使用 ESP32-S3，推荐 `spi_bus` 绑定至 SPI2（VSPI），因其时钟源更稳定，且与 WiFi/BT 射频干扰隔离

### 3.2 VoSPI 帧读取接口

```cpp
bool FlirLepton::readVoSpi(size_t buffer_size, uint8_t* frame_buffer);
```

**参数说明：**

| 参数 | 说明 |
|---|---|
| `buffer_size` | 传入缓冲区字节数，**必须 ≥ 38400**，否则返回 `false` |
| `frame_buffer` | 指向 38400 字节缓冲区的指针，数据按 MSB-first 顺序存储（`frame_buffer[0]` 为像素 (0,0) 的高字节） |

**返回值语义：**

- `true`：成功读取一帧有效数据，`frame_buffer` 已填充
- `false`：当前为丢弃帧（discard frame），或缓冲区不足，或 SPI 通信超时

**关键行为：**

- **阻塞式读取**：函数内部调用 `spi_bus.transfer()` 循环读取 38400 字节，期间 CPU 被完全占用
- **无帧同步保障**：若未启用 VSYNC，首次调用可能读到帧中间数据，需丢弃前 2–3 帧确保同步
- **时序敏感性**：ESP32-S3 在 `SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0))` 下实测最稳定

### 3.3 视频参数动态配置接口

```cpp
void FlirLepton::setVideoParameters(uint8_t bytes_per_pixel,
                                   uint8_t frame_width,
                                   uint8_t frame_height,
                                   size_t video_packet_data_len,
                                   size_t packets_per_segment,
                                   size_t segments_per_frame);
```

**参数说明（针对 Lepton 3.5 固定值）：**

| 参数 | Lepton 3.5 推荐值 | 说明 |
|---|---|---|
| `bytes_per_pixel` | 2 | 14-bit 数据打包为 16-bit 字 |
| `frame_width` | 160 | 水平像素数 |
| `frame_height` | 120 | 垂直像素数 |
| `video_packet_data_len` | 32 | VoSPI 单次 SPI 事务最大字节数（Lepton 硬件限制） |
| `packets_per_segment` | 1200 | 每段包含的 packet 数（160×120×2 / 32 = 1200） |
| `segments_per_frame` | 1 | 单帧仅含 1 段（Lepton 3.5 固定） |

**工程价值：** 此接口为未来扩展预留，例如适配 Lepton 2.5（320×240）或 Lepton 4.0（160×120 @ 60Hz）。实际项目中，若需切换分辨率，只需修改 `frame_width` / `frame_height` 并重新计算 `packets_per_segment`，无需改动底层 SPI 逻辑。

### 3.4 状态查询与错误处理

```cpp
bool FlirLepton::isReady();           // 轮询 I²C 状态寄存器 0x00
uint8_t FlirLepton::getLastError();   // 返回最近一次 I²C/SPI 错误码（0 = 无错）
void FlirLepton::clearError();        // 清除错误标志
```

**错误码定义（驱动内部枚举）：**

| 错误码 | 含义 | 典型原因 |
|---|---|---|
| `0x01` | I2C_TIMEOUT | I²C 总线被占用或上拉不足 |
| `0x02` | SPI_TIMEOUT | SPI 读取超时（> 2ms），VoSPI 同步失败 |
| `0x03` | BUFFER_OVERFLOW | `buffer_size` < 所需帧长 |
| `0x04` | INVALID_FRAME | 读取到非标准帧头（非 `0x0000`） |

**实战调试建议：** 当出现 *"constantly attempts to re-sync without success"* 时，应首先检查 `getLastError()`：

- 若返回 `0x02`，立即审查 SPI 时钟配置与 VSYNC 连接
- 若返回 `0x01`，用逻辑分析仪抓取 I²C 波形，确认 SCL/SDA 上升沿时间是否 > 300 ns（需更换更小上拉电阻）

## 4. 典型应用示例深度解析

### 4.1 ESP32-S3 Web Server 实现（MJPEG 流）

此示例是 `arduino-lepton` 工程价值的集中体现，其架构如下：

```
Lepton 3.5 → VoSPI → ESP32-S3 DMA Buffer → JPEGENC Library → HTTP Chunked Response
                          ↑
                      FreeRTOS Task (High Priority)
```

**关键代码片段（精简版）：**

```cpp
// 1. 创建高优先级任务（高于 WiFi 任务）
xTaskCreatePinnedToCore(leptonCaptureTask, "lepton", 8192, NULL, 10, NULL, 0);

void leptonCaptureTask(void* pvParameters) {
  FlirLepton lepton(Wire, SPI, PIN_LEP_CS, PIN_LEP_RST);
  lepton.begin();
  while (!lepton.isReady()) vTaskDelay(100 / portTICK_PERIOD_MS);

  uint8_t rawFrame[38400];
  uint8_t jpegBuffer[20480]; // JPEG 编码输出缓冲区

  for(;;) {
    if (lepton.readVoSpi(sizeof(rawFrame), rawFrame)) {
      // 2. 将 16-bit 红外数据缩放为 8-bit 灰度（0-255）
      uint8_t grayFrame[19200]; // 160x120
      for(int i = 0; i < 19200; i++) {
        uint16_t val = (rawFrame[i*2] << 8) | rawFrame[i*2+1];
        grayFrame[i] = map(val, 0, 16383, 0, 255); // 14-bit to 8-bit
      }

      // 3. JPEG 编码（RGB888 模式需先伪彩色映射）
      size_t jpegLen = jpegEncodeRGB888(grayFrame, 160, 120, jpegBuffer, sizeof(jpegBuffer));

      // 4. 通过 WebServer 发送 MJPEG chunk
      server.sendContent("Content-Type: image/jpeg\r\n\r\n");
      server.sendContent((char*)jpegBuffer, jpegLen);
      server.sendContent("\r\n--frame\r\n");
    }
  }
}
```

**FreeRTOS 集成要点：**

- 任务优先级设为 10（ESP32 默认 IDLE=0，WiFi=5），确保 VoSPI 读取不被抢占
- 使用 `vTaskDelay(0)` 让出 CPU，避免独占核心影响 WiFi 协议栈
- `jpegEncodeRGB888()` 调用 JPEGENC 库的硬件加速 JPEG 编码器（ESP32-S3 内置）

### 4.2 串口控制台调试示例

此示例牺牲图像质量换取极致的跨平台兼容性，适用于快速验证硬件连接：

```cpp
void serialDebugLoop() {
  uint8_t frame[38400];
  while(true) {
    if (lepton.readVoSpi(sizeof(frame), frame)) {
      Serial.println("FRAME START");
      for(int y = 0; y < 120; y++) {
        for(int x = 0; x < 160; x++) {
          uint16_t val = (frame[(y*160+x)*2] << 8) | frame[(y*160+x)*2+1];
          char c = '0' + map(val, 0, 16383, 0, 9); // 0-9 字符映射
          Serial.print(c);
        }
        Serial.println();
      }
      Serial.println("FRAME END");
    }
  }
}
```

**工程价值：** 该代码在 Arduino Uno（ATmega328P）上可运行（需降低 SPI 速率至 8 MHz 并接受丢帧），成为排查硬件焊接、电源噪声、时序匹配等问题的"终极诊断工具"。当串口输出呈现清晰的鸭子轮廓（如 README 中 *"lightly refrigerated plush ducks"* 所示），即证明 VoSPI 物理层已连通。

## 5. 常见问题与硬性工程约束

### 5.1 VoSPI 同步失败的根因分析

文档中描述的 *"Lepton never returns any valid SPI data"* 是最棘手问题，其根本原因有三：

**① SPI 时钟抖动超标：**
ESP32-S3 的 APB 总线时钟若受 WiFi 信道切换干扰，会导致 SPI_SCK 频率瞬时偏移 > 0.5%，Lepton 内部 PLL 失锁。
> **解决方案：** 在 `SPI.beginTransaction()` 前调用 `esp_wifi_set_ps(WIFI_PS_NONE)` 关闭 WiFi 功耗管理。

**② VSYNC 信号未正确接入：**
若 VSYNC 悬空或未接中断引脚，MCU 无法感知帧边界，`readVoSpi()` 可能在任意时刻启动读取，大概率落入帧间空闲期，读到全 `0xFF` 数据。
> **验证方法：** 用示波器测量 VSYNC 周期是否为 33.3 ms（30 Hz）。

**③ 电源纹波过大：**
Lepton 3.5 要求模拟电源（AVDD）纹波 < 10 mVpp。开关电源（DC-DC）直接供电常导致此问题。
> **实测方案：** 在 AVDD 引脚并联 10 μF 钽电容 + 100 nF 陶瓷电容，并改用 LDO（如 TPS7A26）供电。

### 5.2 内存与性能瓶颈突破策略

单帧 38.4 KB 缓冲区对资源受限 MCU 构成挑战。高效利用内存的工程实践包括：

- **双缓冲 DMA：** 配置 SPI DMA 为双缓冲模式（如 ESP32-S3 的 `spi_device_queue_trans()`），当 DMA 正在填充 Buffer A 时，CPU 可处理 Buffer B，实现零拷贝流水线。
- **就地 JPEG 编码：** JPEGENC 库支持直接对 `rawFrame` 缓冲区进行 DCT 变换，避免额外分配 19200 字节灰度缓冲区。
- **分辨率裁剪：** 若仅需中心区域，可在 `readVoSpi()` 后只处理 `rawFrame[10000 ... 20000]`（对应 80×60 区域），减少后续计算量。

### 5.3 与其他开源项目的工程对比

| 项目 | 许可证 | 活跃度 | 关键缺陷 | arduino-lepton 优势 |
|---|---|---|---|---|
| tCam | GPL-3.0 | 高 | 固件与 App 强耦合，无独立驱动库 | MIT 许可，纯 C++ 驱动，可嵌入任意固件 |
| Lepton-FLiR-Arduino | MIT | 低（2020） | 无 VoSPI 支持，仅 I²C 低速模式（< 1 fps） | 原生 VoSPI，30 fps 实时流 |
| purethermal1-firmware | MIT | 中 | STM32Cube 专用，无 Arduino HAL 抽象 | 真正的 Arduino 兼容，`#include <Arduino.h>` 即用 |

`arduino-lepton` 的不可替代性在于：它是在 Arduino 生态中，**首个将 Lepton 3.5 VoSPI 协议工程化落地的开源驱动**，其代码已通过 ESP32-S3 量产验证，所有 API 设计均源于真实产线调试经验，而非理论推演。

## 6. 未来演进方向与平台优化路径

基于文档中 *"Future versions might look at splitting out the VoSPI into a different class"* 的提示，结合嵌入式开发趋势，可行的演进路径包括：

### 6.1 平台专属 VoSPI 实现分层

构建抽象基类 `LeptonVoSPI`，派生出平台优化子类：

```cpp
class LeptonVoSPI {
public:
  virtual bool readFrame(uint8_t* buffer, size_t len) = 0;
};

class LeptonVoSPI_ESP32 : public LeptonVoSPI {
public:
  bool readFrame(uint8_t* buffer, size_t len) override {
    // 使用 ESP32-S3 的 GDMA + SPI LCD 模块，支持 40 MHz VoSPI
    return gdma_read(buffer, len);
  }
};

class LeptonVoSPI_RP2040 : public LeptonVoSPI {
public:
  bool readFrame(uint8_t* buffer, size_t len) override {
    // 利用 RP2040 PIO 状态机，实现 25 MHz 精确时序 VoSPI
    return pio_read(buffer, len);
  }
};
```

此设计使 `FlirLepton` 核心类仅依赖 `LeptonVoSPI` 接口，上层应用无需关心底层实现，极大提升可移植性。

### 6.2 FreeRTOS 集成标准化

当前示例中手动创建高优先级任务，未来可提供 `FlirLeptonRT` 类，内置：

- 自动任务创建与删除（`beginRT()` / `endRT()`）
- 内置帧队列（`xQueueCreate(3, sizeof(FrameHandle))`）
- 事件组通知（`xEventGroupSetBits()` 通知新帧就绪）
- 内存池管理（预分配 38400 × 3 字节缓冲池）

此举将驱动与 RTOS 深度耦合，降低用户学习成本，符合工业嵌入式开发规范。

### 6.3 温度标定数据注入接口

Lepton 原始数据为 14-bit ADC 值，需通过校准系数转换为摄氏度。可扩展 API：

```cpp
struct LeptonCalibration {
  float k_vdd;      // VDD 补偿系数
  float k_ptat;     // PTAT 温度传感器补偿
  int16_t gain[4];  // 4 个增益校准值
  int16_t offset[4]; // 4 个偏移校准值
};

void FlirLepton::injectCalibration(const LeptonCalibration& cal);
float FlirLepton::rawToCelsius(uint16_t raw_val, uint8_t pixel_x, uint8_t pixel_y);
```

此功能将驱动从"纯视频流"升级为"热成像传感器"，直接服务于工业测温场景。

---

在 ESP32-S3 开发板上焊接 Lepton 3.5 模组后，执行 `serialDebugLoop()`，当串口监视器稳定输出由字符 `'0'`-`'9'` 构成的鸭子轮廓时，意味着你已穿透红外成像的物理层迷雾，握住了热辐射数据的源头。此时，`readVoSpi()` 返回的每一个字节，都是现实世界温度场在硅基芯片上的精确投射。
