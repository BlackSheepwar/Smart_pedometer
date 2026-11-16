# 保留引脚清单（这些引脚不要占用）

> 最后更新：2026-11-16
> 背景：整机改为面包板搭建。**SD 卡的两种接法都已从工程里移除**——SPI 模式（片选 PB9）
> 与 SDIO 模式（PC8~PC12 + PD2），CubeMX 配置与代码里都不再出现。这些引脚空出来留给
> 后续功能，**不要分配给任何外设**，也不要拿来当普通 GPIO 用。

## 一、已释放、禁止占用的引脚

| 引脚 | 原来的用途（已移除） | 当前状态 | 备注 |
|---|---|---|---|
| **PB0** | LCD 的数据/命令选择（DC） | 空闲 | DC 已改到 **PB11**，PB0 与显示链路无关了 |
| **PB9** | SPI 模式 SD 卡片选 `SD_CS` | 空闲 | 原 `bsp_gpio_pb9` 句柄已删除 |
| **PC8** | SDIO_D0 | 空闲 | |
| **PC9** | SDIO_D1 | 空闲 | |
| **PC10** | SDIO_D2 | 空闲 | |
| **PC11** | SDIO_D3 | 空闲 | |
| **PC12** | SDIO_CK | 空闲 | |
| **PD2** | SDIO_CMD | 空闲 | |

这 8 个脚在 CubeMX 里**没有**任何配置，复位后是浮空输入，可以随便接线但**不要接强驱动源**
（浮空输入被外部强拉会有额外电流）。

## 二、正在使用、更不能动的引脚

| 引脚 | 用途 | 归属 |
|---|---|---|
| PA4 | 触摸片选 `TP_CS` | SPI1 |
| PA5 | SPI1_SCK（屏 + 触摸共用） | SPI1 |
| PA6 | SPI1_MISO（只接触摸） | SPI1 |
| PA7 | SPI1_MOSI（屏 + 触摸共用） | SPI1 |
| PA8 | 屏片选 `LCD_CS` | SPI1 |
| **PB11** | 屏 `DC`（数据/命令选择） | GPIO |
| PB12 | 屏 `RST`（硬复位） | GPIO |
| PC4 | 触摸 `IRQ`/PENIRQ（低有效，10K 上拉在模组上） | GPIO 输入 |
| PB2 | 板载 LED | GPIO |
| PC13 | 按键输入（下拉，按下接 3V3） | GPIO 输入 |
| PA9 / PA10 | USART1 TX/RX | USART1 |
| PA2 / PA3 | USART2 TX/RX | USART2 |
| PB13 / PB14 / PB15 | SPI2 SCK/MISO/MOSI | SPI2 |
| PA11 / PA12 | USB OTG FS DM/DP | USB |
| PA13 / PA14 | SWDIO / SWCLK | 调试口 |
| PH0 / PH1 | 外部晶振 | RCC |

## 三、改动时的规矩

1. **CubeMX 里不要重新分配第一节的 8 个引脚**。CubeMX 打开工程后如果看到它们处于
   "未分配"状态，是对的，不要手痒点上。
2. 确实要用其中某个脚时，按这个顺序做：
   ① 先更新本文件（说明新用途、谁在用）；
   ② 再在 `.ioc` 里分配；
   ③ 同步 `Src/gpio.c`（引脚电气配置）与 `User/BSP/BSP_GPIO/`（句柄）；
   ④ 如果涉及 DMA/中断，一并检查 `bsp_*` 层。
3. **每次用 CubeMX 重新生成代码后，检查三件事**（本次移除是否被还原）：
   - `Src/` 下不应再出现 `sdio.c`；
   - `Src/main.c` 的初始化序列里不应有 `MX_SDIO_MMC_Init()`；
   - `Inc/main.h` 里不应有 `SD_CS_Pin`。
   如果出现了，说明 `.ioc` 被改回去了，别直接编译——先修 `.ioc`。

## 四、SDIO 为什么必须移除（留个记录，免得以后有人又加回来）

`MX_SDIO_MMC_Init()` 位于 `main()` 里、`osKernelStart()` **之前**。面包板上没有 SD 卡时，
`HAL_MMC_Init()` 会真的去轮询卡片上线、约 1 秒后超时返回错误，而生成的代码在错误分支里调
`Error_Handler()` —— 那个函数是 `__disable_irq(); while(1);`。于是整台设备永久停在上电阶段：
屏幕不亮、按键无响应、触摸无响应，看起来像"板子根本没跑起来"，极难反查到"是 SD 卡没插"。

将来真要做 SD 卡功能，也必须满足：**无卡是可恢复的正常状态**，不能走 `Error_Handler()`。

## 五、本次移除动了哪些文件（便于审计/回滚）

| 文件 | 改动 |
|---|---|
| `Smart_pedometer.ioc` | 删除 SDIO 外设与 PC8~PC12、PD2、PB9；`Mcu.IPNb` 12→11、`Mcu.PinsNb` 33→26；SPI1 预分频改 8（10.5 MHz） |
| `Src/sdio.c`、`Inc/sdio.h` | 删除 |
| `Src/main.c` | 去掉 `MX_SDIO_MMC_Init();` 与 `#include "sdio.h"` |
| `Inc/main.h` | 去掉 `SD_CS_Pin` / `SD_CS_GPIO_Port` |
| `Src/gpio.c` | 去掉 `SD_CS_Pin` 的初值与初始化 |
| `Inc/stm32f4xx_hal_conf.h` | 关闭 `HAL_MMC_MODULE_ENABLED`（连同其 include） |
| `cmake/stm32cubemx/CMakeLists.txt` | 源文件列表去掉 `Src/sdio.c` |
| `User/BSP/BSP_GPIO/bsp_gpio.h`、`bsp_gpio.c` | 删除 `bsp_gpio_pb9` 句柄 |
