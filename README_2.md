# README_2 — Lepton 3.5 驱动开发日志（STM32H743 / XIH6_V2）

> 接续 `README.md`（附录 A/B 是 SDMMC 排查）。本篇专记 **FLIR Lepton 3.5 热像仪** 的
> bring-up 开发流程：从"读代码 → 发现根因 → 移植 CCI → 加诊断"，等待上板复测反馈。
>
> 开发日期：2026-07-04 ｜ 阶段：Lepton 首次驱动（未上板验证）

---

## 0. 参考的 Claude Skill

| Skill | 用途 |
|---|---|
| `agent-skill_stm32-freertos` (REFERENCE/DEBUG_TOOLS, HAL_DRIVERS) | HAL I2C/TIM 驱动写法、ITM/UART 诊断打印范式 |
| `embed-ai-tool/serial-monitor` | 串口日志抓取 SOP（USART1@115200，本项目诊断输出通道） |
| `embed-ai-tool/build-keil` `flash-keil` `debug-jlink` | 上板编译/烧录/断点流程（配合 README 附录 A 的 Keil debug SOP） |

> 注：本工程 **未编译 FreeRTOS**（`uvprojx` 里无 tasks.c/port.c），仍是裸机 super-loop，
> 所以 FreeRTOS 相关 skill 只借用其 **HAL 外设 + 调试打印** 部分，不引入 RTOS API。

---

## 1. 硬件接线（以 `LEPTON_IO_CONNET.txt` + CubeMX 实测为准）

| 功能 | 单片机引脚 | 说明 | CubeMX 宏 |
|---|---|---|---|
| VoSPI SCK  | **PE2**  | SPI4_SCK（丝印写 SPI3，实为 SPI4） | — |
| VoSPI MISO | **PE5**  | SPI4_MISO | — |
| VoSPI MOSI | **PE6**  | SPI4_MOSI（空闲，Lepton 不收） | — |
| VoSPI CS   | **PE4**  | 软件 GPIO 片选（整包拉低） | `SPI_SSEL_Pin` |
| CCI SCL    | **PD12** | I2C4_SCL（硬件已上拉；MCU 内部开漏） | — |
| CCI SDA    | **PD13** | I2C4_SDA | — |
| MST_CLK    | **PA8**  | TIM1_CH1 PWM ~24MHz（无 HSE，合成时钟） | `LEPTON_CLK_Pin` |
| **RESET_L**| **PJ3**  | **低有效复位**（本次关键） | `LEPTON_RST_Pin` |
| **PWR_DWN_L**| **PH6** | **低有效掉电**（本次关键） | `LEPTON_PWDN_Pin` |
| VSYNC      | **PB2**  | Lepton 输出（MCU 侧应为输入） | `LEPTON_VSYNC_Pin` |

I2C 从机地址：7-bit `0x2A`（HAL 用 `0x2A<<1`）。

---

## 2. 根因发现 ⭐（本次最重要的结论）

**旧 `lepton.c` 认为 "RESET_L / PWR_DWN_L 未接线，靠内部 POR"，这是错的。**

实测 `Core/Inc/main.h` + `Core/Src/gpio.c`：

```c
/* gpio.c 第 72 行 */
HAL_GPIO_WritePin(GPIOJ, ...|LEPTON_RST_Pin,  GPIO_PIN_RESET);  /* PJ3 = 0 */
/* gpio.c 第 81 行 */
HAL_GPIO_WritePin(GPIOH, TP_CS_Pin|LEPTON_PWDN_Pin, GPIO_PIN_RESET); /* PH6 = 0 */
```

- PJ3(RESET_L) 与 PH6(PWR_DWN_L) 都是 **低有效**；
- CubeMX 在 `MX_GPIO_Init()` 里把它们初始化为推挽输出并 **拉低** →
  **Lepton 一上电就被永久按在"复位 + 掉电"状态**；
- 旧驱动从不碰这两脚 → 传感器根本没跑起来 → `Lepton_Capture_Frame()` 永远
  返回 0（"no frame"）。

> 这是"任何 VoSPI/CCI 配置都救不回来"的硬门槛，必须先在软件里 **按正确时序释放**
> 这两根线。这是本次改动的核心。

