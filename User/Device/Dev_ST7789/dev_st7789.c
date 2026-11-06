/**
 * @file dev_st7789.c
 * @brief 设备层实现：ST7789 SPI 彩屏，开窗 + 突发写像素
 * @author Ricardo
 * @date 2026-11-04
 *
 * @note 本文件不含引脚号：SCK/MOSI/CS 属于 SPI2 总线与设备句柄（BSP 层），
 *       DC/RST/BL 只是三个 bsp_gpio_t 指针。换引脚只改本文件末尾的
 *       句柄初始化器，以及 gpio.c 里的电气配置。
 *
 *       一次写屏的时序：
 *         CS 低 → DC 低 → 发命令 → DC 高 → 发参数/像素 → CS 高
 *       片选在整帧期间保持有效，中途不做释放：ST7789 的串行接口在 CS
 *       上升沿会复位移位计数器，RAMWR 期间松片选可能导致写入提前结束。
 *       代价是同一根总线上的其它设备在整帧期间拿不到总线，本工程 SPI2
 *       只挂这一块屏，可以接受。
 *
 *       时间基准：Init 的复位/上电延时用 HAL_Delay（一次性、允许阻塞）；
 *       传输超时交给 BSP 的 timeout_ms，本层不自己计数。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include "dev_st7789.h"
// 功能包含
#include "bsp_spi.h"
#include "bsp_gpio.h"
#include "stm32f4xx_hal.h"     /* 仅用于 Init 期间的 HAL_Delay */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单次 SPI 传输的最大字节数：len 是 uint16_t，且 DMA 计数器上限 65535 */
#define DEV_ST7789_CHUNK            4096u
/** @brief 纯色填充的图案缓冲大小（字节），必须是偶数 */
#define DEV_ST7789_FILL_BUF_SIZE    512u

/** @brief 软件复位 */
#define DEV_ST7789_CMD_SWRESET      0x01u
/** @brief 退出睡眠 */
#define DEV_ST7789_CMD_SLPOUT       0x11u
/** @brief 正片显示（IPS 面板常需要） */
#define DEV_ST7789_CMD_INVON        0x21u
/** @brief 开显示 */
#define DEV_ST7789_CMD_DISPON       0x29u
/** @brief 列地址设置 */
#define DEV_ST7789_CMD_CASET        0x2Au
/** @brief 行地址设置 */
#define DEV_ST7789_CMD_RASET        0x2Bu
/** @brief 写显存 */
#define DEV_ST7789_CMD_RAMWR        0x2Cu
/** @brief 扫描方向 */
#define DEV_ST7789_CMD_MADCTL       0x36u
/** @brief 像素格式 */
#define DEV_ST7789_CMD_COLMOD       0x3Au
/** @brief 正常显示模式 */
#define DEV_ST7789_CMD_NORON        0x13u

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief ST7789 面板句柄
 * @note 只放配置：总线、片选、三个控制脚、面板原生尺寸与偏移。
 *       运行时状态（当前旋转、逻辑宽高）不进句柄，放在下面的 static 里。
 */
