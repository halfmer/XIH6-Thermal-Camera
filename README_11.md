# README_11 - USART1 拉锯复盘、Step9D 手工恢复启动死机与根因分析

日期：2026-07-07

本文替代已删除的旧 README_11（USART1 1.5Mbps 最小变量测试）与旧 README_12（手工恢复 Step 9D），把这两轮调试过程、当前上板失败现象、项目架构分析和修改计划归纳到一起。

**当前状态：README_12 恢复版固件（MD5 `55B9AED814C7075E4CFBE0D8CF9588FA`）上板后 LED 从上电起完全不翻转、USART1 无任何输出、长时间无动静——启动链路彻底死机。本文 §5 给出根因分析，§6 为修改计划（待确认后执行）。**

## 1. 调试过程归纳（原 README_11/12 内容）

### 1.1 背景

README_10 结束时的状态：

- 时间轮版本（`AD67C918`）用户实测"还是不如上一版"，已回退。
- 继续回退掉 USART1 1.5M + DMA ring，恢复 USART1=115200 阻塞发送，产物 `B09E58BF2C447D1CB6D4D162E7E3FA18`。**这是最后一个用户确认可以跑的版本。**

### 1.2 原 README_11：USART1 单独升 1.5M（最小变量测试）—— 失败

只改一个变量：USART1 波特率 115200 → 1500000（阻塞发送不变、不加 DMA、不加中断）。

- 产物：`C2A3552E7519A8DAF5097C06DE5363D2`
- 用户实测反馈："退回去吧，不太行"
- 已回退到 `B09E58BF`（USART1=115200）。

结论：单独提高 USART1 波特率不成立。

### 1.3 原 README_12：手工恢复"时间轮前一版"（Step 9D）—— 上板死机

用户判断："已经不是串口波特率问题了，是我提出时间轮的前一个代码才是支持的，现在已经不对了"。

因此按 README_9/README_10 的文字记录，手工恢复 README_9 Step 9D 的全部行为：

- USART1 `1500000` + 8KB TX DMA ring（`DMA1_Stream0`）
- UART4 RAW16 DMA + Ping-Pong（`DMA1_Stream1`，不变）
- `raw=min/max span` 诊断 + `span < 16` 扁平帧门禁
- 主循环保持非时间轮结构

涉及文件：`Core/Src/main.c`、`Core/Src/usart.c`、`Core/Src/dma.c`、`Core/Src/stm32h7xx_it.c`、`Core/Inc/stm32h7xx_it.h`、`Drivers/PER/LEPTON/lepton_stream.c/.h`、`XIH6_V2.ioc`。

构建 0 Error / 0 Warning：

```text
HEX : MDK-ARM/XIH6_V2/XIH6_V2.hex (2026-07-07 08:40)
MD5 : 55B9AED814C7075E4CFBE0D8CF9588FA
Program Size: Code=85406 RO-data=9786 RW-data=40 ZI-data=177856
map : sd_uart_dma_ring=0x24013f20/8192, stream_buf=0x240184a0/76864
```

关键限制：**工程没有 git 快照，无法 checkout 回原 Step 9D 产物（`BDF8B0E4`），只能按 README 文字记述逐文件手工恢复，无法与原版源码 diff。**

### 1.4 上板实测：彻底死机

```text
LED   ：上电起完全不翻转
USART1：没有任何数据
现象  ：过了很久仍然没有任何动静
```

对比历史很重要：**原版 Step 9B/9D（`BDF8B0E4`）当时上板的反馈是"灯不翻转，串口没输出，像是卡程序；过了好一会才有串口数据出来"（README_9 §12.1）——同一族症状，只是原版最终能起来，手工恢复版彻底起不来。** 这说明 USART1 DMA ring 这套形态从原版起就带着启动期隐患，不是手工恢复"抄错了"才出现的新问题。

## 2. 项目架构分析

### 2.1 入口文件