---

## 3. 本次改动清单

### 3.1 `Drivers/PER/LEPTON/lepton.h`
- 用 CubeMX 宏（`LEPTON_RST_Pin` 等）替换硬编码，注释订正真实接线与"被按在复位"事实。
- 新增 **CCI 寄存器图 + 命令字宏**：
  - `STATUS=0x0002`(BUSY/BootMode/BootStatus/err)、`COMMAND=0x0004`、
    `DATA_LENGTH=0x0006`、`DATA0=0x0008`；
  - 命令字 = `MODULE_BASE | offset | TYPE [| 0x4000 保护位]`；
  - 关键命令：`RAD_TLINEAR_EN_SET=0x4EC1`、`AGC_ENABLE_SET=0x0101`、
    `SYS_FFC_RUN=0x0242`、`SYS_PING=0x0202`。
- 新增 `Lepton_Status_t` 返回码 与 `Lepton_Diag_t` 诊断结构体（供 main 打印）。

### 3.2 `Drivers/PER/LEPTON/lepton.c`
1. **`Lepton_CtrlPins_Config()`**：接管 PJ3/PH6（输出、先拉低）、PB2 改输入。
2. **`Lepton_PowerOn_Sequence()`**：时钟先行 → 释放 PWR_DWN_L → 50ms → 释放 RESET_L
   → **等 1000ms 启动**（数据手册 ~950ms）。
3. **CCI 命令层**：`WaitBusy / SetAttr / GetAttr / RunCmd`（轮询 BUSY、写 DATA、写
   DATA_LENGTH、写 COMMAND、再轮询、读 error code）。
4. **高层**：`WaitBoot`（轮询 BootStatus）、`EnableTLinear`、`SetAGC`、`RunFFC`。
5. `Lepton_Init()` 串起：MCLK→控制脚→CS→SPI4→上电时序→等启动→开 TLinear，
   **全程不死等**（失败写入 `lepton_diag`，主循环继续）。
6. **VoSPI 保留** 原有分段解析逻辑（已符合 3.5 规格），新增 `Lepton_VoSPI_Resync()`
   （CS 拉高 185ms 强制重同步）。

### 3.3 `Core/Src/main.c`
- 启动后打印一行诊断：
  `[LEP] mclk=.. pwr=.. rst=.. boot=..(..ms) status=0x.... tlin_err=..`；
  未启动再补一行失败提示。
- 抓帧成功后打印 **中心像素 raw + 摄氏度**（TLinear 下 `T=raw*0.01-273.15`）；
  失败时调用一次 `Lepton_VoSPI_Resync()`。
- 修正打印缓冲：中心像素用独立 `char lp[48]`，避免溢出 `disp_buff[32]`。

---

## 4. 关键设计决策与依据

| 决策 | 依据 |
|---|---|
| MCLK 用 24MHz（非 25） | 无 HSE，只能 TIM1 整数分频；240/10=24MHz 在 Lepton 允许区间内 |
| SPI 15MHz、Mode3、8-bit、软 CS | VoSPI 要求 CPOL=CPHA=1，整包 CS 拉低；15MHz < 20MHz 上限 |
| 启动等 1000ms | 数据手册 boot ~950ms；期间 CCI 不可用 |
| 开 **TLinear** 而非手动关 AGC | TLinear 使能即输出辐射线性数据，raw=开尔文×100，最省事 |
| RAD 命令带 `0x4000` 保护位 | RAD/OEM 模块命令字必须置保护位，否则返回错误码 |
| 全程不 while(1) 死等 | 与 SD 一致的"失败也让 super-loop 继续跑"策略，便于串口观察 |

---

## 5. ⚠️ 待上板确认的假设（可能需要按现象微调）

这些是**没有硬件无法验证**的点，复测时重点看：

1. **CCI 常量**：`RAD_TLINEAR_ENABLE=0x4EC1`、模块基址、STATUS/COMMAND 地址来自
   FLIR Lepton Software IDD 记忆。**若 `tlin_err` 非 0 或 I2C 无 ACK，优先核对这几个
   十六进制值**（联网核对 FLIR SDK `LEP_RAD_ENABLE_TLINEAR` 与寄存器地址）。