struct dev_st7789 {
    bsp_spi_t           *bus;       /* 所属 SPI 总线，负责搬字节 */
    const bsp_spi_dev_t *dev;       /* 片选设备句柄，只负责 CS */
    const bsp_gpio_t    *dc;        /* 数据/命令选择：0 命令，1 数据 */
    const bsp_gpio_t    *rst;       /* 硬复位，NULL 表示接板级复位 */
    const bsp_gpio_t    *bl;        /* 背光，NULL 表示常亮 */
    uint16_t             x_offset;  /* 面板列方向显示偏移 */
    uint16_t             y_offset;  /* 面板行方向显示偏移 */
    uint16_t             width;     /* 面板原生宽度（未旋转） */
    uint16_t             height;    /* 面板原生高度（未旋转） */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/**
 * @brief 默认面板实例
 * @note 172×320 竖屏（1.47 寸常见规格）；DC 用 PB0，RST 用 PB12，背光不控制。
 *       x_offset = 34 来自"ST7789 的显存是 240 列、面板只取中间 172 列"这一
 *       常见接法；若实测画面左边（竖屏时）出现 34 像素黑边、右侧被切掉，
 *       说明模组从第 0 列开始映射，把 x_offset 改 0 即可。
 */
const dev_st7789_t dev_st7789_0 = {
    .bus      = &bsp_spi2,
    .dev      = &bsp_spi2_dev_lcd,
    .dc       = &bsp_gpio_pb0,
    .rst      = &bsp_gpio_pb12,
    .bl       = NULL,
    .x_offset = 34u,
    .y_offset = 0u,
    .width    = 172u,
    .height   = 320u,
};

/** @brief 当前逻辑宽度（随旋转变化），Init 之前为 0 */
static uint16_t s_width;
/** @brief 当前逻辑高度（随旋转变化），Init 之前为 0 */
static uint16_t s_height;
/** @brief 当前旋转档位 0~3 */
static uint8_t  s_rotation;
/** @brief 1 表示 Init 已完成，未完成时拒绝开窗与写像素 */
static uint8_t  s_ready;
/** @brief 纯色填充用的图案缓冲，放 .bss，避免占任务栈 */
static uint8_t  s_fill_buf[DEV_ST7789_FILL_BUF_SIZE];

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 在当前片选帧内发送一段字节（不碰片选与 DC）
 * @param lcd  面板句柄，调用者保证非 NULL
 * @param data 待发送缓冲区，不能为 NULL
 * @param len  字节数，不能为 0
 * @retval 0: 全部发完
 * @retval -2: 其中一次传输失败
 * @note 按 DEV_ST7789_CHUNK 分块，因为 BSP 的 len 是 uint16_t。
 *       片选由调用者在帧首尾负责，本函数只管搬字节。
 */
static int Dev_ST7789_Raw(const dev_st7789_t *lcd, const uint8_t *data,
                          uint32_t len)
{
    while (len > 0u) {
        uint16_t chunk = (len > DEV_ST7789_CHUNK) ?
                         (uint16_t)DEV_ST7789_CHUNK : (uint16_t)len;

        if (BSP_SPI_Transfer(lcd->bus, data, NULL, chunk, 0u) != BSP_SPI_OK) {
            return -2;
        }

        data += chunk;
        len  -= chunk;
    }

    return 0;
}

/**
 * @brief 发送一条命令，可选带参数（整帧独占片选）
 * @param lcd       面板句柄，调用者保证非 NULL
 * @param cmd       命令字
 * @param param     参数缓冲区，NULL 表示无参数
 * @param param_len 参数字节数，0 表示无参数
 * @retval 0: 成功
 * @retval -2: 传输失败
 * @note 命令与参数在同一个片选帧内完成，避免 CS 上升沿打断参数序列。
 */
static int Dev_ST7789_Cmd(const dev_st7789_t *lcd, uint8_t cmd,
                          const uint8_t *param, uint16_t param_len)
{
    int status;

    BSP_SPI_Dev_Select(lcd->dev);
    BSP_GPIO_Write(lcd->dc, 0u);
    status = Dev_ST7789_Raw(lcd, &cmd, 1u);

    if ((status == 0) && (param != NULL) && (param_len > 0u)) {
        BSP_GPIO_Write(lcd->dc, 1u);
        status = Dev_ST7789_Raw(lcd, param, param_len);
    }

    BSP_SPI_Dev_Deselect(lcd->dev);
    return status;
}

/**
 * @brief 发送 RAMWR 并在同一帧内写像素
 * @param lcd 面板句柄，调用者保证非 NULL
 * @param pix 像素缓冲区，不能为 NULL
 * @param len 字节数，不能为 0
 * @retval 0: 成功
 * @retval -2: 传输失败
 * @note 把 "0x2C + 数据" 合成一帧，是写屏的主路径。
 */
static int Dev_ST7789_Burst(const dev_st7789_t *lcd, const uint8_t *pix,
                            uint32_t len)
{
    uint8_t cmd = (uint8_t)DEV_ST7789_CMD_RAMWR;
    int status;

    BSP_SPI_Dev_Select(lcd->dev);
    BSP_GPIO_Write(lcd->dc, 0u);
    status = Dev_ST7789_Raw(lcd, &cmd, 1u);

    if (status == 0) {
        BSP_GPIO_Write(lcd->dc, 1u);
        status = Dev_ST7789_Raw(lcd, pix, len);
    }

    BSP_SPI_Dev_Deselect(lcd->dev);
    return status;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化面板：硬复位 → 软复位 → 上电时序 → 清屏
 * @param lcd 面板句柄，不能为 NULL
 * @retval 0: 初始化成功
 * @retval -1: 句柄或成员为空
 * @retval -2: SPI 传输失败
 * @note 必须在调度器启动后的线程里调用。上电时序取自 ST7789V 常见模组
 *       配置（240×320 IPS），若屏幕颜色发灰或出现残影，优先调
 *       PORCTRL / VCOMS / 伽马三段，其余项一般不用动。
 */
int Dev_ST7789_Init(const dev_st7789_t *lcd)
{
    static const uint8_t porctrl[]  = { 0x0Cu, 0x0Cu, 0x00u, 0x33u, 0x33u };
    static const uint8_t gctrl[]    = { 0x35u };
    static const uint8_t vcoms[]    = { 0x19u };
    static const uint8_t lcmctrl[]  = { 0x2Cu };
    static const uint8_t vdvrvhen[] = { 0x01u };
    static const uint8_t vrhs[]     = { 0x12u };
    static const uint8_t vdvs[]     = { 0x20u };
    static const uint8_t frctrl2[]  = { 0x0Fu };
    static const uint8_t pwctrl1[]  = { 0xA4u, 0xA1u };
    static const uint8_t colmod16[] = { 0x55u };   /* 16 bit/pixel */
    static const uint8_t pvgam[]    = { 0xD0u, 0x04u, 0x0Du, 0x11u, 0x13u,
                                        0x2Bu, 0x3Fu, 0x54u, 0x4Cu, 0x18u,
                                        0x0Du, 0x0Bu, 0x1Fu, 0x23u };
    static const uint8_t nvgam[]    = { 0xD0u, 0x04u, 0x0Cu, 0x11u, 0x13u,
                                        0x2Cu, 0x3Fu, 0x44u, 0x51u, 0x2Fu,
                                        0x1Fu, 0x1Fu, 0x20u, 0x23u };

    if ((lcd == NULL) || (lcd->bus == NULL) ||
        (lcd->dev == NULL) || (lcd->dc == NULL)) {
        return -1;
    }

    /* ---------- 1. 硬复位：RST 低 20 ms 再拉高，等待内部复位完成 ---------- */
    if (lcd->rst != NULL) {
        BSP_GPIO_Write(lcd->rst, 1u);
        HAL_Delay(5u);
        BSP_GPIO_Write(lcd->rst, 0u);
        HAL_Delay(20u);
        BSP_GPIO_Write(lcd->rst, 1u);
        HAL_Delay(120u);
    }

    /* ---------- 2. 软复位与退出睡眠 ---------- */
    (void)Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_SWRESET, NULL, 0u);
    HAL_Delay(150u);
    (void)Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_SLPOUT, NULL, 0u);
    HAL_Delay(120u);

