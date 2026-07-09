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
→ `RUN(ppm + 迟滞报警: MQ-2≥4ppm 或 MQ-135≥2000ppm 亮灯, 双双回落才灭)`。
OLED 首行随状态显示 `MQ PREHEAT xxs` / `MQ CAL` / `G%4uppm A%4u FIRE`。

> 注：MQ-2 清洁空气基线 = 574.25 × 9.83^(−2.222) ≈ **3.58 ppm**，所以 RUN 态
> 持续显示 G=3~4ppm 是**正常基线**，不是故障；打火机放气应使 Rs 下降、ppm 上升
> 到几十~上百。阈值 4ppm 即"高于基线即报警"（用户 2026-07-09 指定：G>4ppm 亮 RGB 灯）。

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

## 9. 第三轮修订：同帧装配 + 死像素修复 + AGC关闭 + 色图过滤 + OLED原始值（2026-07-09）

上板反馈四条：① 打火机火焰上位机只读出"几度"；② 撕裂拒显虽改善但仍有
160×80 上下半割裂；③ G 恒 3~4ppm、A 恒 0；④ 希望 STM32 做简单图像处理让轮廓
明显，且**不影响其他外设与视频流**。本节逐条归因与修复。

### 9.1 撕裂根因 = 持久 shelf 跨帧拼接（lepton.c）

旧 `Lepton_Capture_Frame` 的发布条件是"seg==4 且 `vospi_cached_mask==0x0F`"，
而 **shelf 永不清除**——若第 N 帧丢了 segment 3，shelf 里仍留着第 N−1 帧的
segment 3；第 N 帧 seg4 到达时 mask=0x0F，发布的就是
{seg1,2,4 来自 N 帧} + {seg3 来自 N−1 帧} → segment 边界（第 60 行）水平撕裂。
Qt 侧段缓存（§10 旧版）虽不丢弃撕裂帧，但每帧都把 4 段拷进缓存，撕裂照样保留。

**修复：同帧装配**。新增 `frame_seg_mask`（当前帧的段位图），以"seg==1 到达"
作为帧边界判据：若 seg1 到达时 `frame_seg_mask != 0`，说明上一帧不完整（丢了
某段），**丢弃部分集**而不是跨帧拼接。仅当 `frame_seg_mask == 0x0F`（同一帧的
4 段全到齐）才发布。丢段计数进 `vospi_stale_block`。

```text
旧: shelf 永不清除 → 丢段时跨帧缝合 → 撕裂
新: seg1=帧边界, 丢段即丢帧, 同帧 4 段齐才发布 → 连贯完整, FPS 换完整性(Principle 1)
```

每次 `Lepton_Capture_Frame` 入口复位 `frame_seg_mask=0`，失败捕获的残留段不跨
调用泄漏。`vospi_cached_mask`（持久"曾见"诊断）保持不变。

### 9.2 火焰"只多度"根因 = AGC 未关 + p98 截断（lepton.c + colormap.cpp）

双重根因：

1. **AGC 工厂默认 ON**。AGC 把场景动态范围重映射，raw16 不再是绝对 centikelvin，
   即便 `Lepton_EnableTLinear(1)` 也被 AGC 压回相对值——火焰读出接近环境温度。
   修复：`Lepton_Init` 在 TLinear 之前显式 `Lepton_SetAGC(0)`。
2. **Qt p98 百分位截断火焰**。火焰像素 <2%，被 p98 当离群点 clamp 到 p98 值，
   显示不够亮。修复：colormap 先**过滤 0 与 0xFFFF**（VoSPI 丢段/坏像素/饱和火焰
   像素），再对剩余有效像素取 p2/p98；火焰像素（0xFFFF）被排除后，在渲染循环里
   经 `qBound` 钳到 LUT 顶端 → 显示为最亮色，温度读数也回到 382°C（0xFFFF 的
   centikelvin 换算）而非"几度"。

### 9.3 STM32 图像处理：死像素插值（lepton.c，§11）