| 层级 | 文件 | 说明 |
|---|---|---|
| 工程入口 | `MDK-ARM/XIH6_V2.uvprojx` | Keil MDK 工程，Target=XIH6_V2，ARMCLANG V6.24，标准 C 库（`useUlib=0`，非 microlib） |
| 复位入口 | `MDK-ARM/startup_stm32h743xx.s` | 向量表、Reset_Handler；**`Stack_Size EQU 0x400`（栈仅 1KB）、`Heap_Size EQU 0x200`** |
| 应用入口 | `Core/Src/main.c` → `main()` | 所有初始化与超级循环 |

### 2.2 启动链路（main() 执行顺序）

```text
MPU_Config → HAL_Init → SystemClock_Config(HSI→PLL1 480MHz) → PeriphCommonClock_Config
→ MX_GPIO_Init → MX_DMA_Init(DMA1时钟+Stream0 NVIC)
→ MX_USART1_UART_Init(1.5M; MspInit里配 hdma_usart1_tx=DMA1_Stream0 + USART1_IRQn)
→ MX_SDMMC2/I2C4/SPI4/ADC1/ADC2/LTDC/SPI5 → MX_UART4_Init(115200占位) → MX_UART5_Init
→ Lepton_Stream_Init(&huart4)   UART4 重配 1.5M+CR2.SWAP + DMA1_Stream1 + UART4_IRQn + Receive_IT('S'/'P')
→ BEEP 静音 → SW_I2C_Init → OLED_Init → OLED 三行显示
→ SD_UART_Print("It's mygo!!!!!")          ← 第一条串口日志（main.c:301）
→ [RST] 复位原因打印 → [STREAM] 提示打印
→ SD 卡检测/init/自检（有卡才跑）
→ Lepton_Init(&hspi4,&hi2c4)  释放PJ3/PH6→1200ms boot→CCI探测(硬件死,每次全跑约2~4s:
    HAL 0x2A探测→全地址扫描→bitbang双pass) → [LEP] 大段诊断打印(lbuf[320]+ib[200]+bb[192]+sw[160])
→ while(1) 超级循环:
    stream_active? → Lepton_Capture_Frame → Lepton_Stream_SendFrame(UART4 DMA) → 5s诊断 → LED_TURN(1)
    idle          → LED_TURN(250) → SD热插拔 → 每16轮Lepton空闲采集 → SHT40 → OLED
```

**LED 翻转只存在于 `while(1)` 里（`LED_TURN` = 亮 delay 灭 delay，DWT 忙等）。第一条串口输出在 main.c:301。**

### 2.3 模块依赖关系

```text
main.c ──┬─ Core/Src/*.c (CubeMX: gpio/dma/usart/sdmmc/i2c/spi/adc/ltdc)
         │    usart.c: huart1(1.5M)+hdma_usart1_tx(DMA1_Stream0)  huart4(占位)  huart5(ESP32预留)
         │    dma.c  : DMA1 时钟 + DMA1_Stream0_IRQn NVIC
         ├─ Drivers/PER/LEPTON/lepton.c        VoSPI采集(SPI4)+CCI(I2C4,硬件死)+MCLK(TIM1 PA8 24M)
         ├─ Drivers/PER/LEPTON/lepton_stream.c UART4流: hdma_uart4_tx(DMA1_Stream1)+ping-pong
         │      ★ HAL_UART_{RxCplt,TxCplt,Error}Callback 全工程唯一定义在此,
         │        USART1 的事件转发回 main.c 的 SD_UART_{TxCplt,Error}Callback
         ├─ main.c 内 SD_UART_Print()  8KB ring → HAL_UART_Transmit_DMA(&huart1)
         │        全部调试日志（含 lepton.c 经 extern 调用）的唯一出口
         ├─ Drivers/PER/: OLED(SW_I2C 软件I2C), SHT40, SD_Card(SDMMC2+DMA+DTCM bounce)
         └─ Drivers/other/: LED.c(LED_TURN), DELAY.c(DWT忙等, SysTick/HAL_GetTick 仍正常)
中断入口: stm32h7xx_it.c  DMA1_Stream0(USART1 TX)/DMA1_Stream1(UART4 TX)/USART1/UART4/SDMMC2/SysTick
          HardFault_Handler = PA15 快闪特征（区别于普通卡死）
上位机  : ESP_UART_Host/ESP_UART_Windows  Qt6, SerialWorker 线程 + FrameParser(AA55) + ThermalWidget
```

