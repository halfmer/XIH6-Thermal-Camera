# README_14 - 协作式多外设调度与火灾预警（MQ-2/MQ-135）

日期：2026-07-09。承接 README_13（双链路切换）；本篇解决"开流后其他外设全部饿死"
的结构问题，并接入火灾预警传感器。

## 1. 需求

1. 各外设**伪同时运行、互不干扰**：LEPTON 视频（UART4 或 SPI5→ESP32 TCP）、
   SHT40、OLED、SD 卡、MQ-2、MQ-135、火灾指示灯（先不写蜂鸣器）。
2. MQ-2（PA1_C）与 MQ-135（PA0_C）数据显示在 OLED；超阈值点亮火灾 LED。
3. 不引入 FreeRTOS——协作式轮询 + 时间片门控（README_13 §4.3 形态）。

## 2. 硬件事实

```text
MQ-2    -> PA1_C = ADC1_INP1   (CubeMX 已配: 16bit, 软件触发单次转换)
MQ-135  -> PA0_C = ADC2_INP0   (CubeMX 已配: 同上)
火灾LED -> PJ15 (LED_CONTROL) -> AO3400 NMOS, 高=亮, 低=灭
蜂鸣器  -> PG9  (BEEP_GPIO)   -> NPN 低边驱动, 高=响, 低=不响 (本轮不驱动)
```

## 3. 结构改动：主循环从"流模式独占"改为时间片调度

**旧问题**：`if (Lepton_Stream_Active()) { ...; continue; }`——开流（现默认开机
即流）后 `continue` 跳过一切，SHT40/OLED/SD 永不运行。

**新形态**（main.c，视频始终是第一优先客户）：

```text
while(1):
  Lepton_Stream_Poll()                       // IO0时序/链路切换/ok报文
  slice0: 视频     每轮      流模式=采集+发送; 空闲模式=8s一次健康检查
  slice1: FireGuard 200ms门控 MQ-2/MQ-135 ADC轮询采样 + 报警LED
  slice2: OLED气体  500ms门控 首行 "G:xxxxx A:xxxxx FIRE"
  slice3: SHT40     2s门控    读温湿度(~10ms) + OLED 两行
  slice4: SD热插拔  1s门控    PG10 电平沿检测
```

互不干扰的依据（每片资源独占 + 时长有界）：

| 时间片 | 外设资源 | 单次耗时 | 与视频链路交集 |
|---|---|---|---|
| FireGuard | ADC1/ADC2 独占 | ~µs×2 | 无 |
| OLED | SW_I2C 位拍 GPIO | ~ms 级 | 无 |
| SHT40 | I2C（含 10ms 延时） | ~10ms/2s | 无 |
| SD 检测 | PG10 GPIO 读 | ~µs | 无（插卡初始化为罕见事件） |
| 视频 | SPI4+UART4/SPI5+DMA | 帧周期 | — |

Step9A 红线保持：时间片内**零 USART1 打印**（SD 插拔时的 `SD_ReportStatus`
打印为罕见人工事件，风险已知可接受）；`ok/change over` 仍只在 DMA 空闲时输出。

## 4. 新增模块 Drivers/PER/FIRE/fire_guard.c/h

```text
FireGuard_Init : PJ15/PG9 置低; ADC1/ADC2 一次性线性校准(启动路径)
FireGuard_Poll : 200ms 门控; 两路 ADC 轮询读取; 1/8 IIR 滤波;
                 迟滞报警: 任一路 >= ON 阈值锁存报警, 两路都 < OFF 阈值才解除;
                 PJ15 = 报警状态; PG9 蜂鸣器钩子已写好但注释停用(单变量原则)
FireGuard_MQ2 / _MQ135 / _Alarm : 供 OLED/后续逻辑读取
阈值(占位, 待现场标定, MQ 系列需预热数分钟):
  FIRE_MQ2_ON_RAW=20000 / OFF=15000; FIRE_MQ135_ON_RAW=20000 / OFF=15000 (16bit 满量程 65535)
```

OLED 版面：首行原 "--- SHT40 DATA ---" 改为 `G:xxxxx A:xxxxx FIRE`（12 号字），
Temp/Humi/SD 三行不变。

工程登记：uvprojx 新增组 `Drivers/PER/FIRE` + IncludePath `../Drivers/PER/FIRE`。

## 5. 改动文件清单