2. **RESET/PWDN 极性**：假设两者均低有效。若释放后仍 boot=0，试把释放电平反相验证。
3. **1000ms 启动够不够**：个别模块更慢，`WaitBoot` 已给 2000ms 上限兜底。
4. **VSYNC(PB2) 方向**：本次只把它设为输入、未用作触发；后续做中断同步再启用。
5. **PLL2 未给 SPI/无关**：Lepton 不依赖 PLL2，SD 卡时钟树互不影响。

---

## 6. 上板测试步骤（复现流程）

1. **编译**：Keil `XIH6_V2.uvprojx`，确认 0 Error（`build-keil` skill）。
2. **烧录 + 串口**：USART1 @ **115200**（`serial-monitor` skill，`--wait-reset` 抓早期日志）。
3. **看启动行** `[LEP] ...`：
   - `mclk=1`：TIM1 PWM 已跑（可示波器量 PA8 ≈24MHz 佐证）；
   - `pwr=1 rst=1`：两控制脚已释放；
   - **`boot=1`**：CCI 读到 BootStatus=1 —— **说明 Lepton 真正活了**（本次首要目标）；
   - `status=0x....`：boot=0 时看这个值和 err 高字节定位；
   - `tlin_err=0`：辐射模式开启成功。
4. **看抓帧行** `[LEP] OK c_raw=.. c=..C`（约每 8s 一次）：
   - `c_raw` 在 ~30000 上下、`c` 接近室温（20~30℃）→ **VoSPI + 辐射标定通**；
   - 一直 `no frame` → 回第 3 步看 boot 是否成功，再查 MCLK/接线。

### 期望现象对照表

| 现象 | 判读 | 下一步 |
|---|---|---|
| `boot=0 status=0x0000` | CCI 完全无应答 | 查 PA8 MCLK、I2C4 上拉、3V3 供电、PJ3/PH6 是否真被释放 |
| `boot=0 status!=0` 有 err | 启动中/命令错 | 看 err 高字节；确认等待时间 |
| `boot=1 tlin_err!=0` | 活了但 RAD 命令错 | 核对 `0x4EC1`/保护位（第 5 节假设 1） |
| `boot=1` 但一直 `no frame` | CCI 通、VoSPI 不同步 | 查 SPI4 三线、CS(PE4)、Mode3；试降速/resync |
| `OK` 但温度离谱 | 标定/发射率 | 先确认非 AGC；再谈 emissivity/两点校正 |

---

## 7. 反馈占位（上板后填）

> 用户上板复测后把 `[LEP]` 串口输出贴这里，我据现象对照第 6 节表继续调。

```
（待填：启动行 + 抓帧行原文）
```

---

## 8. 第二轮：首次上板现象 + 定位（2026-07-04 20:38）

### 8.1 实测串口（`UART_OUTPUT.txt`）
```
[SD] no card in slot
[LEP] mclk=1 pwr=1 rst=1 boot=0(2002ms) status=0x0000 tlin_err=0
[LEP] boot FAIL ...
[LEP] no frame ...; resync   (x2)
```
现象补充：**插入 SD 卡后整机卡死**。

### 8.2 SDMMC 是否"重置后配错了"？——**没配错**
逐项核对：
- 时钟源 `SdmmcClockSelection=RCC_SDMMCCLKSOURCE_PLL2` → PLL2R；PLL2 = 64/32×200=400MHz，
  PLL2R=400/2=**200MHz**（与 README B.5 实测 `kerclk=200000000` 一致）。
- `ClockDiv=4` → 200/(2×4)=**25MHz**，识别阶段 HAL 自动再分频到 ~400kHz。**正确**。
- 引脚 CMD=PD7 / CK=PD6 / D0-3=PB14/PB15/PB3/PB4，`SDMMC2_IRQn` 已使能。**正确**。
- **注意**：`SD_Card.c` 里两处**过时注释**（说"DELAY.c 冻结 SysTick / HAL tick 冻结"、
  "kernel=PLL2P=100MHz"）是**误导**——实测 `DELAY.c` 是 DWT 实现、SysTick 完好
  （`boot=0(2002ms)` 这个计时就是 `HAL_GetTick` 正常走动的证据）。已订正注释。