USART1 一条日志的完整中断链（DMA ring 形态特有）：

```text
SD_UART_Print(写ring) → Kick → HAL_UART_Transmit_DMA
→ [中断1] DMA1_Stream0 TC → HAL_DMA_IRQHandler → 使能 USART1 TCIE
→ [中断2] USART1 TC → HAL_UART_IRQHandler → HAL_UART_TxCpltCallback(lepton_stream.c)
→ 转发 SD_UART_TxCpltCallback(main.c) → 推进tail → Kick 下一段 → HAL_UART_Transmit_DMA ...
```

即**每一段日志产生 2 次中断，且回调链在中断上下文里直接发起下一次 DMA**。115200 阻塞形态完全没有这条链。

## 3. 死点范围推理

1. **串口零输出** → 程序没有活着到达 main.c:301（`It's mygo!!!!!`），或到达了但输出链路完全瘫痪；
2. **LED 零翻转** → 程序没有活着进入 `while(1)`（LED 翻转只在主循环）；
3. 二者同时成立 + 长时间无动静 → **死点在初始化链路（main() 开头 ~ 主循环之前），且是死循环级别**（Error_Handler / HardFault / 卡死），不是"慢"。
4. 启动链路里被本轮恢复触碰过的环节只有：`MX_DMA_Init`、`MX_USART1_UART_Init`（+MspInit）、`SD_UART_Print` DMA ring 化、回调分发。**其余环节（OLED/SD/Lepton_Init 等）与能跑的 `B09E58BF` 完全同源。**

静态审读结果：中断向量齐全（Stream0/Stream1/USART1/UART4 都有 handler 且 NVIC 已使能）、DMA 缓冲都在 AXI SRAM（DMA1 可达，无 DTCM 陷阱）、回调唯一性无冲突、初始化顺序正确（DMA 先于 UART）。**逐行逻辑上找不到"必死"的显式 bug——问题在更隐蔽的资源层。**

## 4. 根因分析：1KB 栈 + 标准库 sprintf + USART1 DMA 中断链叠加 = 栈溢出（头号嫌疑）

### 4.1 证据

```text
startup_stm32h743xx.s:32   Stack_Size EQU 0x400        ← 全工程栈只有 1024 字节
XIH6_V2.uvprojx            useUlib=0                    ← 标准 C 库, sprintf/snprintf 栈开销大(数百字节)
main.c LEP_StreamDiag_MaybePrint(): char msg[704]      ← 单个局部数组即占栈 69%
main.c Lepton 诊断块: lbuf[320] + 嵌套 ib[200] + bb[192] + sw[160]  ← 启动路径,最坏并存 ~870B
main.c LEP_PrintVoSPIDiag(): char vd[448]              ← idle 每16轮
```

启动到 Lepton 诊断打印时，主线程栈深 ≈ `main 帧 + lbuf(320) + ib(200) + bb(192) + sprintf 内部(标准库约 200~400B)` —— **本来就贴着 1KB 顶**。历史上 115200 阻塞版能活，属于"恰好没炸透"。

### 4.2 USART1 DMA 化为什么把它推下悬崖

- 阻塞形态：`sprintf` 跑的时候没有任何 USART1 中断；唯一常态中断是 SysTick（很浅）。
- DMA ring 形态：启动期日志密集（mygo/[RST]/[STREAM]/[SD]/[LEP] 一大串），ring 一段接一段发，**DMA1_Stream0 TC 和 USART1 TC 中断在整个启动期高频到来**；每次中断在当前主线程栈顶再压入 `硬件保存(≥32B) + HAL_UART_IRQHandler + 回调转发 + SD_UART_DMA_Kick + HAL_UART_Transmit_DMA ≈ 250~400B`。
- 当中断恰好落在主线程 sprintf 最深处 → **1KB 栈溢出**。栈位于 ZI 顶端（约 0x2402B6xx），向下越界直接改写 ZI 末尾的静态对象（具体踩到谁由链接布局决定）→ HAL 句柄/状态被写坏 → 死循环、HardFault 或行为错乱。

