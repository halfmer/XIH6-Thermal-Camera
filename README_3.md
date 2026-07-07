# README_3 - Lepton 3.5 驱动配置与时序排查记录

> 接续 `README_2.md`。本轮基于用户反馈：硬件连通性已逐线确认，SPI/I2C 引脚选择认为无误，因此重点从
> **STM32H743 外设配置、CCI 时序、VoSPI 片选/速率、诊断路径是否过早下结论** 重新审查。
>
> 日期：2026-07-05

---

## 1. 本轮参考的 skill

| Skill | 用途 |
|---|---|
| `aemb-peripheral-driver` | 按外设 BSP 驱动方式排查 Lepton：先确认总线、handle、设备地址，再做分层适配 |
| `stm32-freertos-developer` | 只取 STM32 HAL 外设集成参考，不引入 FreeRTOS API；重点看 I2C/SPI/TIM/GPIO |
| `keil` / `build-keil` | 检查 Keil 工程纳入关系，并用 UV4 命令行编译验证 |

本工程仍是裸机 super-loop，不是 FreeRTOS 工程。

---

## 2. 已确认的软件事实

- Keil 工程已纳入 `Drivers/PER/LEPTON/lepton.c/.h`。
- Keil 工程已纳入 HAL I2C/SPI/TIM 源文件，`DELAY.c` 也在工程内。
- CubeMX 生成配置：
  - CCI：`I2C4`，`PD12=SCL`，`PD13=SDA`，7-bit 地址仍按 `0x2A`。
  - VoSPI：`SPI4`，`PE2=SCK`，`PE5=MISO`，`PE6=MOSI`，`PE4` 被驱动改成软件 CS。
  - MCLK：当前已回到 `PA8=TIM1_CH1` 约 24MHz；`PA8=MCO1` 输出 25.000MHz 只作为 `UART_OUTPUT_6` 的失败实验记录，不能再当当前基线。
- `HAL_SPI_Receive()` 在 STM32H7 HAL 的 2-line master RX 路径会启动 SPI 时钟，阻塞读包方案本身可用。
- 当前无法从代码侧证明硬件坏；因此本轮不再把 “bit-bang 0 设备” 直接写死为断线，而是保留为“不是 I2C4 外设本身”的证据。
- 用户观察到 Lepton 3.5 上电后有快门/挡片闭合再打开的现象，且与官方 OpenMV4 板表现接近。这个现象强烈说明 Lepton core、电源、复位释放、MCLK/内部固件至少已进入运行流程；它不能证明 MCU 的 CCI/VoSPI 已经完全通，但会推翻“模块一定没启动”的简单判断。
- `UART_OUTPUT_4.txt` 中启动行仍是旧格式：`boot=0(...) i2c_ok=0`，没有当前固件应有的 `cci=HAL/BB` 字段。因此这份日志很可能不是烧录本轮修改后的固件，不能直接拿它验证当前代码。

---

## 3. 本轮发现的软件风险点

### 3.1 SPI4 引脚速度不合理

`MX_SPI4_Init()` 生成的 PE2/PE5/PE6 GPIO speed 是 `GPIO_SPEED_FREQ_LOW`，但 VoSPI 原先按 15MHz 读包。
这会导致一种很典型的现象：万用表/通断测试都正常，但实际 SPI 边沿质量差，包头解析一直失败。

### 3.2 CCI 只有诊断 bit-bang，没有备用传输

旧代码能用软件 bit-bang 去探测 `0x2A`。如果出现 `HAL I2C4 不 ACK，bit-bang ACK`，旧驱动仍然继续用 HAL I2C4 去读 STATUS。
也就是说，诊断已经指出“配置可能有问题”，但驱动没有顺势切换到可工作的路径。

### 3.3 I2C 缺少总线恢复