用户要求"STM32 做简单图像处理让轮廓明显，前提是不妨碍其他外设与视频流"。
真正的边缘锐化（Sobel/unsharp）会修改像素值、破坏绝对温度——与①矛盾。
折中方案：**死像素插值** `Lepton_Assembly_DeadPixelFix()`——4 邻域均值替换
0/0xFFFF 像素。去除椒盐噪声后轮廓更清晰，且**不改动任何有效像素的辐射值**
（温度精度保留给上位机光标读数）。

- 时机：同帧 4 段齐后、`memcpy` 到 `lepton_raw_frame`（流源）之前——与 TX DMA
  无竞争（采集路径与流路径在主循环里串行，非并发）。
- 开销：O(W×H) ≈ 19200 像素，H7@480MHz <1ms，每帧一次。
- 不触碰 SPI4/UART4/DMA/CCI，零外设交集。

### 9.4 MQ 诊断：OLED 同时显示 raw + ppm（main.c）

`App_Task_Oled` 首行改为 `g%5u %3up a%5u %3up`（MQ-2 raw/ppm + MQ-135 raw/ppm），
WARMUP/CALIB 态也显示 raw。直接在屏上判断：

- **A 恒 0**：看 a 的 raw——若 ≈0 → ADC2 转换 OK 但 AO ≈0V，查 MQ-135 模块 AO
  接线/5V 供电（启动自检 `[FIRE] selftest: MQ135(ADC2) raw=xxxx st=d` 同样判据）；
  若 raw 正常（几千~几万）但 ppm=0 → 标定 R0 异常，查 CALIB 态 raw 是否被当
  "洁净空气"采样；
- **G 打火机不动**：看 g 的 raw——打火机放气时 Rs 下降→Vout 上升→raw 应上升；
  若 raw 不动 → 气体未到传感器（物理位置/外壳遮挡），非代码问题；
- **G=3~4ppm 是正常基线**（§8.2 注），不是故障。

### 9.5 改动文件（第三轮）

```text
Drivers/PER/LEPTON/lepton.c     同帧装配(frame_seg_mask) + 死像素插值 + Lepton_SetAGC(0)
Drivers/PER/LEPTON/lepton.h     vospi_stale_block 注释更新(同帧丢弃计数)
Core/Src/main.c                 App_Task_Oled 改显 raw+ppm(双传感器)
ESP_UART_Host/.../colormap.cpp  过滤 0/0xFFFF 后再 p2/p98(火焰不再被截断)
```

Qt 侧段缓存（`m_segCache`）保留为安全网——STM32 同帧装配后帧本就连贯，缓存只做
兜底。thermalwidget_selftest exit=0（p2/p98 对无 0/0xFFFF 的梯度图仍在 [first,last]
内，§9.2 的过滤不破坏既有断言）。

构建记录：
- STM32 MDK-ARM 0E/0W，HEX MD5 = `f9b072404949867e0cd4aa88cb8e9c08`
- Qt6 mingw 0E/0W，selftest exit=0，exe 已同步 deploy/

### 9.6 上板验证清单

1. 烧录 HEX `f9b072404949867e0cd4aa88cb8e9c08`，Qt 用 `build-codex\esp_uart.exe`。
2. 画面**无 160×80 上下割裂**（同帧装配）；运动时不再冻结（撕裂帧不再产生，
   段缓存兜底）。若 `vospi_stale_block` 增长快 = 丢段率高 = VoSPI 时序仍不稳，
   查 MCLK/SPI4 接线（非本期能修）。
3. 打火机对准火焰像素：温度读数应远高于环境（0xFFFF 饱和≈382°C，或至少 >100°C），
   不再"只多度"；画面火焰显示为色图顶端色（白/亮红），不再粉红。
4. OLED：WARMUP 倒计时期间看 g/a 的 raw——a≈0 即查 MQ-135 接线/5V；RUN 态打火机
   放气看 g 的 raw 上升、ppm 超过 4 → RGB 灯(PJ15)亮。
5. 轮廓清晰度：死像素修复去椒盐噪点后，物体边缘应比上版干净；如需更强轮廓增强
   只能上 Sobel/unsharp，但会破坏绝对温度——已评估为不可接受，不做。


