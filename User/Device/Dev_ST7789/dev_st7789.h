/**
 * @file dev_st7789.h
 * @brief 设备层：ST7789 SPI 彩屏（172×320 / RGB565，支持四向旋转）
 * @author Ricardo
 * @date 2026-11-04
 *
 * @note 只回答"这块屏能做什么"，不回答"什么时候画什么"：
 *       开窗、写像素、重复写图案、开关背光在这里；画什么由上层决定。
 *       本层不含颜色语义：要填一块纯色，由上层构造一个像素图案再调
 *       Dev_ST7789_WritePattern()，而不是在这里开一个 FillColor()。
 *
 *       像素数据一律按 **大端（高字节在前）** 发送，这是 ST7789 的硬件
 *       要求。上层若持有小端 RGB565 缓冲，须在送屏前自行交换字节序
 *       （或在渲染阶段就产出大端像素），本层不做转换——那会让每次
 *       刷屏多一遍拷贝。本层不感知上层用的是哪套 GUI 库。
 *
 *       **单实例设计**：旋转档位、逻辑宽高、就绪标志都是模块级变量，
 *       只服务 dev_st7789_0 一块屏。挂第二块屏需要把这些状态搬进句柄
 *       或改成按实例索引的数组，不是多 extern 一个实例就能用的。
 *
 *       本层不建线程、不重试；除 Init 里的复位与上电时序外不做阻塞延时。
 */

#ifndef __Dev_ST7789_H__
#define __Dev_ST7789_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief ST7789 面板句柄（不透明类型，成员定义在 dev_st7789.c）
 */
typedef struct dev_st7789 dev_st7789_t;

/*==============================================================================
 * 外部声明
 *============================================================================*/
/**
 * @brief 默认面板实例：172×320 竖屏，旋转为 0，列偏移 34
 * @note 144×320 等其他窄屏要改 width 与 x_offset；改法是在 dev_st7789.c
 *       里复制一份初始化器，再 extern 一个新实例——但见文件头"单实例设计"：
 *       运行时状态是共享的，同时使用两个实例会互相覆盖。
 */
extern const dev_st7789_t dev_st7789_0;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化面板：硬复位（若接了 RST）→ 软复位 → 上电时序 → 开显示
 * @param lcd 面板句柄，不能为 NULL
 * @retval 0: 初始化成功
 * @retval -1: 句柄或成员为空、调度器尚未运行，或面板几何越出 ST7789
 *         显存范围（列方向 240、行方向 320）
 * @retval -2: SPI 传输失败（片选、总线或超时，具体原因在 BSP 层）
 * @note 必须在**调度器启动之后的线程里**调用：底层传输是
 *       "抢互斥量 + DMA + 任务通知"，调度器没跑时会被 BSP 判成 ERR_STATE。
 *       与 OLED 同理，不能放进 MX_FREERTOS_Init()。
 *       内部约 450 ms 的等待用 osDelay，会主动让出 CPU，期间低优先级
 *       任务照常运行。结束后**不清屏**：面板显存上电为随机值，第一帧
 *       由应用层自己刷，设备层不替上层决定画什么。
 */
int Dev_ST7789_Init(const dev_st7789_t *lcd);

/**
 * @brief 设置显示方向
 * @param lcd      面板句柄，不能为 NULL
 * @param rotation 0/1/2/3 对应 0°/90°/180°/270°
 * @retval 0: 设置成功
 * @retval -1: lcd 为 NULL，或 rotation 超出 0~3
 * @retval -2: SPI 传输失败
 * @note 旋转只改扫描方向（MADCTL），不改分辨率；
 *       逻辑宽高由 Dev_ST7789_Width/Height 返回，二者会随旋转互换。
 *       句柄的 bgr 为 1 时 MADCTL 会置 RGB/BGR 位：实测红蓝互换
 *       （设红色显示成蓝色）就是模组走线为 BGR，把该字段改成 1。
 */
int Dev_ST7789_SetRotation(const dev_st7789_t *lcd, uint8_t rotation);

/**
 * @brief 查询当前逻辑宽度（随旋转变化）
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑宽度（像素）
 * @retval 0: lcd 为 NULL；或既未 Init 也从未 SetRotation（两个设置宽高的
 *         入口都没走过）
 */
uint16_t Dev_ST7789_Width(const dev_st7789_t *lcd);

/**
 * @brief 查询当前逻辑高度（随旋转变化）
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑高度（像素）
 * @retval 0: lcd 为 NULL；或既未 Init 也从未 SetRotation
 */
uint16_t Dev_ST7789_Height(const dev_st7789_t *lcd);