Lepton CCI/TWI 对上电后的访问时序敏感；如果上一轮测试在从机半字节状态、过早访问、MCU 复位但 Lepton 未冷断电等情况下中断，SDA/SCL 可能处在非预期状态。
本轮加入 9 个 SCL 脉冲 + STOP 的 I2C 总线恢复，先清状态再做 HAL 地址探测。

### 3.4 VoSPI 片选边沿没有留量

旧代码 CS 拉低后立刻启动 SPI 时钟，读完立刻拉高。对长线/转接板 bring-up 来说，给 CS setup/hold 留 2us 更稳。

### 3.5 `boot=0` 的判断语义过强

旧串口输出里的 `boot=0 i2c_ok=0` 实际含义是“MCU 没有通过 CCI 读到 STATUS boot bit”，不是“Lepton core 没启动”。
如果 STATUS 寄存器根本没读成功，就不能把 boot bit 当成有效 0。结合用户看到快门动作，后续输出必须把它显示为 CCI boot unknown。

### 3.6 Lepton 3.x VoSPI segment 解析错误

当前代码把 packet 20 的 segment number 解析为：

```c
seg = (ID0 >> 1) & 0x07;
```

但本地参考驱动 `Lepton驱动参考/software/raspberrypi_video/LeptonThread.cpp` 对 Lepton 3.x 的处理是：

```c
segmentNumber = (result[j * PACKET_SIZE] >> 4) & 0x0f;
```

也就是 segment number 在 `ID[0]` 的高 4 bit，而不是 bit[3:1]。这个 bug 会导致一种很符合当前现象的失败：Lepton 已经输出 VoSPI 包，但代码一直判 segment 错误，最终只看到 `[LEP] no frame`。

---

## 4. 本轮代码改动

### 4.1 `Drivers/PER/LEPTON/lepton.c`

- 新增 `Lepton_SPI_GPIO_Config()`：
  - 强制 PE2/PE5/PE6 为 `AF5_SPI4`、`GPIO_SPEED_FREQ_VERY_HIGH`。
- `Lepton_SPI_Config()` 调整：
  - SPI mode 3 不变；
  - VoSPI 速率从 `/8 = 15MHz` 降到 `/16 = 7.5MHz`，先提高上板容错；
  - 补齐 H7 SPI 扩展字段，启用 `SPI_MASTER_KEEP_IO_STATE_ENABLE`。
- 新增 CCI 软件传输 fallback：
  - `Lepton_BB_Write_Mem()`
  - `Lepton_BB_Read_Mem()`
  - 公共 `Lepton_I2C_Write_Reg()` / `Lepton_I2C_Read_Reg()` 会根据 `lepton_cci_use_bitbang` 自动走 HAL 或 bit-bang。
- 新增 `Lepton_I2C_BusRecover()`：
  - 在 SCL/SDA idle-high 时，先发 9 个 SCL 脉冲和 STOP；
  - 随后完整 reset + `MX_I2C4_Init()`，避免 HAL I2C 残留错误状态。
- bit-bang 时钟从约 100kHz 放慢到约 50kHz，优先保证识别稳定。
- `Lepton_Init()` 新逻辑：
  - MCLK/RESET/PWDN 后做 line check；
  - 做 I2C bus recovery；
  - HAL I2C4 探测 `0x2A`；
  - HAL 不通时跑 bit-bang；
  - 如果 normal bit-bang 能 ACK `0x2A`，自动切换到软件 CCI 继续 `WaitBoot()` 和 TLinear。
- `Lepton_SPI_Read_Packet()`：
  - CS low 后延时 2us 再出 SCK；
  - 读完后延时 2us 再 CS high；
  - CS high 后再延时 2us。
- `Lepton_Capture_Frame()`：
  - 修正 Lepton 3.x packet 20 segment 解析：`(ID[0] >> 4) & 0x0F`；
  - 每次捕获失败都记录 VoSPI 诊断计数：读包数、有效包、discard 包、packet0、desync、bad segment、SPI 错误、最后 4 字节包头；
  - 保留 full-frame 成功条件：必须收齐 4 个 segment 后才返回 1。
  - 在 `UART_OUTPUT_5` 后进一步收紧 VoSPI 包头判定：只有 `ID[0]` 低 4 bit 为 0 且 packet number 小于 60 才算 valid；`0xF` 只作为 discard；其他包头记为 invalid。
  - desync、discard 插入到段中、bad segment 后参考开源驱动延时 1ms 再重新找 packet 0，避免在错误相位里高速循环。