**结论**：卡死不是时钟/引脚配错，也不是"SysTick 冻结导致 HAL_Delay 死等"。最可能是
`HAL_SD_Init()` 在**信号完整性差的 SD 总线**上阻塞（ACMD41 电压协商循环最多 0xFFFF 次、
每次带命令超时 → 观感像"卡死"）。这正是 README 附录 B 一直在查的硬件问题。

### 8.3 Lepton `status=0x0000` 的**歧义**已修
旧 `WaitBoot` 只在 I2C 读成功时才写 `status_reg`，所以 `0x0000` 既可能是"读成功=0"，
也可能是"每次读都 NACK、字段保持初值 0"——**两者根因完全不同**。本轮改动：
- 新增 `HAL_I2C_IsDeviceReady(0x2A)` 探测 → `i2c_rdy` 字段；
- 新增 `last_i2c_ok`（最后一次 STATUS 读是否 HAL_OK）；
- `tlin_err` 未执行时置 `-1`（旧的 `0` 是假象）。

新启动行格式：
```
[LEP] mclk=.. pwr=.. rst=.. i2c_rdy=.. boot=..(..ms) i2c_ok=.. status=0x.... tlin_err=..
```
判读：
| i2c_rdy | 含义 | 方向 |
|---|---|---|
| **0** | CCI 从机 0x2A **不 ACK** | I2C4 PD12/PD13 接线/上拉/3V3 供电、或 Lepton 没上电 |
| 1 但 boot=0 | I2C 通、但没启动 | 查 PA8 MCLK 是否真 24MHz、RESET_L(PJ3) 是否真释放、boot 时间 |
| 1 且 boot=1 | 活了 | 继续看抓帧/温度 |

### 8.4 本轮改动
- `SD_Card.c`：订正 SysTick/时钟过时注释（仅注释，无行为变化）。
- `lepton.h/.c`：诊断结构加 `i2c_ready/last_i2c_ok`；`Init` 加 I2C 探测；`tlin_err` 置 -1 兜底。
- `main.c`：
  - Lepton 启动行增加 `i2c_rdy/i2c_ok`，并按 `i2c_rdy` 给分级提示；
  - **SD 插卡加面包屑**：`[SD] insert detected -> init...` / `[SD] init call returned`
    —— 下次复测若只见前者不见后者，就**坐实卡死在 `HAL_SD_Init()`（总线/硬件）**。

### 8.5 下次复测重点看
1. 插卡后是否打印 `[SD] init call returned`：
   - **没有** → 死在 `HAL_SD_Init()` → 按附录 B.6 查 SD 总线上拉/接线（硬件）。
   - **有** → 卡死在别处，再定位。
2. Lepton 的 `i2c_rdy`：
   - `i2c_rdy=0` → **先解决 I2C**（PD12/PD13 上拉、0x2A、3V3）——这是没驱动成功的第一嫌疑。
   - `i2c_rdy=1 boot=0` → 用示波器量 PA8 是否 ~24MHz 方波；确认 PJ3 释放到高。

---

## 9. 第三轮：现象定位到硬件（2026-07-04 二次上板）

### 9.1 实测串口（`UART_OUTPUT.txt`）
```
[SD] no card in slot
[LEP] mclk=1 pwr=1 rst=1 i2c_rdy=0 boot=0(2002ms) i2c_ok=0 status=0x0000 tlin_err=-1
[LEP] CCI no ACK @0x2A ...
[LEP] no frame ...; resync
[SD] insert detected -> init...      <-- 到此为止，没有 "init call returned"
```
用户补充实测：
- PA8 示波器：24MHz，但**波形失真成正弦、幅度只有 1~1.5V**；
- **PJ3 一直是低**（RESET_L 没有真正被释放到高）；I2C 上拉正常；VSYNC 未测。

### 9.2 三个结论

