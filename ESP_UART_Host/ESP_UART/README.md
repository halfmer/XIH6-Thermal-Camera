# ESP UART — 串口调试 & 热成像上位机

跨平台（Windows / Linux）ESP32-S3-N16R8 上位机，支持**串口调试**和 **LEPTON 3.5 热成像视频流**两种模式，基于 Qt 6 + QSerialPort。

## 功能

### 串口调试模式
- 串口参数配置（端口、波特率、数据位、停止位、校验位）
- 文本 / Hex 双模式收发，带时间戳和彩色区分
- DTR / RTS 引脚控制 — ESP32 自动下载电路
- 字节计数器、自动滚动、定时发送
- 保存日志、换行符选择、转义字符解析

### 热成像模式
- LEPTON 3.5 160×120 热成像视频实时显示
- **6 种色表**: 铁红 / 白热 / 黑热 / 彩虹 / 熔岩 / 极地
- 自动温标 (min/max)、中心点十字标
- FPS 帧率、帧计数、错误帧统计
- 二进制帧协议自动同步

## 热成像协议

ESP32 端需按以下格式发送帧数据（波特率建议 **921600**）：

```
[0xAA, 0x55]     2 bytes  同步头
[type]            1 byte   帧类型: 0x01 = raw 16-bit thermal
[frame_id]        2 bytes  帧序号 uint16 BE
[width]           2 bytes  图像宽度 uint16 BE
[height]          2 bytes  图像高度 uint16 BE
[pixel_len]       4 bytes  像素数据字节数 uint32 BE (= width × height × 2)
[pixel_data]      N bytes  原始像素 uint16 BE (14-bit 温度值)
[checksum]        2 bytes  前面所有字节的累加和 uint16 BE
```

### ESP32 端参考代码

```cpp
// Arduino / ESP-IDF 示例
void sendThermalFrame(uint16_t *pixels, uint16_t w, uint16_t h, uint16_t frame_id) {
    uint32_t pixel_len = w * h * 2;
    uint8_t header[] = {
        0xAA, 0x55,                    // sync
        0x01,                          // type: raw thermal
        (uint8_t)(frame_id >> 8), (uint8_t)(frame_id & 0xFF),
        (uint8_t)(w >> 8), (uint8_t)(w & 0xFF),
        (uint8_t)(h >> 8), (uint8_t)(h & 0xFF),
        (uint8_t)(pixel_len >> 24), (uint8_t)(pixel_len >> 16),
        (uint8_t)(pixel_len >> 8), (uint8_t)(pixel_len & 0xFF),
    };
    Serial.write(header, sizeof(header));

    // Write pixel data as big-endian uint16
    for (uint32_t i = 0; i < w * h; i++) {
        Serial.write((uint8_t)(pixels[i] >> 8));
        Serial.write((uint8_t)(pixels[i] & 0xFF));
    }

    // Checksum
    uint16_t cksum = 0;
    for (int i = 0; i < sizeof(header); i++) cksum += header[i];
    uint8_t *p = (uint8_t *)pixels;
    for (uint32_t i = 0; i < pixel_len; i++) cksum += p[i];
    Serial.write((uint8_t)(cksum >> 8));
    Serial.write((uint8_t)(cksum & 0xFF));
}
```

## 项目结构

```
ESP_UART/
├── CMakeLists.txt
├── CMakePresets.json
├── main.cpp
├── mainwindow.h / .cpp       # 主窗口 + 双模式
├── colormap.h / .cpp         # 热成像色表
├── frameparser.h / .cpp      # 二进制帧协议解析
├── thermalwidget.h / .cpp    # 热成像显示控件
├── .gitignore
└── README.md
```

## 构建

- CMake ≥ 3.16
- Qt 6.8+（Widgets + SerialPort）
- 编译器：Linux GCC / Windows MSVC 2022 或 MinGW

```bash
# Linux — 使用 Qt 6.11.1
cmake --preset linux && cmake --build --preset linux
./build/esp_uart

# Windows — MSVC
cmake --preset windows-msvc
cmake --build --preset windows-msvc --config RelWithDebInfo
```

## 许可

内部工具，暂无。