### 4.2 `Drivers/PER/LEPTON/lepton.h`

- `Lepton_Diag_t` 增加 CCI transport 字段：

```c
uint8_t cci_transport;  /* 0=HAL I2C4, 1=software bit-bang CCI fallback */
```

- `Lepton_Diag_t` 增加 VoSPI 失败诊断字段：

```c
uint32_t vospi_reads;
uint32_t vospi_valid;
uint32_t vospi_discard;
uint32_t vospi_invalid;
uint32_t vospi_pkt0;
uint32_t vospi_desync;
uint32_t vospi_bad_seg;
uint32_t vospi_spi_err;
uint8_t  vospi_fail_reason; /* 0=none,1=guard,2=spi,3=desync,4=bad_seg,5=invalid */
uint8_t  vospi_last_expected;
uint8_t  vospi_last_id0, vospi_last_id1, vospi_last_crc0, vospi_last_crc1;
uint8_t  vospi_last_seg;
```

### 4.3 `Core/Src/main.c`

- Lepton 启动行改为区分 CCI boot 是否真的可读：

```text
[LEP] ... i2c_rdy=.. cci=HAL/BB cci_boot=0/1/? status_ok=.. status=.. tlin=..
```

- `cci_boot=?` 表示 STATUS 寄存器没读成功，不能据此说 Lepton core 没启动。
- 如果 CCI status unread，会额外提示：快门/FFC 动作仍然说明 Lepton core 很可能已经启动。
- 如果 bit-bang ACK 但 HAL 不 ACK，提示改为：

```text
using software CCI fallback; I2C4 timing/config remains suspect
```

- 如果 normal/swapped bit-bang 都 0 ACK，不再直接断言“开路”，改为提示继续看 CCI pad 电平/模块状态/时序。
- `[LEP] no frame` 后新增一行 VoSPI 诊断：

```text
[LEP] VoSPI diag reason=.. reads=.. valid=.. discard=.. invalid=.. pkt0=.. desync=.. badseg=.. spierr=.. exp=.. last=ID0 ID1 CRC0 CRC1 seg=..
```

---

## 5. Keil 编译验证

构建命令由 `build-keil` 脚本调用：

```text
E:\keil_v5\UV4\UV4.exe -b D:\stm32_project\XIH6_V3\MDK-ARM\XIH6_V2.uvprojx -t XIH6_V2
```

结果：

```text
编译成功
目标: XIH6_V2
errors=0 warnings=0
Flash 约 76.7 KB
RAM 约 52.9 KB
产物:
  MDK-ARM\XIH6_V2\XIH6_V2.axf
  MDK-ARM\XIH6_V2\XIH6_V2.hex
日志:
  .embeddedskills\build\XIH6_V2-XIH6_V2-build.log
```

---

## 6. 下一次上板看这几类输出

### 6.1 HAL 直接成功

```text
i2c_rdy=1 cci=HAL cci_boot=1 status_ok=1 status=0x.... tlin=0
```

判读：硬件 I2C4 + CCI 都通，下一步看 VoSPI 是否出 `[LEP] OK c_raw=...`。

### 6.2 HAL 不通，但 bit-bang 成功

```text
i2c_rdy=0 cci=BB cci_boot=1 status_ok=1 status=0x.... tlin=0
[LEP] >>> bit-bang ACKs @0x2A but HAL did NOT -> using software CCI fallback
```

判读：Lepton CCI 物理链路是通的，硬件 I2C4 的 timing/filter/状态机配置仍有问题；但当前固件会先用软件 CCI 继续推进，不会卡在识别阶段。