    /* ---------- 3. 像素格式：16 bit RGB565 ---------- */
    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_COLMOD,
                       colmod16, (uint16_t)sizeof(colmod16)) != 0) {
        return -2;
    }

    /* ---------- 4. 时序、电源与帧率 ---------- */
    (void)Dev_ST7789_Cmd(lcd, 0xB2u, porctrl, (uint16_t)sizeof(porctrl));
    (void)Dev_ST7789_Cmd(lcd, 0xB7u, gctrl, (uint16_t)sizeof(gctrl));
    (void)Dev_ST7789_Cmd(lcd, 0xBBu, vcoms, (uint16_t)sizeof(vcoms));
    (void)Dev_ST7789_Cmd(lcd, 0xC0u, lcmctrl, (uint16_t)sizeof(lcmctrl));
    (void)Dev_ST7789_Cmd(lcd, 0xC2u, vdvrvhen, (uint16_t)sizeof(vdvrvhen));
    (void)Dev_ST7789_Cmd(lcd, 0xC3u, vrhs, (uint16_t)sizeof(vrhs));
    (void)Dev_ST7789_Cmd(lcd, 0xC4u, vdvs, (uint16_t)sizeof(vdvs));
    (void)Dev_ST7789_Cmd(lcd, 0xC6u, frctrl2, (uint16_t)sizeof(frctrl2));
    (void)Dev_ST7789_Cmd(lcd, 0xD0u, pwctrl1, (uint16_t)sizeof(pwctrl1));

    /* ---------- 5. 伽马校正：正极性 + 负极性 ---------- */
    (void)Dev_ST7789_Cmd(lcd, 0xE0u, pvgam, (uint16_t)sizeof(pvgam));
    (void)Dev_ST7789_Cmd(lcd, 0xE1u, nvgam, (uint16_t)sizeof(nvgam));

    /* ---------- 6. 显示相关开关 ---------- */
    (void)Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_INVON, NULL, 0u);
    (void)Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_NORON, NULL, 0u);
    (void)Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_DISPON, NULL, 0u);
    HAL_Delay(20u);

    /* ---------- 7. 默认方向与清屏 ---------- */
    s_ready = 1u;
    if (Dev_ST7789_SetRotation(lcd, 0u) != 0) {
        return -2;
    }
    if (Dev_ST7789_FillScreen(lcd, 0x0000u) != 0) {
        return -2;
    }

    return 0;
}

