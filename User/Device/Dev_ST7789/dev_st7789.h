/**
 * @file dev_st7789.h
 * @brief 设备层：ST7789 SPI 彩屏（240×320 / RGB565，支持四向旋转）
 * @author Ricardo
 * @date 2026-11-04
 *
 * @note 只回答"这块屏能做什么"，不回答"什么时候画什么"：
 *       开窗、写像素、整屏填色、开关背光在这里；画什么由上层决定。
 *
 *       像素数据一律按 **大端（高字节在前）** 发送，这是 ST7789 的硬件
 *       要求。LVGL 的 RGB565 在内存里是小端，接 LVGL 时用
 *       LV_COLOR_FORMAT_RGB565_SWAPPED，或在 flush 里逐字节交换，
 *       不要指望本层替你转——那会让每次刷屏多一遍拷贝。
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
 * @brief 默认面板实例：240×320 竖屏，旋转为 0，无显示偏移
 * @note 240×240 的方屏要把 height 改成 240、y_offset 改成 80；
 *       改法是在 dev_st7789.c 里复制一份初始化器，再 extern 一个新实例。
 */
extern const dev_st7789_t dev_st7789_0;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化面板：硬复位（若接了 RST）→ 软复位 → 上电时序 → 清屏
 * @param lcd 面板句柄，不能为 NULL
 * @retval 0: 初始化成功
 * @retval -1: 句柄或成员为空，或屏幕尚未初始化就被调用
 * @retval -2: SPI 传输失败（片选、总线或超时，具体原因在 BSP 层）
 * @note 必须在**调度器启动之后的线程里**调用：底层传输是
 *       "抢互斥量 + DMA + 任务通知"，调度器没跑时会被 BSP 判成 ERR_STATE。
 *       与 OLED 同理，不能放进 MX_FREERTOS_Init()。
 *       本函数内含十余处毫秒级阻塞延时（合计约 400 ms），仅供上电时调用一次。
 */
int Dev_ST7789_Init(const dev_st7789_t *lcd);

/**
 * @brief 设置显示方向
 * @param lcd      面板句柄，不能为 NULL
 * @param rotation 0/1/2/3 对应 0°/90°/180°/270°
 * @retval 0: 设置成功
 * @retval -1: rotation 超出 0~3
 * @retval -2: SPI 传输失败
 * @note 旋转只改扫描方向（MADCTL），不改分辨率；
 *       逻辑宽高由 Dev_ST7789_Width/Height 返回，二者会随旋转互换。
 */
int Dev_ST7789_SetRotation(const dev_st7789_t *lcd, uint8_t rotation);

/**
 * @brief 查询当前逻辑宽度（随旋转变化）
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑宽度（像素）
 * @retval 0: lcd 为 NULL，或尚未调用 Init
 */
uint16_t Dev_ST7789_Width(const dev_st7789_t *lcd);

/**
 * @brief 查询当前逻辑高度（随旋转变化）
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑高度（像素）
 * @retval 0: lcd 为 NULL，或尚未调用 Init
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
 * @retval -1: 参数越界（超出当前逻辑宽高）或坐标顺序颠倒
 * @retval -2: SPI 传输失败
 * @note 窗口设好之后，后续 Dev_ST7789_WritePixels() 会按行优先、
 *       从左到右从上到下的顺序填满该区域，写满后自动回到窗口起点。
 */
int Dev_ST7789_SetWindow(const dev_st7789_t *lcd, uint16_t x0, uint16_t y0,
                         uint16_t x1, uint16_t y1);

/**
 * @brief 向当前窗口写入像素数据
 * @param lcd  面板句柄，不能为 NULL
 * @param data 像素缓冲区，大端 RGB565，不能为 NULL
 * @param len  字节数，必须是 2 的整数倍
 * @retval 0: 写入成功
 * @retval -1: 参数非法（空指针、长度为 0 或奇数、未初始化）
 * @retval -2: SPI 传输失败
 * @note 调用前必须先用 Dev_ST7789_SetWindow() 指定区域。
 *       内部按 4 KB 分块发送，整屏 240×320 共 153 600 字节，分 38 块。
 *       片选在整帧期间保持有效，中途不会被其他传输打断。
 */
int Dev_ST7789_WritePixels(const dev_st7789_t *lcd, const uint8_t *data,
                           uint32_t len);

/**
 * @brief 用纯色填充当前窗口
 * @param lcd   面板句柄，不能为 NULL
 * @param color 颜色，RGB565（例如 0xF800 为红）
 * @param count 填充像素个数，通常是窗口宽度 × 高度
 * @retval 0: 填充成功
 * @retval -1: 参数非法（空指针或 count 为 0）
 * @retval -2: SPI 传输失败
 * @note 内部用 512 字节的静态图案缓冲反复发送，不额外申请内存；
 *       全部数据在同一个片选帧内发完。
 */
int Dev_ST7789_FillColor(const dev_st7789_t *lcd, uint16_t color,
                         uint32_t count);

/**
 * @brief 整屏填充纯色（等价于开全屏窗口 + FillColor）
 * @param lcd   面板句柄，不能为 NULL
 * @param color 颜色，RGB565
 * @retval 0: 填充成功
 * @retval -1: 参数非法（句柄为空或尚未初始化）
 * @retval -2: SPI 传输失败
 * @note 240×320 全屏在 10.5 Mbit/s 下约 120 ms，只适合上电清屏或切页。
 */
int Dev_ST7789_FillScreen(const dev_st7789_t *lcd, uint16_t color);

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