### 6.3 CCI 可读但 boot bit 为 0

```text
cci=BB cci_boot=0 status_ok=1 status=0x....
```

判读：地址层通，STATUS 可读，但 boot bit 没起来。重点看 MCLK 是否持续、RESET/PWDN 是否稳定、是否需要完整冷断电清 Lepton 内部状态。

### 6.4 CCI 不可读但快门动作存在

```text
cci_boot=? status_ok=0
[LEP] CCI status unread -> cci_boot is UNKNOWN; shutter/FFC motion still means the Lepton core likely booted
```

判读：不要把它解释为 Lepton 没启动。它只说明 MCU 没能通过 CCI 读到 STATUS。下一步看 VoSPI 诊断，如果能看到有效 packet/segment，则控制面和视频面要分开处理。

### 6.5 CCI 通了但 VoSPI 仍无帧

```text
cci_boot=1 tlin=0
[LEP] no frame (check MCLK/VoSPI); resync
[LEP] VoSPI diag reason=.. reads=.. valid=.. discard=.. invalid=.. pkt0=.. desync=.. badseg=.. spierr=.. exp=.. last=.. .. .. .. seg=..
```

判读：

- `reads=0` 或 `spierr>0`：先看 SPI4/HAL/CS。
- `discard` 很多但 `valid=0`：VoSPI 有响应但不同步，执行 resync 或检查 CS idle 时间。
- `invalid` 很多：SPI 采样相位/边沿质量/片选包边界仍可疑，或者状态机在错误相位读到了 payload 并误当包头。
- `valid>0 pkt0>0 badseg>0`：以前很可能就是 segment 解析错；本轮已修正为高 4 bit。
- `valid>0 desync>0`：SPI mode/速率/边沿质量/CS 包边界仍可疑。
- `last` 不是固定 `00 00 00 00` 或 `FF FF FF FF`：说明 MISO 上至少有真实数据，不能再按“完全没输出”处理。

---

## 7. UART_OUTPUT_5 判读

最新输出已更新到这一类：

```text
i2c_rdy=0 cci=HAL cci_boot=?(2002ms) status_ok=0
bit-bang I2C ack@0x2A=0
bit-bang SWAPPED ack@0x2A=0
VoSPI diag reason=1 reads=3000 valid=2643 discard=357 invalid=0 pkt0=62 desync=7 badseg=51 spierr=0 exp=20 last=00 26 56 7C seg=0x02
[LEP] OK c_raw=30274 c=29.59C
VoSPI diag reason=1 reads=3000 valid=2622 discard=378 invalid=0 pkt0=59 desync=15 badseg=38 spierr=0 exp=20 last=0F FF FF FF seg=0x00
```

结论：

- CCI 控制面仍不通：HAL I2C4、normal bit-bang、swapped bit-bang 都没有 ACK。由于 VoSPI 已经有数据，这不再支持“Lepton 没启动”，而是更像 CCI 两根线没有到 Lepton CCI pad、CCI pad 电平/电源域不对，或转接板命名和实际 CCI pad 不一致。
- VoSPI 视频面已经能完整成帧：`[LEP] OK c_raw=30274 c=29.59C` 证明 SPI4/MISO/CS/MCLK/分段组帧至少有一次完整成功。
- 后续仍出现 `badseg`/`desync`，说明当前轮询式 VoSPI 同步还不够稳，但这已经不是“完全驱动不起来”的状态。
- `invalid=0` 且 `last` 多次符合合法包头形态，说明上一轮收紧包头分类是有效的；剩余问题更像同步窗口、轮询节奏、segment 4 结束后的 resync 处理。
- CCI 和 VoSPI 已经可以明确分开：视频面可工作，控制面仍无 ACK。

本轮代码动作：

