/**
 * @file dev_tsc2046.c
 * @brief 设备层实现：HR2046/TSC2046 电阻触摸采样与逻辑坐标换算
 * @author Ricardo
 * @date 2026-11-15
 *
 * @note 本文件不含引脚号：SCK/MOSI/MISO 属于 SPI1 总线，CS 与 PENIRQ 是
 *       设备句柄里的两个 bsp_gpio_t 指针。换引脚只改本文件末尾的实例，
 *       以及 gpio.c 里的电气配置。
 *
 *       一次采样的时序：
 *         CS 低 → 发 1 字节命令 → 再发 2 字节空数据（同时收回来）→ CS 高
 *       总共 24 个 DCLK。命令的 PD1/PD0 必须为 00：置位会关掉 PENIRQ，
 *       之后读引脚永远读不到按下。BUSY 悬空的模组不能等忙信号，只能按时钟
 *       数读满 24 位再取有效位。
 *
 *       ★12 位落在哪几位：shift 字段★
 *       手册的时序是"命令之后第 1 个时钟出 BUSY(0)，接着 12 个时钟出
 *       B11..B0"，即 16 位里有效数据在 bit14..bit3，取 >>3。部分成熟例程
 *       写 >>4，那是把 BUSY 位当成数据最高位的写法。两种都能在某些模组上
 *       跑通，所以这里做成可配字段而不是写死：
 *         四角量程只到 ~2047 → 少移了一位，把 shift 改小（4 → 3）；
 *         四角量程超过 4095  → 多移了一位，把 shift 改大（3 → 4）。
 *       标定会吸收倍率误差，但移位数错了会先截掉有效位，标定救不回来。
 *
 *       ★几何必须与 dev_st7789_0 一致★
 *       panel_width/height 是玻璃的物理像素（240×320），width/height 是 LCD
 *       当前可视的逻辑尺寸（172×320），x_offset/y_offset 是可视区相对玻璃
 *       左上角的偏移（34/0）。改屏宽时这三组值要一起改，否则触摸点整体偏移。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include "dev_tsc2046.h"
// 功能包含
#include "bsp_spi.h"
#include "bsp_gpio.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 读 X 通道：S=1 | A=101 | 12 位 | 差分 | PD=00 */
#define DEV_TSC2046_CMD_READ_X      0xD0u
/** @brief 读 Y 通道：S=1 | A=001 | 12 位 | 差分 | PD=00 */
#define DEV_TSC2046_CMD_READ_Y      0x90u

/** @brief 有效数据位宽：移位后统一按 12 位截取 */
#define DEV_TSC2046_RAW_MAX         0x0FFFu

/** @brief 一次采样的事务长度：1 字节命令 + 2 字节数据 */
#define DEV_TSC2046_SAMPLE_LEN      3u

/** @brief 一次采样的超时预算（ms）：3 字节 @1.31 MHz 约 20 µs，50 ms 只为兜底 */
#define DEV_TSC2046_FRAME_TIMEOUT_MS 50u

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 触摸控制器句柄
 * @note 只放配置：总线、中断脚、几何、量程与方向修正。
 *       就绪标志是运行时状态，放在下面的 static 里——因此本模块是
 *       单实例设计，与 dev_st7789.c 的约定一致。
 */