**(A) SD 卡死 = 卡在 `HAL_SD_Init()` 内部 —— 实锤。**
`[SD] insert detected -> init...` 打印了，但 `[SD] init call returned` **没有**，
证明程序进了 `SD_Card_Init()` 却再没出来。结合 README 附录 B，这是
**SD 总线信号完整性/接线**问题（ACMD41/CMD 握手在坏总线上阻塞）。**非固件配置错误**。
→ 按 **B.6 硬件清单**查 CMD(PD7)/CK(PD6)/DAT0(PB14) 上拉与接线。

**(B) Lepton 没驱动成功的根因 = RESET_L(PJ3) 没抬起来。**
`i2c_rdy=0` 且 PJ3 实测一直低 → Lepton 被按在复位里 → 当然不 ACK I2C。
固件明明写了 `WritePin(PJ3, SET)`，实测却是低，只有两种可能：
1. **外部把 RESET 网络拉低**（短路/强下拉/Lepton 端异常）——硬件；
2. 引脚根本没驱动起来（但 STM32H743**XI** 是 TFBGA240，Port J 已封装引出，PJ3 存在）。
→ 本轮加了 **IDR 读回**（`rstIDR`/`pwdnIDR`）来判别：ODR 写 1 而 IDR 读 0 = 外部拉低=硬件。

**(C) PA8 MCLK 1.5V 正弦 = 信号完整性，非固件。**
固件里 PA8 已是 `AF1_TIM1` 推挽 + **VERY_HIGH** 最大驱动，24MHz 方波正常产生。
1.5V 正弦是**长杜邦线 + 探头电容**把 24MHz 方波滤成小正弦的典型现象。固件无法再"驱动更强"。
→ 缩短 PA8 到 Lepton 的线、就近走线；**若 Lepton 模块自带晶振则此路可无视**。
注意：这是**第二个**问题，即使解决也要先把 (B) 的 RESET 抬起来才有 I2C。

### 9.3 本轮改动（已编译验证）
- `lepton.h/.c`：诊断加 `rst_readback`/`pwdn_readback`（上电时序末尾用 `HAL_GPIO_ReadPin`
  读 PJ3/PH6 真实 pad 电平）。
- `main.c`：启动行加 `rstIDR/pwdnIDR`，并按 `rstIDR` 优先给出"RESET 被外部拉低=硬件"提示。
- **Keil 批量编译通过**：`UV4 -b` → **0 Error / 0 Warning**（Compiler V6.24）。

### 9.4 下次复测判读表
| rstIDR | i2c_rdy | 判读 | 动作 |
|---|---|---|---|
| **0** | 0 | ODR=1 但 pad=0 → RESET 网络被外部拉低 | 量 PJ3↔Lepton RESET 是否短地/接错/焊桥；断开 Lepton 单独量 MCU 侧 PJ3 是否能到 3.3V |
| 1 | 0 | RESET 已抬起但仍无 I2C | 多半是 MCLK 太弱(1.5V)→缩短 PA8 线；再查 PD12/13 上拉与 0x2A |
| 1 | 1 | I2C 通 | 看 boot / 抓帧 |

> 关键顺序：**先让 rstIDR=1（RESET 真正释放），再谈 MCLK 和 I2C**。

---

## 10. 第四轮：快门会动 → 问题重新定位（2026-07-04，多代理联网调研）

### 10.1 新现象（决定性）
- 串口：`rstIDR=1 pwdnIDR=1`（RESET/PWDN 都已确实释放）、`i2c_rdy=0`、快门无关行。
- 用户实测：**下载程序后 Lepton 快门（FFC 挡片）闭合再打开一次**，然后没动静。
- 插 SD 卡：**整板卡死，连 LED 都不闪了**。

### 10.2 联网调研结论（FLIR 数据手册/IDD + ST 论坛，5 代理已核对）

**快门动一次 = Lepton 确实启动成功**（ROM boot→固件加载→PLL 锁定→FPA 上电→首次 FFC）。
这条把问题**彻底重新定位**：启动/供电/时钟/核心都活着，
所以 **CCI 不 ACK 是"主机侧 / I/O 环"问题，不是启动或时钟问题**。
→ 之前担心的"MCLK 太弱(1.5V 正弦)"其实是**示波器探头+杜邦线的假象**（真1.5V正弦过不了
Lepton 的 VIH，根本启动不了）；"RESET 卡低"也已被 `rstIDR=1` 排除。