- 收紧包头分类，避免把 payload/乱相位数据当 valid packet。
- 段内遇到 discard、invalid、packet number mismatch、bad segment 后延时 1ms 再重找 packet0，参考 `raspberrypi_video/LeptonThread.cpp` 的 `usleep(1000)` 策略。
- 串口新增 `invalid` 和 `exp`，下一次能区分“包头本身非法”与“合法包但包号不连续”。
- Lepton CCI 的软件 I2C 改成 `Drivers/soft/SW_I2C` 同类方式：PD12/PD13 固定配置为开漏输出，写 1 释放总线，写 0 拉低总线，不再通过反复切输入模式来释放。

---

## 8. 当前调试结论

本轮没有继续假定硬件断路；按“硬件连通基本可信”的前提，已把最可能的软件/时序风险都收进驱动：

- I2C 上电后先恢复总线；
- HAL I2C4 不行时自动走 bit-bang CCI；
- bit-bang 由扫描升级为可读写 CCI 寄存器；
- VoSPI 降速并加强 GPIO 驱动；
- CS 时序留出 setup/hold。
- `boot=0` 的语义改成 `cci_boot=?/0/1`，避免把 CCI 未读到误判为 core 未启动；
- 修正 Lepton 3.x VoSPI segment 解析 bug；
- 增加 VoSPI 失败诊断，下一次日志可以判断 SPI 是否已经收到真实包头；
- 收紧 VoSPI 包头合法性判断，并在失步后延时重同步。
- CCI bit-bang 改成 `SW_I2C` 风格的开漏 GPIO 软件 I2C；若下一轮仍 `ack@0x2A=0`，就基本排除“硬件 I2C 外设配置不行”这条软件原因。

下一步需要烧录本次固件，把完整 `[LEP]` 启动行、bit-bang 两行、第一次 `[LEP] no frame/OK` 和紧随其后的 `[LEP] VoSPI diag/okdiag ...` 行贴回来。后续判断以 `cci=HAL/BB`、`cci_boot`、`status_ok`、`tlin`、`VoSPI diag/okdiag` 的 `mask/segs/dup/bad0/badx/invalid/desync/badseg/exp/last` 为准。

---

## 9. UART_OUTPUT_5 后续复判与本轮补强

用户新反馈认为可能是 “CCI 和 VoSPI 要同时对齐才 OK”，也可能是拿起、晃动、改变摄像头状态后偶发成功。结合当前串口，判断如下：

- 不是“完全没驱动成功”：日志里已经出现多次 `[LEP] OK c_raw=... c=...C`，说明 Lepton core、MCLK、RESET/PWDN、VoSPI 的 SPI4/MISO/CS 至少能完整组成帧。
- CCI 和 VoSPI 不要求同时 ACK 才能出图：CCI 是控制/状态通道，用来读 boot、设置 TLinear、AGC、FFC、VSYNC 等；VoSPI 是视频流通道。当前 CCI 不通会导致配置不可控，但不阻止默认 VoSPI 流输出。
- `HAL I2C4 no ACK` 加 `SW-OD I2C ack@0x2A=0` 加 swapped 也 0，已经基本排除“只是 I2C4 外设 timing 配错”。如果 PD12/PD13 空闲为高，下一步更应查 CCI pad 到模块的实际连线、转接板命名、VDDIO/pull-up 电压、FPC/连接器压接，而不是继续写第三套 I2C。
- 晃动/拿起后偶发成功更像 VoSPI 物理层边界变好：FPC 压接、跳线接触、GND 参考、SCK/MISO/CS 边沿质量或线长耦合。Lepton 没有普通相机意义上的“对焦”；看到挡片/快门动作更接近 FFC 或上电内部动作，不应解释成 CCI 必须同步。

本轮代码补强：

- 成功帧也打印 `VoSPI okdiag`，不再只有失败时有诊断。
- VoSPI 诊断新增：
- `mask=0x0F`：当前 `lepton_raw_frame` 已缓存过有效的 segment 1..4。
- `segs=a/b/c/d`：本次捕获调用里完整读到的 segment 1..4 次数。
  - `dup`：已经收过的 segment 又重复出现。
  - `bad0/badx`：packet 20 解析出的 segment id 为 0 或大于 4。
  - `wait`：寻找 packet0 时遇到 discard 后做长等待的次数。