/**
 * @brief 查询当前旋转档位
 * @param lcd 面板句柄，不能为 NULL
 * @retval 0~3: 当前旋转档位
 * @retval 0xFF: lcd 为 NULL
 * @note 供上层做布局适配（横屏/竖屏两套排版的切换依据）。
 */
uint8_t Dev_ST7789_GetRotation(const dev_st7789_t *lcd);

/**
 * @brief 设置写入窗口（列地址 + 行地址），包含首尾像素
 * @param lcd 面板句柄，不能为 NULL
 * @param x0  起始列（含），必须小于等于 x1
 * @param y0  起始行（含），必须小于等于 y1
 * @param x1  结束列（含）
 * @param y1  结束行（含）
 * @retval 0: 设置成功
 * @retval -1: 参数越界（超出当前逻辑宽高）、坐标顺序颠倒，或尚未 Init
 * @retval -2: SPI 传输失败
 * @note 窗口设好之后，后续 Dev_ST7789_WritePixels() 会按行优先、
 *       从左到右从上到下的顺序填满该区域，写满后自动回到窗口起点。
 *       本函数分两帧发 CASET、RASET，中间会放总线锁；窗口参数被面板
 *       锁存，单任务场景没有问题，多任务同时开窗才需要额外串行化。
 */
int Dev_ST7789_SetWindow(const dev_st7789_t *lcd, uint16_t x0, uint16_t y0,
                         uint16_t x1, uint16_t y1);

/**
 * @brief 向当前窗口写入一段连续像素
 * @param lcd  面板句柄，不能为 NULL
 * @param data 像素缓冲区，大端 RGB565，不能为 NULL
 * @param len  字节数，必须是 2 的整数倍
 * @retval 0: 写入成功
 * @retval -1: 参数非法（空指针、长度为 0 或奇数、未初始化）
 * @retval -2: 抢总线失败，或 SPI 传输失败
 * @note 把缓冲区里的 len 个字节原样写进当前窗口，语义里没有"重复"；
 *       要用小块图案填满大面积，改用 Dev_ST7789_WritePattern()。
 *       调用前必须先用 Dev_ST7789_SetWindow() 指定区域。
 *       内部按 4 KB 分块发送，整屏 172×320 共 110 080 字节，分 27 块。
 *       整帧独占 SPI2：帧首抢总线并拉 CS，帧尾放 CS 之后才放锁，所以
 *       分块之间不会被其它 SPI 设备插进来，CS 也不会中途抬起。
 */
int Dev_ST7789_WritePixels(const dev_st7789_t *lcd, const uint8_t *data,
                           uint32_t len);

/**
 * @brief 在当前窗口内重复写入同一段像素图案，直到写满指定字节数
 * @param lcd       面板句柄，不能为 NULL
 * @param pattern   图案缓冲区，大端 RGB565，不能为 NULL
 * @param pat_len   图案字节数，不能为 0，必须是 2 的倍数
 * @param total_len 要写满的总字节数，不能为 0，必须是 2 的倍数
 * @retval 0: 写入成功
 * @retval -1: 参数非法（空指针、长度为 0 或奇数、未初始化）
 * @retval -2: 抢总线失败，或 SPI 传输失败
 * @note 调用前必须先用 Dev_ST7789_SetWindow() 指定区域。整帧只发一次
 *       RAMWR，图案靠"继续送数据"重复，写指针不会复位——这正是它不能
 *       换成"循环调用 WritePixels"的原因：每次 WritePixels 都会重发
 *       RAMWR，写指针被拉回窗口起点，只有最后一块会留下。
 *       典型用途：整屏刷背景色（pattern 传 256 像素的颜色块，
 *       total_len 传 宽×高×2）、图标或网格平铺。
 *       图案越短，帧内 DMA 启动次数越多：512 B 图案刷 110 080 B 要启动
 *       215 次，换 4096 B 图案只要 27 次。图案越大越快，代价是 RAM。
 */
int Dev_ST7789_WritePattern(const dev_st7789_t *lcd, const uint8_t *pattern,
                            uint32_t pat_len, uint32_t total_len);

/**
 * @brief 控制背光
 * @param lcd 面板句柄，不能为 NULL
 * @param on  1 点亮，0 熄灭
 * @retval 0: 已处理（句柄未接背光脚时也返回 0，表示"无需控制"）
 * @retval -1: lcd 为 NULL
 * @note 句柄的 bl 成员为 NULL 时本函数是空操作，对应背光直接接 3.3V 的硬件。
 */
int Dev_ST7789_Backlight(const dev_st7789_t *lcd, uint8_t on);

#endif /* __Dev_ST7789_H__ */