/**
 * @brief 设置显示方向
 * @param lcd      面板句柄，不能为 NULL
 * @param rotation 0/1/2/3 对应 0°/90°/180°/270°
 * @retval 0: 设置成功
 * @retval -1: lcd 为 NULL 或 rotation 超出 0~3
 * @retval -2: SPI 传输失败
 * @note MADCTL 取值：0x00 正向、0x60 交换行列并镜像、0xC0 双向镜像、
 *       0xA0 交换行列。交换行列的档位（1、3）逻辑宽高随之互换。
 */
int Dev_ST7789_SetRotation(const dev_st7789_t *lcd, uint8_t rotation)
{
    uint8_t madctl;

    if (lcd == NULL) {
        return -1;
    }

    switch (rotation) {
    case 0u:
        madctl   = 0x00u;
        s_width  = lcd->width;
        s_height = lcd->height;
        break;
    case 1u:
        madctl   = 0x60u;
        s_width  = lcd->height;
        s_height = lcd->width;
        break;
    case 2u:
        madctl   = 0xC0u;
        s_width  = lcd->width;
        s_height = lcd->height;
        break;
    case 3u:
        madctl   = 0xA0u;
        s_width  = lcd->height;
        s_height = lcd->width;
        break;
    default:
        return -1;
    }

    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_MADCTL,
                       &madctl, 1u) != 0) {
        return -2;
    }

    s_rotation = rotation;
    return 0;
}

/**
 * @brief 查询当前逻辑宽度
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑宽度（像素）
 * @retval 0: lcd 为 NULL 或尚未初始化
 */
uint16_t Dev_ST7789_Width(const dev_st7789_t *lcd)
{
    if (lcd == NULL) {
        return 0u;
    }

    return s_width;
}

/**
 * @brief 查询当前逻辑高度
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑高度（像素）
 * @retval 0: lcd 为 NULL 或尚未初始化
 */
uint16_t Dev_ST7789_Height(const dev_st7789_t *lcd)
{
    if (lcd == NULL) {
        return 0u;
    }

    return s_height;
}

/**
 * @brief 设置写入窗口（列地址 + 行地址）
 * @param lcd 面板句柄，不能为 NULL
 * @param x0  起始列（含）
 * @param y0  起始行（含）
 * @param x1  结束列（含）
 * @param y1  结束行（含）
 * @retval 0: 设置成功
 * @retval -1: 未初始化、句柄为空，或坐标越界/顺序颠倒
 * @retval -2: SPI 传输失败
 * @note 坐标是"逻辑坐标"（已按旋转换算），面板偏移由句柄的
 *       x_offset / y_offset 补上。
 */
int Dev_ST7789_SetWindow(const dev_st7789_t *lcd, uint16_t x0, uint16_t y0,
                         uint16_t x1, uint16_t y1)
{
    uint8_t  param[4];
    uint32_t xs;
    uint32_t xe;
    uint32_t ys;
    uint32_t ye;

    if ((lcd == NULL) || (s_ready == 0u) || (s_width == 0u)) {
        return -1;
    }
    if ((x0 > x1) || (y0 > y1) || (x1 >= s_width) || (y1 >= s_height)) {
        return -1;
    }

    xs = (uint32_t)x0 + (uint32_t)lcd->x_offset;
    xe = (uint32_t)x1 + (uint32_t)lcd->x_offset;
    ys = (uint32_t)y0 + (uint32_t)lcd->y_offset;
    ye = (uint32_t)y1 + (uint32_t)lcd->y_offset;

    param[0] = (uint8_t)(xs >> 8);
    param[1] = (uint8_t)(xs & 0xFFu);
    param[2] = (uint8_t)(xe >> 8);
    param[3] = (uint8_t)(xe & 0xFFu);
    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_CASET,
                       param, 4u) != 0) {
        return -2;
    }

    param[0] = (uint8_t)(ys >> 8);
    param[1] = (uint8_t)(ys & 0xFFu);
    param[2] = (uint8_t)(ye >> 8);
    param[3] = (uint8_t)(ye & 0xFFu);
    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_RASET,
                       param, 4u) != 0) {
        return -2;
    }

    return 0;
}

/**
 * @brief 向当前窗口写入像素数据（大端 RGB565）
 * @param lcd  面板句柄，不能为 NULL
 * @param data 像素缓冲区，不能为 NULL
 * @param len  字节数，必须是 2 的整数倍
 * @retval 0: 写入成功
 * @retval -1: 参数非法（空指针、len 为 0 或奇数、未初始化）
 * @retval -2: SPI 传输失败
 * @note 不校验数据是否与窗口大小匹配：写多了会从窗口起点重新开始覆盖，
 *       写少了会留下上一帧的残留。窗口大小由调用者自己保证。
 */