- 这一轮曾把 VoSPI guard 从 3000 包放宽到 6000 包，并让首个 discard 等 25ms；但 `UART_OUTPUT_6` 证明该策略变差，第 10 节已撤回到 3000 包和 1ms 等待。

下一轮串口重点看：

```text
[LEP] OK c_raw=... c=...
[LEP] VoSPI okdiag reason=0 ... mask=0x0F segs=.../.../.../... dup=... bad0=... badx=... wait=...
```

判读：

- `okdiag mask=0x0F`：代码已有 1..4 四个有效 segment 缓存在帧缓冲，成功不是把单包误判成整帧。
- 失败时 `mask` 缺某些 bit：说明不是 SPI 完全坏，而是某些 segment 没凑齐。
- `bad0` 很高：更像 segment 同步/读包节奏/边沿质量问题。
- `dup` 很高且 `mask` 不满：说明反复拿到同一段，捕获窗口或物理层稳定性仍不好。
- `invalid` 明显升高：优先看 SPI mode/CS/SCK/MISO 信号质量。
- `spierr` 非 0：才回到 HAL SPI/外设层面查错误。

本轮 Keil 全量重编译验证：

```text
rebuild 成功，errors=0 warnings=0
HEX: MDK-ARM\XIH6_V2\XIH6_V2.hex
AXF: MDK-ARM\XIH6_V2\XIH6_V2.axf
```

--- 

## 10. 用户反馈后回退到最后有 OK 的基线

用户反馈：25MHz/MCO1 测试版比上一版更差，甚至一次 `[LEP] OK` 都没有；但 Lepton 上电后挡片/盖子仍会闭合再打开。

复判：

- 挡片/盖子动作仍然说明 Lepton core、供电、复位释放和基本 MCLK 条件足以让内部固件运行。
- 但它不能证明 VoSPI 当前读包窗口正确，也不能证明 CCI 已通。
- 25MHz/MCO1 实验没有改善，反而比 `_5` 的 24MHz/TIM1 基线差，因此不应继续沿着 25MHz 方向叠加修改。

本轮已回退行为相关改动：

- 主时钟树恢复：
  - `PLLN=60`
  - `PLLP=2`
  - `PLLQ=2`
  - `PLLFRACN=0`
  - SYSCLK/CPU 恢复 480MHz。
- PA8 MCLK 恢复为 `TIM1_CH1`：
  - TIM1 kernel = 240MHz；
  - `ARR=9`，`CCR1=5`；
  - 输出约 24MHz、50% duty。
- VoSPI 捕获恢复：
  - guard = 3000 包；
  - discard/失步后 1ms 重试；
  - 不再使用 6000 包和 25ms 首 discard 等待。

保留的诊断增强：

- 启动行仍打印 `mclk_hz/sys/pclk2`，当前基线应看到：

```text
mclk_hz=24000000 sys=480000000 pclk2=120000000
```

- VoSPI 行继续保留 `seen/mask/segs/dup/bad0/badx`，只用于判断问题，不再改变抓帧节奏。

下一轮上板目标：

- 先确认启动行是 `mclk_hz=24000000 sys=480000000 pclk2=120000000`。
- 如果重新出现 `[LEP] OK`，说明 25MHz/MCO1 方向确实更差，后面应固定在 24MHz/TIM1 基线上继续调 CS/SPI/同步。
- 如果仍无 OK，但 `seen` 出现 1/2/3/4，说明 packet20 段号有恢复，问题转为段内连续读包。
- 如果 `seen=0/0/0/0` 且 `bad0` 仍高，重点不是 CCI 和 VoSPI 同时成功，而是 VoSPI 包边界/SPI 相位/物理信号质量。

本轮 Keil 全量重编译验证：