```text
Drivers/PER/FIRE/fire_guard.c/.h   (新增)
Core/Src/main.c                    (主循环时间片重构 + FireGuard_Init + OLED首行)
MDK-ARM/XIH6_V2.uvprojx            (FIRE 组 + 包含路径)
```

不动：lepton.c、lepton_stream.c/h（本轮零改动）、协议、Qt、ESP32。

## 6. 上板验证清单

1. 构建 0E/0W，记 HEX MD5，git commit。
2. 上电（默认 UART4 流模式）：Qt 热成像(串口)出图的**同时**，OLED 上
   G/A 数值每 0.5s 刷新、Temp/Humi 每 2s 刷新——这是"伪同时"的直接证据。
3. 对 MQ-2 打火机放气（不点火）：G 值上升，超 20000 时 PJ15 灯亮，
   OLED 出现 FIRE；散去后低于 15000 灯灭。
4. 视频 FPS 与 checksum_bad 与上一版持平（时间片不得拖慢视频）。
5. 阈值按实测数据回填（README 记录标定值），之后再单变量启用 PG9 蜂鸣器。

## 7. 已知边界

- MQ 传感器上电预热期（分钟级）读数偏高，可能开机即误报——标定阈值时一并处理
  （必要时加开机静默期）。
- SHT40 的 10ms 阻塞在流模式下每 2s 发生一次，理论上使当帧发送推迟 ≤10ms，
  对 2FPS 无感；若未来提帧率到 8fps 需改非阻塞两段式读取。
## 8. 第二轮修订：ppm 量化 + A=0 根因 + 时间轮化（2026-07-09）

上板反馈：G 从 ~10000 缓慢下降、A 恒 0、担心 LEPTON 无快门动作。

### 8.1 判读与根因

1. **G 缓慢下降 = MQ-2 加热丝预热的正常曲线**（冷态 Rs 低→读数高，热稳定后回落；
   手册要求预热后数据才可信）——不是调度问题；
2. **A 恒 0 根因 = CubeMX 生成的 ADC 采样时间仅 1.5 周期**：MQ 模块分压输出源阻抗
   kΩ~10kΩ 级，1.5 周期采样电容根本充不满。修复：运行期把 ADC1/ADC2 通道重配为
   **810.5 周期**（~32µs，防 CubeMX regen 回退）。若修复后 A 仍为 0 → 量 MQ-135
   模块 AO 电压（接线/5V 供电问题）；
3. LEPTON 快门：本轮调度不触碰 SPI4/CCI；流模式下 USART1 无 [LEP] 日志属于设计
   行为——**CH340 发 'P' 切到空闲模式**即可看到 8s 一次的健康检查日志来定位。

### 8.2 MQ 系列标准量化（业内通用模型，来源见 §8.5）

裸 ADC 值无物理意义，标准单位是 **ppm**：

```text
Vout  = raw/65535 × 3.3V × 分压系数
Rs    = RL × (Vc − Vout) / Vout        RL=模块负载电阻, Vc=5V
ratio = Rs / R0                        R0 = 洁净空气中的 Rs / 洁净空气比
ppm   = A × ratio^B                    数据手册对数曲线幂律拟合
MQ-2 : 洁净比 9.83, LPG/烟雾 A=574.25  B=-2.222
MQ-135: 洁净比 3.60, CO2当量  A=116.602 B=-2.769
```

状态机：`WARMUP(60s, 报警抑制, 解决冷态误报)` → `CALIB(洁净空气 16 拍均值定 R0)`
→ `RUN(ppm + 迟滞报警: MQ-2≥300ppm 或 MQ-135≥2000ppm 亮灯, 双双回落才灭)`。
OLED 首行随状态显示 `MQ PREHEAT xxs` / `MQ CAL` / `G%4uppm A%4u FIRE`。

已知精度边界：MQ-135 对多种气体交叉敏感，ppm 只能作空气质量指示而非精确 CO2；
模块 RL 存在 1kΩ 廉价版（宏 `FIRE_MQ2_RL_OHM/FIRE_MQ135_RL_OHM` 按实物改）；
**MQ 模块 AO 满幅可到 5V 而 H743 模拟脚不耐 5V——需分压电阻并设
`FIRE_ADC_DIVIDER`**；手册标定建议 24h 老化后进行。

### 8.3 时间轮化