**CCI 不 ACK 的根因排序（按可能性）：**
1. **VDDIO（2.8V I/O 电源环）没供电，或 I2C 上拉接到了 3.3V。** ⭐最可疑
   快门用 1.2V 核心电，会动**不代表** 2.8V I/O 环通电。Lepton CCI 是 **2.8V CMOS
   （绝对最大 3.1V）**；**3.3V 上拉超压**、倒灌 ESD 钳位，导致 CCI pad 无法 ACK。
2. **SDA/SCL 接反**（PD12=SCL / PD13=SDA）——总线活着但地址永远 AF。
3. **过早访问 CCI 把 TWI 从机锁死**：boot 未满 ~950ms 就碰 I2C，会把接口锁进
   "拒绝一切 CCI、直到**冷插拔**才恢复（MCU 复位/重烧都救不回）"的状态。
   → 本项目上电时序**已等 1000ms**，本轮加到 **1200ms** 冗余；但若之前测试触发过锁死，
   **必须给 Lepton 断电冷启动一次**。
4. 主机外设配置（I2C4 时钟源/AF4/使能）——一般表现为 BERR/timeout 而非干净 AF。
5. 时序/时钟拉伸/16位寄存器——**排除**（IsDeviceReady 在地址 ACK 层就失败，够不到寄存器）。
   TIMINGR `0x307075B1` ≈ 100kHz，合规。

### 10.3 本轮固件改动（已编译 0/0）
- `lepton.c`：boot 等待 1000→**1200ms**（防过早访问锁死）。
- `lepton.h/.c`：诊断加 `i2c_err`（探测后 `hi2c4.ErrorCode`）、`i2c_scan_first/i2c_scan_count`
  （新函数 `Lepton_I2C_BusScan()` 扫 0x08..0x77）。0x2A 不 ACK 时自动全总线扫描。
- `main.c`：打印 `i2c_err=0x..` 和扫描结果，并给分级提示：
  - 扫描 0 个设备 → I2C 总线死（SDA/SCL 没接到 Lepton / 无上拉 / VDDIO 没电）；
  - 扫到别的地址而非 0x2A → **SDA/SCL 接反 或 模块引脚错**。
- **Keil `UV4 -b` 编译通过：0 Error / 0 Warning（ARMCLANG V6.24）**。

