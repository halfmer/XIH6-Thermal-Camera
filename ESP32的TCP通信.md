# 【物联网初探】- 06 - ESP32 利用 WiFi 进行 TCP 通信（Arduino IDE）

## 文章目录

- [1. 硬件、接线、环境配置](#1-硬件接线环境配置)
- [2. ESP32 下的 WiFi 基本功能 (Arduino)](#2-esp32-下的-wifi-基本功能-arduino)
  - [2.1 WiFi 模式切换](#21-wifi-模式切换)
  - [2.2 AP 模式 - 开启 WiFi 热点](#22-ap-模式---开启-wifi-热点)
  - [2.3 STA 模式 - 连接已有 WiFi](#23-sta-模式---连接已有-wifi)
- [3. ESP32 下 TCP Server/Client 通信](#3-esp32-下-tcp-serverclient-通信)
  - [3.1 TCP / UDP 的极简释义](#31-tcp--udp-的极简释义)
  - [3.2 ESP32 TCP 通信小例子](#32-esp32-tcp-通信小例子)
    - [3.2.1 准备工具](#321-准备工具)
    - [3.2.2 通信流程](#322-通信流程)
    - [3.2.3 简单测试](#323-简单测试)

---

## 1. 硬件、接线、环境配置

前置文章参考：

- [【物联网初探】- 01 - ESP32 开发环境搭建 (Arduino IDE)]()
- [【物联网初探】- 02 - ESP32 利用 SPI 联通 TFT 彩屏 (Arduino IDE)]()
- [【物联网初探】- 03 - ESP32 结合 TFT_eSPI 库标定 TFT 触摸屏 (Arduino IDE)]()
- [【物联网初探】- 04 - ESP32 结合 LVGL 库开发环境搭建 (Arduino IDE)]()
- [【物联网初探】- 05 - ESP32 上 LVGL 库的多个例程测试 (Arduino IDE)]()

---

## 2. ESP32 下的 WiFi 基本功能 (Arduino)

头文件包含 `#include <WiFi.h>`，参考 [Arduino 官网 WiFi 库文档](https://www.arduino.cc/reference/en/libraries/wifi/)。

推荐参考博客：[使用 Arduino 开发 ESP32（03）：WiFi 基本功能使用]()

### 2.1 WiFi 模式切换

ESP32 可设置 WiFi 工作的三种模式：

| 模式 | 说明 |
|------|------|
| **AP** | 自身热点模式 |
| **STA** | 连接外部 WiFi 模式 |
| **AP+STA** | 混合模式 |

可通过 `WiFi.mode(WIFI_MODE_XX);` 来设置，其中 `WIFI_MODE_XX` 参数如下：

```cpp
typedef enum {
    WIFI_MODE_NULL = 0,  /**< null mode */
    WIFI_MODE_STA,       /**< WiFi station mode */
    WIFI_MODE_AP,        /**< WiFi soft-AP mode */
    WIFI_MODE_APSTA,     /**< WiFi station + soft-AP mode */
    WIFI_MODE_MAX
} wifi_mode_t;
```

### 2.2 AP 模式 - 开启 WiFi 热点

**极简代码**：只需调用该函数，即可开启一个名为 `ESP32` 的热点，此时模块自身 IP 默认为 `192.168.4.1`。

```cpp
WiFi.softAP("ESP32");
```

**常用代码**：定义好 WiFi 的名字、密码、自身 IP、网关、子网掩码，并传入 AP 启动函数：

```cpp
#include <WiFi.h>

const char *ssid = "ESP32_wifi";
const char *password = "12345678";

IPAddress local_IP(192, 168, 4, 2);
IPAddress gateway(192, 168, 4, 9);
IPAddress subnet(255, 255, 255, 0);

void setup() {
    Serial.begin(9600);                            // 启动串口通讯
    WiFi.mode(WIFI_AP);                            // 设置为接入点模式
    WiFi.softAPConfig(local_IP, gateway, subnet);  // 配置接入点的 IP、网关、子网掩码

    Serial.printf("设置接入点中 ... ");

    // 启动校验式网络（需要输入账号密码），通道=3，隐藏 WiFi，最大连接数=4
    WiFi.softAP(AP_ssid, password, 3, 1);

    bool flag = WiFi.softAP(AP_ssid, password);    // 监控状态变量
    if (flag) {
        Serial.println("开启成功");
    } else {
        Serial.println("开启失败");
    }
}

void loop() {}
```

### 2.3 STA 模式 - 连接已有 WiFi

**极简代码**：只需指定 WiFi 的名字和密码即可连接。

```cpp
WiFi.begin(ssid, password);
```

**常用代码**：

```cpp
#include <WiFi.h>

const char *ssid = "********";       // 网络名称
const char *password = "********";   // 网络密码

void setup() {
    Serial.begin(9600);
    Serial.println();
    WiFi.begin(ssid, password);      // 连接网络

    while (WiFi.status() != WL_CONNECTED) {  // 等待网络连接成功
        delay(500);
        Serial.print(".");
    }

    Serial.println("WiFi connected, IP address: ");
    Serial.println(WiFi.localIP());  // 打印模块 IP
}

void loop() {}
```

---

## 3. ESP32 下 TCP Server/Client 通信

### 3.1 TCP / UDP 的极简释义

| 协议 | 全称 | 特点 |
|------|------|------|
| **TCP** | Transmission Control Protocol（传输控制协议） | 面向连接，收发数据前必须和对方建立可靠的连接 |
| **UDP** | User Data Protocol（用户数据报协议） | 非连接，传输数据前源端和终端不建立连接 |

> 简单理解：TCP 必须客户端和服务端连接上才能收发数据；UDP 则是数据都扔在网上，谁用谁收，谁有谁发。

### 3.2 ESP32 TCP 通信小例子

#### 3.2.1 准备工具

推荐使用**网络调试助手**：

- Linux 下可使用 [NetAssistant](https://github.com/)（GitHub 开源项目）
- 任意能够创建 TCP 客户端的小工具都可以
- 也可以利用 Python 脚本实现

> 下面以 NetAssistant 为例演示。

#### 3.2.2 通信流程

当服务端和客户端在同一网络下，**ESP32 作为客户端，其他主机作为服务端**，基本思路如下：

**第一步**：以家庭 WiFi 为例，先确定服务端主机的 IP 和端口号。这里在 Ubuntu 主机上利用 NetAssistant 建立一个 TCP 服务器，IP 为本机 IP，端口号随意设置为 `56050`，点击连接网络，就建立好了服务端。

> *(此处为示意图位置)*

**第二步**：ESP32 使用 STA 模式连接家庭 WiFi，并建立客户端，与服务端建立连接，在建立的 TCP 连接上互相通信。

```cpp
#include <WiFi.h>

const char *ssid = "xxx";         // WiFi 名
const char *password = "xxx";     // WiFi 密码

const IPAddress serverIP(192, 168, 31, 133);  // 欲访问的服务端 IP 地址
uint16_t serverPort = 56050;                   // 服务端口号

WiFiClient client;  // 声明一个 ESP32 客户端对象，用于与服务器进行连接

void setup() {
    Serial.begin(115200);
    Serial.println();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // 关闭 STA 模式下 WiFi 休眠，提高响应速度
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("Connected");
    Serial.print("IP Address:");
    Serial.println(WiFi.localIP());
}

void loop() {
    Serial.println("尝试访问服务器");

    if (client.connect(serverIP, serverPort)) {  // 尝试访问目标地址
        Serial.println("访问成功");

        client.print("Hello world!");  // 向服务器发送数据

        // 如果已连接或有收到的未读取的数据
        while (client.connected() || client.available()) {
            if (client.available()) {  // 如果有数据可读取
                String line = client.readStringUntil('\n');  // 读取数据到换行符
                Serial.print("读取到数据：");
                Serial.println(line);
                client.write(line.c_str());  // 将收到的数据回发
            }
        }

        Serial.println("关闭当前连接");
        client.stop();  // 关闭客户端
    } else {
        Serial.println("访问失败");
        client.stop();  // 关闭客户端
    }
    delay(5000);
}
```

**第三步**：连接测试。

1. 保持 NetAssistant 上的服务端运行
2. ESP32 连接在电脑上，打开 Arduino 的串口监视器，设置好波特率
3. 烧录程序至 ESP32，成功后即会显示连接信息（WiFi 连接正常）
4. 此时 NetAssistant 上的窗口也会收到 ESP32 发送的 **"Hello world!"** 字符串

> *(此处为串口监视器示意图位置)*

#### 3.2.3 简单测试

可以在 NetAssistant 上发送任意字符至 ESP32 客户端，在 Arduino 的串口监视器中即可接收到。

例如在 NetAssistant 上发送 `"esp32 TCP test !!!"` ，如下图所示：

> *(此处为测试示意图位置)*

> **提示**：反向传输的话需要自己写一个 Arduino 上的串口数据接收函数，本例程中没有实现。