背景任务改为 `App_WheelTask_t wheel[]` 表驱动（period/last/fn），视频路径
**不进轮**、每轮无条件先跑——与 README_10 失败的时间轮的本质区别就在这里。

### 8.4 改动文件（第二轮）

```text
Drivers/PER/FIRE/fire_guard.c/.h  重写: 810.5周期采样 + ppm 模型 + 三态状态机
Core/Src/main.c                   时间轮任务表 + OLED 状态/ppm 显示
```

### 8.5 参考来源

- [Codrey: How To Use MQ-135 Gas Sensor](https://www.codrey.com/electronic-circuits/how-to-use-mq-135-gas-sensor/)
- [CircuitDigest: Measuring CO2 with MQ-135](https://circuitdigest.com/microcontroller-projects/interfacing-mq135-gas-sensor-with-arduino-to-measure-co2-levels-in-ppm)
- [GitHub: MQ-135 CO2 Calibration notes](https://github.com/Bobbo117/MQ135-Air-Quality-Sensor)
- [TeachMeMicro: MQ-135 tutorial](https://www.teachmemicro.com/mq-135-air-quality-sensor-tutorial/)

## 9. 第三轮：LEPTON 过温报警——热点 ≥100°C 灯闪+蜂鸣（2026-07-09）

需求：热像画面里检测到 **100°C 以上就直接驱动 RGB 灯闪烁和蜂鸣器响**。

### 9.1 设计

温度来源是 LEPTON TLinear raw（0.01 K/LSB，README_2）：`raw = (T°C+273.15)×100`，
故 **ON=37315 (100.00°C)，OFF=36815 (95.00°C，5°C 迟滞)**。与 Qt 端
`raw*0.01-273.15` 同一约定，无需 CCI（TLinear 为出厂默认，CCI 硬件仍搁置）。

```text
采集侧  main.c: Lepton_Capture_Frame()==1 后调 FireGuard_ThermalScan()
        - 流模式: SendFrame() 之后扫描, ~100µs 与 TX DMA 并行, 视频零延迟
        - 空闲模式: 8s 健康检查成功后扫描(此模式下报警刷新最慢 8s)
判决    fire_guard.c: 全帧 19200 像素一遍
        - 跳过 0xFFFF(死像素哨兵, README_9~12 时期已确认此传感器有坏点)
        - ≥4 个像素(FIRE_THERMAL_MIN_PIX) raw≥ON → 锁存报警
          (孤立坏点永远触发不了; 真实 100°C 热源在 160×120 视场必占多像素)
        - 有效像素最大值 < OFF → 解除; 无数据超时——火警保持响直到画面变凉(fail-loud)
输出    FireGuard_Poll(200ms 节拍) 优先级:
        1) 过温: PJ15 与 PG9 同拍翻转 → 2.5Hz 闪烁 + 断续鸣叫(蜂鸣器首次启用)
        2) 仅气体: PJ15 常亮, 蜂鸣器静音(维持原行为)
        3) 无报警: 全灭
        过温通道不受 MQ WARMUP/CALIB 抑制——热像无需预热。
OLED    RUN 态首行 FIRE 字样改为 气体||过温; WARMUP/CALIB 态首行仍显示预热/标定
        文案(此时过温若触发, 灯闪+蜂鸣已足够示警, 属已知可接受)
```

### 9.2 改动文件

```text
Drivers/PER/FIRE/fire_guard.c/.h  过温通道: ThermalScan + 输出级优先仲裁 + PG9 启用
Core/Src/main.c                   两处 Capture 成功点挂 ThermalScan + OLED FIRE 联动
```

不动：lepton.c/lepton_stream.c、协议、Qt、ESP32、Android——四端兼容性天然保持。

### 9.3 上板验证清单

1. 打火机火焰/烙铁对准镜头：Qt 图上热点出现的同时 PJ15 闪烁(2.5Hz)、蜂鸣器
   断续响、OLED 出现 FIRE；
2. 移开热源、画面冷却后灯灭声止（95°C 迟滞解除）；
3. 常温下长时间运行无误报（验证死像素免疫：坏点若恰读 >37315 但 <4 个不触发）；
4. 气体报警回归：MQ-2 打气仍是常亮不响，与过温行为可区分；
5. 视频 FPS/checksum_bad 与 160CD0F1 基线持平（扫描不拖视频）。