int Dev_ST7789_WritePixels(const dev_st7789_t *lcd, const uint8_t *data,
                           uint32_t len)
{
    if ((lcd == NULL) || (data == NULL) || (len == 0u) || (s_ready == 0u)) {
        return -1;
    }
    if ((len & 1u) != 0u) {
        return -1;
    }

    return Dev_ST7789_Burst(lcd, data, len);
}

/**
 * @brief 用纯色填充当前窗口
 * @param lcd   面板句柄，不能为 NULL
 * @param color 颜色，RGB565
 * @param count 填充像素个数
 * @retval 0: 填充成功
 * @retval -1: 参数非法（空指针或 count 为 0）
 * @retval -2: SPI 传输失败
 * @note 图案缓冲是模块级 static，重复调用不需要重新构造，也不占任务栈。
 */
int Dev_ST7789_FillColor(const dev_st7789_t *lcd, uint16_t color,
                         uint32_t count)
{
    const uint32_t pixels_per_buf = DEV_ST7789_FILL_BUF_SIZE / 2u;
    uint8_t        cmd = (uint8_t)DEV_ST7789_CMD_RAMWR;
    uint32_t       index;
    int            status;

    if ((lcd == NULL) || (count == 0u)) {
        return -1;
    }

    /* ---------- 1. 构造一个缓冲的纯色图案 ---------- */
    for (index = 0u; index < DEV_ST7789_FILL_BUF_SIZE; index += 2u) {
        s_fill_buf[index]           = (uint8_t)(color >> 8);
        s_fill_buf[index + 1u]      = (uint8_t)(color & 0xFFu);
    }

    /* ---------- 2. 整个填充过程只用一个片选帧 ---------- */
    BSP_SPI_Dev_Select(lcd->dev);
    BSP_GPIO_Write(lcd->dc, 0u);
    status = Dev_ST7789_Raw(lcd, &cmd, 1u);

    if (status == 0) {
        BSP_GPIO_Write(lcd->dc, 1u);

        while (count > 0u) {
            uint32_t chunk_pixels = (count > pixels_per_buf) ?
                                    pixels_per_buf : count;

            status = Dev_ST7789_Raw(lcd, s_fill_buf, chunk_pixels * 2u);
            if (status != 0) {
                break;
            }
            count -= chunk_pixels;
        }
    }

    BSP_SPI_Dev_Deselect(lcd->dev);
    return status;
}

/**
 * @brief 整屏填充纯色
 * @param lcd   面板句柄，不能为 NULL
 * @param color 颜色，RGB565
 * @retval 0: 填充成功
 * @retval -1: 参数非法（句柄为空或尚未初始化）
 * @retval -2: SPI 传输失败
 */
int Dev_ST7789_FillScreen(const dev_st7789_t *lcd, uint16_t color)
{
    if ((lcd == NULL) || (s_ready == 0u) || (s_width == 0u)) {
        return -1;
    }

    if (Dev_ST7789_SetWindow(lcd, 0u, 0u,
                             (uint16_t)(s_width - 1u),
                             (uint16_t)(s_height - 1u)) != 0) {
        return -2;
    }

    return Dev_ST7789_FillColor(lcd, color,
                                (uint32_t)s_width * (uint32_t)s_height);
}

/**
 * @brief 控制背光
 * @param lcd 面板句柄，不能为 NULL
 * @param on  1 点亮，0 熄灭
 * @retval 0: 已处理（未接背光脚时是空操作）
 * @retval -1: lcd 为 NULL
 */
int Dev_ST7789_Backlight(const dev_st7789_t *lcd, uint8_t on)
{
    if (lcd == NULL) {
        return -1;
    }

    if (lcd->bl != NULL) {
        BSP_GPIO_Write(lcd->bl, (on != 0u) ? 1u : 0u);
    }

    return 0;
}

/*==============================================================================
 * 内部调试钩子
 *============================================================================*/
/**
 * @brief 查询当前旋转档位
 * @param lcd 面板句柄，不能为 NULL
 * @retval 0~3: 当前旋转档位
 * @retval 0xFF: lcd 为 NULL
 * @note 供上层做布局适配时读取；不进公共头文件，避免接口面无谓扩大。
 */
uint8_t Dev_ST7789_GetRotation(const dev_st7789_t *lcd)
{
    if (lcd == NULL) {
        return 0xFFu;
    }

    return s_rotation;
}