```text
rebuild 成功，errors=0 warnings=0
HEX: MDK-ARM\XIH6_V2\XIH6_V2.hex
AXF: MDK-ARM\XIH6_V2\XIH6_V2.axf
```

---

## 11. 历史记录：UART_OUTPUT_6 复判与 25MHz MCLK 失败实验

`UART_OUTPUT_6.txt` 的关键变化：

```text
[LEP] no frame (check MCLK/VoSPI); resync
[LEP] VoSPI diag reason=1 reads=6000 valid=3653 discard=2347 invalid=0 pkt0=79 desync=26 badseg=52 spierr=0 mask=0x00 segs=0/0/0/0 dup=0 bad0=52 badx=0 wait=79 exp=20 ...
```

判读：

- `spierr=0`、`valid` 很多，说明 SPI4 仍然能读到有结构的 VoSPI 数据，不是完全没时钟或 MISO 悬空。
- `mask=0x00`、`segs=0/0/0/0`，说明没有任何完整 segment 被提交。
- `bad0` 很高，说明每次读到 packet 20 时，segment nibble 经常解析成 0；这比 `_5` 更差。
- `wait=76~80` 说明上一轮“首个 discard 等 25ms”的策略太重，可能错过后续窗口；这条已撤回。

本轮接受用户对 PA8/MCLK 的怀疑，做一个更干净的验证版：

- PA8 不再用 `TIM1_CH1 = 24MHz`。
- 主 PLL 改为：
  - HSI 64MHz / 4 = 16MHz；
  - `PLLN=56`、`PLLFRACN=2048`，VCO = 900MHz；
  - `PLLP=2`，CPU/SYSCLK = 450MHz；
  - `PLLQ=6`，PLL1Q = 150MHz。
- PA8 改为 `MCO1 = PLL1Q / 6 = 25.000MHz`。
- `DELAY.c` 不再写死 480MHz，改为用 `SystemCoreClock` 动态计算 DWT 延时。
- VoSPI guard 恢复到 3000 包；discard 等待恢复为 1ms，不再使用 25ms 长等待。
- 串口启动行新增：

```text
mclk_hz=25000000 sys=450000000 pclk2=112500000
```

下一轮上板先确认启动行是否出现这些值。如果不是，说明烧录的不是这版固件。

新的 VoSPI 行还会多 `seen=a/b/c/d`：

- `seen` 表示 packet 20 曾经解析到 segment 1..4，即使后续没完整读完；
- `segs` 表示完整 60 包 segment 成功提交；
- 如果 25MHz 后 `seen` 仍几乎全 0、`bad0` 仍高，重点回到 SPI 相位/CS 包边界/信号质量；
- 如果 `seen` 开始出现 1/2/3/4 但 `segs` 仍是 0，说明 MCLK 有改善但段内仍丢包；
- 如果 `mask=0x0F` 或重新出现 `OK`，说明 24MHz/25MHz 差异确实是关键变量之一。

本轮 Keil 全量重编译验证：

```text
rebuild 成功，errors=0 warnings=0
HEX: MDK-ARM\XIH6_V2\XIH6_V2.hex
AXF: MDK-ARM\XIH6_V2\XIH6_V2.axf
```

---

## 12. 本轮修正：VoSPI 从一次性收齐改为 segment 缓存组帧

用户反馈 25MHz/MCO1 测试版比上一版更差，且“挡片/盖子仍会正常闭合再打开”。复判后本轮不再继续改 MCLK：

- 25MHz/MCO1 已作为失败实验保留在第 11 节；
- 当前固件保持 `PA8=TIM1_CH1` 约 24MHz；
- SYSCLK/CPU 保持 480MHz；
- VoSPI `guard=3000`、discard/失步后 1ms 重试。

这次真正调整的是 VoSPI 组帧判断：