### 10.4 `i2c_err` 判读（下次复测看这个）
| i2c_err | 含义 | 指向 |
|---|---|---|
| `0x04` (AF) | 总线电气正常，没人应答 | VDDIO/上拉超压(#1) 或 SDA/SCL 反(#2) 或锁死(#3) |
| `0x01`(BERR)/`0x02`(ARLO) | 线冲突/上拉缺失/AF复用错 | 电气或主机配置(#4) |
| `0x20`(TIMEOUT) | 内核时钟没配/外设死 | 主机 I2C4 时钟(#4) |

### 10.5 SD 插卡卡死 = 硬件掉压（brownout），非软件死循环
调研：STM32H7 HAL 的 `HAL_SD_Init()` 全程有超时上限（~5s 后**返回错误**，不会永久卡死）。
**整板冻结（LED 都停）是插卡瞬间浪涌把 3.3V 拉垮、核心 brownout/复位**的特征
（实测热插拔可掉 0.3~0.6V；去耦不足直接让 MCU 复位）。
- **一句话确认**：在 `SysTick_Handler` 里翻转一个 GPIO，若插卡时它也停 → 是硬件复位而非代码循环。
- **硬件修**（按序）：SD 座 VDD 就近加 **2×10µF + 100nF**；再不行上**带缓启的负载开关**
  （TPS22918/AP22802），CD 去抖后再上电；**别用串联电感**。
- **固件缓解**：可选启用 **IWDG(~1-2s)** 让任何残留卡死自恢复；
  或 **带卡上电、开机只初始化一次**，把 SD 移出热插拔路径，先不挡 Lepton 调试。
  （本轮未擅自改 SD 行为——这是使用方式选择，等你定。）

### 10.6 你现在按这个顺序做（最快亮灯路径）
1. **给 Lepton 冷断电一次**（拔 VIN，不是按复位）——清掉可能的 CCI 锁死。
2. **万用表量 Lepton VDDIO 引脚 = 2.5~3.1V**；把 I2C 上拉从 3.3V 挪到 **2.8V VDDIO 轨**。
3. 烧新固件，看 `i2c_err` 和 busScan：
   - 还是全 AF、扫不到设备 → 做 **SDA/SCL 对调测试**（物理换 PD12↔PD13 或两种接法都试）。
   - 扫到非 0x2A 地址 → 坐实接反。
4. SD：先**带卡开机**或加去耦，别让它卡死挡 Lepton；brownout 确认用 SysTick GPIO 翻转法。


---

## 11. 第 4 轮（2026-07-04，看原理图 + 软件 bit-bang I2C）

### 11.1 读原理图（`转接板原理图.pdf` + `LEPTON.pdf`）的关键结论
- **Lepton 模块自带完整电源**：`U2=LM3670MF-1.2`→**1V2**（核心）、`U4=ADP150-2.8`→**2V8**（VDDIO）。
  转接板只给模块 `VCC`(3V3)，模块内部再降压出 1V2/2V8。
  → **"VDDIO 没供电"这个第 3 轮的头号嫌疑被推翻**：只要 3V3 到位、模块内部 2V8 就有；快门会动也印证内部电源在跑。
- **转接板上拉是 3V3**：`R1/R2=4.7K` 接 3V3（不是 2V8），到 PD12/PD13。
  经 4.7K 灌进 Lepton ESD 钳位的电流 ≈ (3.3−2.8−0.3)/4700 ≈ **42µA**，很小，多数 Lepton 直连 3V3 能忍——**未必致命**。

### 11.2 25MHz：数学上做不到（保留 24MHz）
TIM1 内核 = **240MHz**（`.ioc` 确认）。PWM = 240/(ARR+1)，ARR+1 须整数：
**240÷25 = 9.6 非整数**（240=2⁴·3·5 只含一个 5，25 需两个 5）。
所有能出精确 25MHz 的路都要动主时钟树：SYSCLK 500MHz 超上限 / HCLK 200MHz 会让 **USART1 乱码 + SPI4 变速 + DELAY.c 480MHz 校准全错**；
PA8=MCO1 除不尽 25；PLL2P÷4=25MHz 只能从 **MCO2=PC9** 出（非 PA8）。
→ **PA8 上无法在不破坏 UART/SPI/SD 的前提下得到精确 25MHz；快门会动已证 24MHz 足够启动，时钟不是 CCI 不通的根因。保留 24MHz。**

### 11.3 软件 bit-bang I2C（`Lepton_I2C_BitBang_Probe`）= 决定性判据
在 PD12/PD13 上纯软件打 I2C 时序（开漏模拟：拉低=输出低、释放=切输入靠上拉，~100kHz，含时钟拉伸等待），
**完全绕开 STM32 I2C4 外设**。当 HAL 探测失败时自动跑，串口打印 `[LEP] bitbang ack@0x2A=? scan_first=0x.. scan_cnt=..`。
判据：
- **bb_ack=1（bit-bang 拿到 ACK）** → 硬件/电平 OK，是 **I2C4 外设配置**的锅 → 切驱动到 bit-bang，CCI 当场可用。
- **bb_ack=0（还是没 ACK）** → **硬件/电平层**（3V3 上拉过压 / SDA-SCL 接反 / 接线）→ 停止纠缠配置。

已加：`lepton.h` 诊断字段 `bb_ack_2a/bb_scan_first/bb_scan_count` + API；`lepton.c` 完整 bit-bang 实现并接入 `Lepton_Init`；`main.c` 打印。**UV4 编译 0 Error / 0 Warning。**

### 11.4 你现在做（第 4 轮，决定性）
1. **给 Lepton 冷断电一次**（拔 VCC）清 CCI 锁死。
2. 烧新固件，读串口那行 **`bitbang ack@0x2A`**：
   - `=1` → 配置问题（你的直觉对），我把 CCI 切到 bit-bang。
   - `=0` → 硬件/电平，转查上拉挪 2V8 / SDA-SCL 对调。

---

---

## 12. 第 5 轮：供电实测正常 + bit-bang 也 0 设备 → 锁定 SDA/SCL 信号路径

### 12.1 决定性证据（UART_OUTPUT_2.txt + 用户万用表实测）
```
[LEP] CCI no ACK @0x2A: err=0x20 isr=0x1 SCLidle=1 SDAidle=1 scan_cnt=0
[LEP] bit-bang I2C (bypasses I2C4 periph): ack@0x2A=0 scan_first=0xFF scan_cnt=0
[LEP] >>> bit-bang also 0 devices -> HARDWARE/level, NOT config
```
**用户实测 Lepton 座子电压：1.2V 和 2.8V 均正常。**

### 12.2 排除法（这一轮把范围缩到唯一一类）
| 嫌疑 | 状态 | 依据 |
|---|---|---|
| 电源（1V2/2V8） | ❌ 排除 | 用户万用表实测两轨正常 |
| I2C4 外设配置 | ❌ 排除 | **bit-bang 绕开整个 I2C4 外设，结果一样 0 设备** |
| 上拉缺失/线被拉死 | ❌ 排除 | `SCLidle=1 SDAidle=1` 两线空闲高 |
| MCU 时钟/外设 | ❌ 排除 | bit-bang 纯 GPIO 打时序照样 0 ACK |
| **SDA/SCL 信号路径** | ✅ **唯一剩下** | 接反 或 断路/虚焊 |

> 关键澄清：`SCLidle=1` 只证明 **STM32 这侧**（转接板上拉）是高，**不能证明信号真的到达 Lepton 座子**。FPC 虚焊/断线时，MCU 侧靠自己上拉仍读高，但 Lepton 根本收不到。

### 12.3 为什么"电压正常却不通信"
电源脚（VDD/VDDIO）和信号脚（SCL/SDA）是**完全独立**的引脚。量到 1.2V/2.8V 正常只证明**芯片活着、快门能动**；I2C 通信要靠 SCL/SDA 两根信号线**正确连通且不接反**。芯片供电再好，这两根线接反或断了，主机发的地址 Lepton 永远收不到 → 不 ACK。

### 12.4 本轮固件：bit-bang 引脚角色对调测试（免示波器、免重新接线）
`Lepton_I2C_BitBang_Probe()` 现在一次上电跑**两遍**扫描：
- **Pass 1 正常**：SCL=PD12, SDA=PD13
- **Pass 2 对调**：SCL=PD13, SDA=PD12

引脚角色改为运行时变量（`bb_scl_pin`/`bb_sda_pin`），新增 `bb_scan_pass()` helper；诊断字段 `bb_swap_ack/bb_swap_first/bb_swap_count`；main.c 打印两遍结果。**UV4 编译 0 Error / 0 Warning。**

### 12.5 你现在做（第 5 轮，决定性）
1. 冷断电 Lepton → 烧新固件 → 读串口两行 bit-bang：
   - **对调 pass `bb_swap_ack=1` 或 `bb_swap_cnt>0`** → **坐实 SDA/SCL 接反**！只需在焊盘/FPC 上把 PD12↔PD13 两根线对调，不用示波器。
   - **两遍都 0 设备** → 不是接反，是**断路/虚焊/FPC 没插到位** → 用万用表量 STM32 PD12/PD13 焊盘 ↔ Lepton 座子 SCL/SDA 脚的**通断**（导通阻值应 <1Ω）。
2. 通断确认后若仍不通，最后才怀疑 Lepton 模块本身 CCI 损坏。

---

## 附：改动文件

- `Drivers/PER/LEPTON/lepton.h` — CCI 寄存器/命令宏、诊断结构、API 扩展（+bit-bang 字段/声明 +bb_swap_* 对调字段）
- `Drivers/PER/LEPTON/lepton.c` — 控制脚接管、上电时序、CCI 命令层、TLinear、resync、bit-bang（+运行时引脚对调 +探测后复位 I2C4）
- `Core/Src/main.c` — 启动诊断打印、抓帧温度打印、resync 调用、打印缓冲修正、bit-bang 对调结果打印