struct dev_tsc2046 {
    const bsp_spi_dev_t *dev;         /* SPI 设备句柄：片选与所属总线都由它携带 */
    const bsp_gpio_t    *irq;         /* PENIRQ，NULL 表示没接、改用量程判定 */
    uint16_t panel_width;             /* 玻璃物理宽度（像素） */
    uint16_t panel_height;            /* 玻璃物理高度（像素） */
    uint16_t x_offset;                /* 可视区左边界相对玻璃的列偏移 */
    uint16_t y_offset;                /* 可视区上边界相对玻璃的行偏移 */
    uint16_t width;                   /* 可视区逻辑宽度 */
    uint16_t height;                  /* 可视区逻辑高度 */
    uint16_t raw_x_min;               /* X 通道最小有效原始码（标定值） */
    uint16_t raw_x_max;               /* X 通道最大有效原始码（标定值） */
    uint16_t raw_y_min;               /* Y 通道最小有效原始码（标定值） */
    uint16_t raw_y_max;               /* Y 通道最大有效原始码（标定值） */
    uint8_t  shift;                   /* 16 位读回值右移位数，取 3 或 4 */
    uint8_t  swap_xy;                 /* 1: 原始 X/Y 需互换（屏装反 90°） */
    uint8_t  invert_x;                /* 1: 映射后 X 取反 */
    uint8_t  invert_y;                /* 1: 映射后 Y 取反 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/**
 * @brief 默认触摸实例
 * @note 与 dev_st7789_0 的几何一一对应（240×320 玻璃、可视 172×320、左偏 34）。
 *       原始量程取 200~3900：这类模组的有效范围通常落在 150~4000 之间，
 *       先给一组保守值让点击落在屏内；实测四角后按实测值收紧。
 *
 *       方向修正默认全 0。接反时的现象与改法：
 *         手指按左边、坐标报右边 → invert_x 置 1（或把 raw_x_min/max 对调）；
 *         手指按上边、坐标报下边 → invert_y 置 1；
 *         左右与上下整体互换   → swap_xy 置 1。
 *       先只动一个字段再验证，同时改两个会分不清是谁生效。
 */
const dev_tsc2046_t dev_tsc2046_0 = {
    .dev          = &bsp_spi1_dev_touch,
    .irq          = &bsp_gpio_pc4,
    .panel_width  = 240u,
    .panel_height = 320u,
    .x_offset     = 34u,
    .y_offset     = 0u,
    .width        = 172u,
    .height       = 320u,
    .raw_x_min    = 200u,
    .raw_x_max    = 3900u,
    .raw_y_min    = 200u,
    .raw_y_max    = 3900u,
    .shift        = 3u,
    .swap_xy      = 0u,
    .invert_x     = 0u,
    .invert_y     = 0u,
};

/** @brief 1 表示 Init 已完成，未完成时拒绝采样 */
static uint8_t s_ready;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 发一条命令并取回一个通道的原始码
 * @param tp  触摸句柄，调用者保证非 NULL 且已就绪
 * @param cmd 命令字，取 DEV_TSC2046_CMD_READ_X / _Y
 * @param raw 输出：12 位原始码
 * @retval 0: 采样成功
 * @retval -2: 片选、总线或传输超时
 * @note 收发缓冲都在栈上：BSP 的传输是阻塞式完成（DMA + 任务通知），
 *       函数返回时 DMA 已经停了，栈空间不会在传输途中被回收。
 *       掩码只取 12 位：移位后高位可能残留 BUSY 位，不截掉会让量程判断
 *       把未按下误判成按下。
 */
static int Dev_TSC2046_Sample(const dev_tsc2046_t *tp, uint8_t cmd,
                              uint16_t *raw)
{
    uint8_t tx[DEV_TSC2046_SAMPLE_LEN] = { cmd, 0u, 0u };
    uint8_t rx[DEV_TSC2046_SAMPLE_LEN] = { 0u, 0u, 0u };
    uint32_t value;

    if (BSP_SPI_Dev_Transfer(tp->dev, tx, rx, DEV_TSC2046_SAMPLE_LEN,
                             DEV_TSC2046_FRAME_TIMEOUT_MS) != BSP_SPI_OK) {
        return -2;
    }

    value  = ((uint32_t)rx[1] << 8) | (uint32_t)rx[2];
    value >>= tp->shift;
    *raw    = (uint16_t)(value & DEV_TSC2046_RAW_MAX);
    return 0;
}

/**
 * @brief 判断原始码是否落在有效量程内
 * @param tp 触摸句柄，调用者保证非 NULL
 * @param x  X 通道原始码
 * @param y  Y 通道原始码
 * @retval 1: 两个通道都在量程内，视为有按下
 * @retval 0: 至少一个通道越界，视为未按下
 * @note 只在没有 PENIRQ 时作为按下判据：电阻屏未按下时通道是浮空的，
 *       输出会贴到 0 或满量程，用"两端都落在量程内"能挡掉绝大多数误判。
 */
static uint8_t Dev_TSC2046_In_Range(const dev_tsc2046_t *tp, uint16_t x,
                                    uint16_t y)
{
    if ((x <= tp->raw_x_min) || (x >= tp->raw_x_max)) {
        return 0u;
    }
    if ((y <= tp->raw_y_min) || (y >= tp->raw_y_max)) {
        return 0u;
    }
    return 1u;
}

/**
 * @brief 把一段原始码线性映射到 [0, span-1]
 * @param raw  原始码
 * @param min  量程下限
 * @param max  量程上限，必须大于 min
 * @param span 目标区间长度（玻璃像素数）
 * @retval 0 ~ span-1 的映射结果
 * @note 越界直接夹到两端，不做外推：黑边上的手指只会得到最近的可视像素，
 *       比返回一个负数或溢出值更容易被上层正确处理。
 */
static uint16_t Dev_TSC2046_Scale(uint16_t raw, uint16_t min, uint16_t max,
                                  uint16_t span)
{
    uint32_t value;

    if (raw <= min) {
        return 0u;
    }
    if (raw >= max) {
        return (uint16_t)(span - 1u);
    }

    value = (((uint32_t)(raw - min) * (uint32_t)(span - 1u))
             / (uint32_t)(max - min));
    return (uint16_t)value;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 校验句柄并标记就绪
 * @param tp 触摸句柄，不能为 NULL
 * @retval 0: 就绪
 * @retval -1: 句柄、总线或片选为空，或几何/量程参数自相矛盾
 * @note 只做参数校验，不碰硬件：本芯片没有 ID 寄存器，读一次也证明不了
 *       器件在线。真正的连通性只能靠"按下时能读到量程内的值"来发现。
 */
int Dev_TSC2046_Init(const dev_tsc2046_t *tp)
{
    if ((tp == NULL) || (tp->dev == NULL)) {
        return -1;
    }
    if ((tp->panel_width == 0u) || (tp->panel_height == 0u) ||
        (tp->width == 0u) || (tp->height == 0u)) {
        return -1;
    }
    /* 可视区必须落在玻璃内：算错会得到负数坐标，随后被夹成 0，
       现象是"屏幕左侧一条竖条怎么点都没反应"，很难反推到配置 */
    if (((uint32_t)tp->x_offset + tp->width > tp->panel_width) ||
        ((uint32_t)tp->y_offset + tp->height > tp->panel_height)) {
        return -1;
    }
    if ((tp->raw_x_max <= tp->raw_x_min) || (tp->raw_y_max <= tp->raw_y_min)) {
        return -1;
    }
    if ((tp->shift != 3u) && (tp->shift != 4u)) {
        return -1;
    }

    s_ready = 1u;
    return 0;
}

/**
 * @brief 查询当前是否被按下
 * @param tp 触摸句柄，不能为 NULL
 * @retval 1: 有按下
 * @retval 0: 未按下，或句柄为空、未初始化、采样失败
 * @note 接了 PENIRQ 时只读一个 GPIO，不占 SPI 总线；没接时做一次量程判定，
 *       会占用一次总线事务。本函数不做消抖。
 */
uint8_t Dev_TSC2046_Is_Pressed(const dev_tsc2046_t *tp)
{
    uint16_t x;
    uint16_t y;

    if ((tp == NULL) || (s_ready == 0u)) {
        return 0u;
    }

    if (tp->irq != NULL) {
        /* PENIRQ 低有效：模组上已有 10K 上拉，未按下时读到高 */
        return (BSP_GPIO_Read(tp->irq) == 0u) ? 1u : 0u;
    }

    if (Dev_TSC2046_Read_Raw(tp, &x, &y) != 0) {
        return 0u;
    }
    return Dev_TSC2046_In_Range(tp, x, y);
}

/**
 * @brief 读一次原始坐标（12 位，未标定）
 * @param tp 触摸句柄，不能为 NULL
 * @param x  输出：X 通道原始码，0~4095
 * @param y  输出：Y 通道原始码，0~4095
 * @retval 0: 读成功
 * @retval -1: 句柄或输出指针为空，或未初始化
 * @retval -2: SPI 传输失败
 * @note X、Y 是两次独立事务：两次采样之间总线会被释放，别的设备可以插进来。
 */
int Dev_TSC2046_Read_Raw(const dev_tsc2046_t *tp, uint16_t *x, uint16_t *y)
{
    if ((tp == NULL) || (x == NULL) || (y == NULL) || (s_ready == 0u)) {
        return -1;
    }

    if (Dev_TSC2046_Sample(tp, (uint8_t)DEV_TSC2046_CMD_READ_X, x) != 0) {
        return -2;
    }
    if (Dev_TSC2046_Sample(tp, (uint8_t)DEV_TSC2046_CMD_READ_Y, y) != 0) {
        return -2;
    }
    return 0;
}

/**
 * @brief 读一次像素坐标（已标定、已换算到 LCD 逻辑坐标）
 * @param tp 触摸句柄，不能为 NULL
 * @param x  输出：逻辑列坐标，0 ~ 可视宽-1
 * @param y  输出：逻辑行坐标，0 ~ 可视高-1
 * @retval 0: 读成功
 * @retval -1: 句柄或输出指针为空，或未初始化
 * @retval -2: SPI 传输失败
 * @note 换算顺序固定：先按 swap_xy 换轴，再映射到玻璃像素，再按
 *       invert_x/invert_y 取反，最后减掉可视区偏移并夹到边界内。
 *       顺序反过来（先减偏移再映射）会让偏移量被当成原始码参与缩放。
 *       **本函数不做"是否按下"的判断**：手指离开时通道浮空，读出来的码
 *       同样能被换算成一个合法坐标。调用前必须先 Dev_TSC2046_Is_Pressed()，
 *       否则会把浮空值当成一次点击。
 *       越界值一律夹到边界而不是报错：按在可视区外的黑边上时，返回最近的
 *       可视像素比返回一个错误码更容易被上层处理。
 */
int Dev_TSC2046_Read_Pixel(const dev_tsc2046_t *tp, uint16_t *x, uint16_t *y)
{
    uint16_t raw_x;
    uint16_t raw_y;
    uint16_t glass_x;
    uint16_t glass_y;
    uint16_t pixel;
    int      status;

    if ((tp == NULL) || (x == NULL) || (y == NULL)) {
        return -1;
    }

    status = Dev_TSC2046_Read_Raw(tp, &raw_x, &raw_y);
    if (status != 0) {
        return status;
    }

    /* ---------- 1. 换轴：屏装反 90° 时 X/Y 整对调换 ---------- */
    if (tp->swap_xy != 0u) {
        uint16_t tmp = raw_x;

        raw_x = raw_y;
        raw_y = tmp;
    }

    /* ---------- 2. 原始码 → 玻璃像素 ---------- */
    glass_x = Dev_TSC2046_Scale(raw_x, tp->raw_x_min, tp->raw_x_max,
                                tp->panel_width);
    glass_y = Dev_TSC2046_Scale(raw_y, tp->raw_y_min, tp->raw_y_max,
                                tp->panel_height);

    /* ---------- 3. 方向修正 ---------- */
    if (tp->invert_x != 0u) {
        glass_x = (uint16_t)((tp->panel_width - 1u) - glass_x);
    }
    if (tp->invert_y != 0u) {
        glass_y = (uint16_t)((tp->panel_height - 1u) - glass_y);
    }

    /* ---------- 4. 玻璃像素 → 可视区逻辑坐标（夹到边界） ---------- */
    pixel = (glass_x > tp->x_offset) ? (uint16_t)(glass_x - tp->x_offset) : 0u;
    *x = (pixel < tp->width) ? pixel : (uint16_t)(tp->width - 1u);

    pixel = (glass_y > tp->y_offset) ? (uint16_t)(glass_y - tp->y_offset) : 0u;
    *y = (pixel < tp->height) ? pixel : (uint16_t)(tp->height - 1u);

    return 0;
}