### 4.3 这个根因同时解释所有历史怪象

| 现象 | 解释 |
|---|---|
| 原版 9B 上板"灯不翻、串口没输出、过好一会才有数据"（README_9 §12.1） | 栈溢出踩坏数据但恰好没踩死要害，挣扎着起来了 |
| Step 9C 启动打点版更糟 | 打点又加了 Flush 等待和更多启动期日志 → 中断更密 |
| 回退到 115200 阻塞（`B09E58BF`）就正常 | 没有 USART1 中断链，栈叠加消失 |
| 手工恢复版（`55B9AED8`）彻底死 | 代码尺寸/链接布局与原版不同（Code 85406 vs 87174），栈溢出踩点不同，这次踩死了要害 |
| 每次"同样的代码"行为不一 | 栈溢出破坏的对象取决于布局与时序，天然不可复现 |

### 4.4 次级候选（若栈修复后仍死再查）

1. 手工恢复与原版 `BDF8B0E4` 存在无法 diff 的差异（无 git，无原版源码）——用二分法定位；
2. 用户侧观测条件：USART1 现为 1.5Mbps，串口助手若仍开 115200 会表现为"看似没数据/乱码"——复测时需确认；
3. HardFault 鉴别：本工程 HardFault_Handler 是 PA15 快闪（数十 Hz，肉眼近似"微亮常亮"），与全灭/全亮的普通卡死不同——复测时注意 LED 的具体形态。

## 5. 防复发约束（新增）

1. **栈红线**：1KB 栈配标准库 sprintf 属于隐性炸弹；任何 >128B 的日志缓冲不允许放栈上（改 static）。
2. **中断链红线**：在中断回调里发起下一段 DMA（Kick 链）会把整条 HAL 发送路径叠进任意时刻的主线程栈，评估栈余量时必须按"主线程最深 + 最深中断链"计算。
3. **版本红线**：没有 git 快照的"手工按文档恢复"不可信也不可 diff——本轮已当场吃亏。先建版本管理再动代码。

## 6. 修改计划（待确认，未动任何代码）

按"一次只改一个变量"推进，每步构建+烧录+git 提交：

### Step 0：建立版本管理（不动固件）

```text
git init + 提交当前全部源码（55B9AED8 对应的现场）
以后每一步一个 commit，可随时精确回退，不再手工按 README 恢复
```

### Step A：只改栈大小（决定性单变量验证）

```text
startup_stm32h743xx.s: Stack_Size 0x400 → 0x2000 (8KB;  AXI 512KB 只用了 ~178KB, 无压力)
其余零改动 → 编译 → 烧录
预期: 若启动恢复(LED 翻转+USART1@1.5M 出日志) → 栈溢出坐实, 保留大栈进入 Step B
      若仍死 → 栈嫌疑排除, 跳到 Step C 二分
```

### Step B：栈安全加固（Step A 成功后）

```text
main.c/lepton_stream.c 大局部数组改 static:
  msg[704], vd[448], lbuf[320], ib[200], bb[192], sw[160], rb[96], dbg[96], STRM msg[220]
（它们都只在主线程单一路径使用, static 无重入风险）
```

### Step C：二分回退（仅当 Step A 无效）

```text
C1: 只撤 USART1 DMA ring(恢复阻塞发送), 保持 1.5M → 烧录
    活了 → 根因在 DMA ring 链路; 死 → 继续
C2: 波特率回 115200(即回到已验证能跑的 B09E58BF 形态) → 烧录, 必活, 再单变量前进
```

### 复测观察清单（每次烧录后）

```text
1. LED 形态三分: 全灭/全亮不动=卡死或 Error_Handler; 微亮快闪=HardFault; 规律翻转=主循环活了
2. USART1 串口助手波特率必须与固件一致(当前 1500000)
3. 活了之后再看: [LEP] 诊断 → UART4 发 'S' → [STRM_DIAG] 的 flat 与全绿帧对应关系
```