- 旧逻辑：一次 `Lepton_Capture_Frame()` 里必须凑齐 segment 1/2/3/4，失败后本次已完整读到的 segment 也丢掉。
- 新逻辑：完整读到任意一个 60 包 segment 后，立刻写入 `lepton_raw_frame` 对应 30 行，并用静态 `mask` 记住已缓存段。
- 当 segment 4 到来，且 `mask=0x0F` 时返回 `[LEP] OK`。
- 这个方式参考 `raspberrypi_video/LeptonThread.cpp` 的 shelf 思路：segment 1/2/3 先暂存，segment 4 到来后发布一帧。

新的串口判读：

- `mask=0x01/0x03/.../0x0F`：帧缓冲里已经缓存过哪些完整 segment，不再局限于本次调用。
- `segs=a/b/c/d`：本次调用实际完整读到的 segment 次数。
- `seen=a/b/c/d`：本次调用 packet20 曾经看到的 segment 号，即使后面没读完整。
- 如果 `seen` 有值但 `segs=0/0/0/0`，说明 packet20 后继续读到 59 包之前掉同步，重点看 SPI 速率/相位/CS 包间时序/线长。
- 如果 `segs` 开始出现非 0 但仍无 OK，说明缓存正在推进，重点看 segment 4 是否能稳定到来。

本轮 Keil 全量重编译验证：

```text
rebuild 成功，errors=0 warnings=0
HEX: MDK-ARM\XIH6_V2\XIH6_V2.hex
AXF: MDK-ARM\XIH6_V2\XIH6_V2.axf
flash_bytes=79624
ram_bytes=54216
```

下一次上板请先确认启动行包含：

```text
mclk_hz=24000000 sys=480000000 pclk2=120000000
```

如果仍然一次 OK 都没有，把第一次 3~5 组 `[LEP] VoSPI diag ... mask=... seen=... segs=... bad0=... exp=... last=...` 贴回来；这版日志能判断是“完整 segment 从未成功”还是“segment 有缓存但 segment 4 没到”。

---

## 13. 当前串口日志复判：segment 缓存方案有效

本次 `UART_OUTPUT_6.txt` 已确认烧录的是回退后的 24MHz/TIM1 基线：

```text
mclk_hz=24000000 sys=480000000 pclk2=120000000
```

启动阶段仍然显示 CCI 控制面不通：

```text
i2c_rdy=0 cci=HAL cci_boot=? status_ok=0
SW-OD I2C ack@0x2A=0
bit-bang SWAPPED ack@0x2A=0
```

这说明 CCI 问题仍未解决，但不影响本轮 VoSPI 结论。VoSPI 日志出现了关键变化：

```text
mask=0x09 ... segs=3/0/0/3
mask=0x0D ... segs=0/0/1/1
mask=0x0D ... segs=1/0/1/2
mask=0x0F ... segs=2/1/1/2
[LEP] OK c_raw=30416 c=31.01C
```

判读：

- `mask` 从 `0x09` 推进到 `0x0F`，证明跨调用缓存 segment 是有效的。
- 后续连续出现 `[LEP] OK`，说明 Lepton core、MCLK、SPI4、CS、VoSPI 分段解析已经可以稳定跑通到可用状态。
- `spierr=0`、`invalid=0`，说明主要问题不是 HAL SPI 报错，也不是 MISO 完全乱码。
- `bad0/desync/wait` 仍非 0，说明读包节奏和包边界还不完美，但已经不是“驱动不起来”。
- 连续两次 OK 有时 `c_raw` 相同，是因为当前中心点在 segment 3；若某次 OK 只更新了 segment 1/4 而没有更新 segment 3，中心像素会沿用上一次缓存。这是当前诊断版的预期现象，不代表温度计算坏。

当前结论：

- 这版是目前 known-good 基线，暂时不要再改 MCLK、主 PLL、CCI 或 VoSPI 大逻辑。
- 下一步如果要提高实时性/画面一致性，应该围绕“更频繁地抓 VoSPI、减少跨多秒缓存、降低 stale segment”做小步优化，而不是回到 25MHz/MCO1 或继续重写 I2C。
