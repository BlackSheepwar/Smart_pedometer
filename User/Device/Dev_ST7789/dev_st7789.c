/**
 * @file dev_st7789.c
 * @brief 设备层实现：ST7789 SPI 彩屏，开窗 + 突发写像素
 * @author Ricardo
 * @date 2026-11-04
 *
 * @note 本文件不含引脚号：SCK/MOSI/CS 属于设备层的 SPI 虚拟外设
 *       （Dev_SPI，片选引脚句柄在 BSP_GPIO 里），DC/RST/BL 只是三个
 *       bsp_gpio_t 指针。本文件只 include 虚拟外设的头，不碰 BSP_SPI。
 *       同一条 SPI1 上只有显示设备：整帧期间本设备的 CS 保持低，所以按字节
 *       搬数据不会被打断。
 *
 *       一次写屏的时序：
 *         CS 低 → DC 低 → 发命令 → DC 高 → 发参数/像素 → CS 高
 *       片选与总线在整帧期间一起被本任务独占：Dev_SPI_Select() 抢锁
 *       成功才拉 CS，帧内只走 Dev_SPI_Transfer()，它检测到帧已开就不再重入互斥量，
 *       Dev_SPI_Deselect() 抬 CS 之后才放锁。所以"CS 全程保持低"与
 *       "总线不被打断"是同一件事，不需要额外的临界区；分块大小只影响
 *       单次 DMA 的长度，不影响帧的完整性。
 *
 *       本层不含颜色语义：所有写显存的入口都是"给我一段字节，我负责
 *       把它送进窗口"。纯色填充、渐变、贴图都是上层的图案，不是设备能力。
 *
 *       时间基准：Init 的复位/上电延时由调用方以 dev_st7789_delay_t 注入，
 *       本文件因此不含 FreeRTOS.h / cmsis_os2.h；传输超时交给 BSP 的
 *       timeout_ms，本层不自己计数。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include "dev_st7789.h"
// 功能包含
#include "dev_spi.h"
#include "bsp_gpio.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单次 SPI 传输的最大字节数：len 是 uint16_t，且 DMA 计数器上限 65535 */
#define DEV_ST7789_CHUNK            4096u
/** @brief 一帧等总线的预算（ms）：按整屏字节数 ÷ 实际 SCK 估算，
 *         再给排队与分块留余量 */
#define DEV_ST7789_FRAME_TIMEOUT_MS 300u
/** @brief ST7789 内部显存列数，窗口起点 + 宽度不能超过它 */
#define DEV_ST7789_RAM_WIDTH        240u
/** @brief ST7789 内部显存行数，窗口起点 + 高度不能超过它 */
#define DEV_ST7789_RAM_HEIGHT       320u

/** @brief MADCTL 的 RGB/BGR 位：置 1 时红蓝互换 */
#define DEV_ST7789_MADCTL_BGR       0x08u

/**
 * @brief 发一条带参数的初始化命令，失败时返回真
 * @note 只给 Dev_ST7789_Init 用：那十几条配置命令的失败处理完全一样，
 *       逐条写 if 会把函数拉长一倍。参数长度由 sizeof 自动取，所以 param
 *       必须是数组（不能是退化成指针的形参）。
 */
#define DEV_ST7789_CMD_FAILED(lcd, cmd, param)                          \
    (Dev_ST7789_Cmd((lcd), (cmd), (param), (uint16_t)sizeof(param)) != 0)

/** @brief 软件复位 */
#define DEV_ST7789_CMD_SWRESET      0x01u
/** @brief 退出睡眠 */
#define DEV_ST7789_CMD_SLPOUT       0x11u
/** @brief 反显（IPS 面板常需要；发给 TN 面板会把白底黑字变成黑底白字） */
#define DEV_ST7789_CMD_INVON        0x21u
/** @brief 正显（TN 面板的默认极性） */
#define DEV_ST7789_CMD_INVOFF       0x20u
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
/** @brief 像素格式命令码，参数在 Init 的 colmod16[]（0x55/0x66/0x33） */
#define DEV_ST7789_CMD_COLMOD       0x3Au
/** @brief 正常显示模式 */
#define DEV_ST7789_CMD_NORON        0x13u

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 面板的运行块（内部类型，不进公共头文件）
 * @note 每块面板一份：状态跟着句柄走，多块面板之间互不覆盖。
 */
typedef struct {
    uint16_t width;     /* 当前逻辑宽度，随旋转变化，Init 之前为 0 */
    uint16_t height;    /* 当前逻辑高度，随旋转变化，Init 之前为 0 */
    uint8_t  rotation;  /* 当前旋转档位 0~3 */
    uint8_t  ready;     /* 1 表示 Init 已完成；未完成时拒绝开窗与写像素 */
} dev_st7789_rt_t;

/**
 * @brief ST7789 面板句柄
 * @note 只放配置与一个指向运行块的指针：总线、片选、三个控制脚、
 *       面板原生尺寸与偏移、色序都在这里；运行块的类型定义在本文件中，
 *       实例与句柄成对出现，状态不共享。
 */
struct dev_st7789 {
    const dev_spi_dev_t *dev;       /* 虚拟 SPI 设备：片选与所属总线由它携带 */
    const bsp_gpio_t    *dc;        /* 数据/命令选择：0 命令，1 数据 */
    const bsp_gpio_t    *rst;       /* 硬复位，NULL 表示接板级复位 */
    const bsp_gpio_t    *bl;        /* 背光，NULL 表示常亮 */
    uint16_t             x_offset;  /* 面板列方向显示偏移 */
    uint16_t             y_offset;  /* 面板行方向显示偏移 */
    uint16_t             width;     /* 面板原生宽度（未旋转） */
    uint16_t             height;    /* 面板原生高度（未旋转） */
    uint8_t              bgr;       /* 1: MADCTL 置 RGB/BGR 位，红蓝互换时用 */
    uint8_t              inv;       /* 1: 发 INVON（IPS 面板）；0: 发 INVOFF（TN 面板） */
    dev_st7789_rt_t     *rt;        /* 本实例的运行块：每块面板各一份 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/**
 * @brief 面板 0 的运行块
 * @note 与面板实例成对定义：一个句柄实例配一份运行块，库侧一起进移植段。
 */
static dev_st7789_rt_t dev_st7789_0_rt;

/**
 * @brief 默认面板实例
 * @note 几何四参数描述"玻璃在显存里的位置"：ST7789 显存固定 240 列 × 320 行，
 *       玻璃只取其中一段时，x_offset / y_offset 是那一段的起点，
 *       width / height 是它的大小；铺满显存时偏移为 0、尺寸为 240×320。
 *
 *       几何必须与 Svc_LVGL 的分辨率、UI 的导出尺寸一致；
 *       不一致时 Svc_LVGL_Init() 返回 -3 停在故障态，不会给出错位的界面。
 *
 *       inv 与 bgr 是玻璃走线的两个属性，都只影响颜色、不影响布局：
 *         inv = 1 发 INVON（IPS 常黑型），inv = 0 发 INVOFF（TN 常白型）；
 *         bgr = 1 在 MADCTL 里置 RGB/BGR 位，修正红蓝互换。
 *       判据看现象：整幅明暗反转 → inv 与该玻璃不符；红蓝互换 → bgr 不符。
 */
const dev_st7789_t dev_st7789_0 = {
    .dev      = &dev_spi_dev_lcd,
    .dc       = &bsp_gpio_pb11,
    .rst      = &bsp_gpio_pb12,
    .bl       = NULL,
    .x_offset = 0u,
    .y_offset = 0u,
    .width    = 240u,
    .height   = 320u,
    .bgr      = 0u,
    .inv      = 0u,
    .rt       = &dev_st7789_0_rt,
};


/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 在当前帧内发送一段字节（不碰片选、不碰锁）
 * @param lcd  面板句柄，调用者保证非 NULL
 * @param data 待发送缓冲区，不能为 NULL
 * @param len  字节数，不能为 0
 * @retval 0: 全部发完
 * @retval -2: 其中一次传输失败（总线错误或传输超时）
 * @note 按 DEV_ST7789_CHUNK 分块，因为虚拟外设的 len 是 uint16_t。
 *       片选与总线由调用者在帧首用 Dev_SPI_Select() 取得，本函数只走
 *       会话内那条分支，不会重新抢锁，所以整帧期间总线不会被别的设备
 *       拿走。
 */
static int Dev_ST7789_Raw(const dev_st7789_t *lcd, const uint8_t *data,
                          uint32_t len)
{
    while (len > 0u) {
        uint16_t chunk = (len > DEV_ST7789_CHUNK) ?
                         (uint16_t)DEV_ST7789_CHUNK : (uint16_t)len;

        if (Dev_SPI_Transfer(lcd->dev, data, NULL, chunk, 0u)
            != DEV_SPI_OK) {
            return -2;
        }

        data += chunk;
        len  -= chunk;
    }

    return 0;
}

/**
 * @brief 发送一条命令，可选带参数（整帧独占总线）
 * @param lcd       面板句柄，调用者保证非 NULL
 * @param cmd       命令字
 * @param param     参数缓冲区，NULL 表示无参数
 * @param param_len 参数字节数，0 表示无参数
 * @retval 0: 成功
 * @retval -2: 抢总线失败，或传输失败
 * @note 命令与参数在同一个片选帧内完成，避免 CS 上升沿打断参数序列。
 *       抢总线失败时片选没有被拉低，直接返回即可，不需要也不能调
 *       Dev_SPI_Deselect()。
 */
static int Dev_ST7789_Cmd(const dev_st7789_t *lcd, uint8_t cmd,
                          const uint8_t *param, uint16_t param_len)
{
    int status;

    if (Dev_SPI_Select(lcd->dev, DEV_ST7789_FRAME_TIMEOUT_MS)
        != DEV_SPI_OK) {
        return -2;
    }

    BSP_GPIO_Write(lcd->dc, 0u);
    status = Dev_ST7789_Raw(lcd, &cmd, 1u);

    if ((status == 0) && (param != NULL) && (param_len > 0u)) {
        BSP_GPIO_Write(lcd->dc, 1u);
        status = Dev_ST7789_Raw(lcd, param, param_len);
    }

    Dev_SPI_Deselect(lcd->dev);
    return status;
}

/**
 * @brief 打开一个写显存帧：抢总线 → 拉片选 → 发 RAMWR → DC 切数据态
 * @param lcd 面板句柄，调用者保证非 NULL 且已 Init
 * @retval 0: 帧已打开，可以连续送像素
 * @retval -2: 抢总线失败，或 RAMWR 命令发送失败
 * @note 与 Dev_ST7789_FrameClose() 成对，WritePixels / WritePattern 共用。
 *       两个失败点处理不同：抢总线失败时片选还没拉低，直接返回；RAMWR
 *       失败时片选已低，必须先抬回去，否则总线会被一台"被选中的死设备"
 *       占着，下一个任务永远抢不到。
 *       DC 只在命令发成功后才切到数据态：失败时留在命令态，后续任何
 *       字节都不会被误当像素写进显存。
 *       一帧只能发一次 RAMWR —— 重发会把写指针复位到窗口起点，分块
 *       续写时前面的数据会被覆盖。
 */
static int Dev_ST7789_FrameOpen(const dev_st7789_t *lcd)
{
    uint8_t cmd = (uint8_t)DEV_ST7789_CMD_RAMWR;

    if (Dev_SPI_Select(lcd->dev, DEV_ST7789_FRAME_TIMEOUT_MS)
        != DEV_SPI_OK) {
        return -2;
    }

    BSP_GPIO_Write(lcd->dc, 0u);
    if (Dev_ST7789_Raw(lcd, &cmd, 1u) != 0) {
        Dev_SPI_Deselect(lcd->dev);
        return -2;
    }
    BSP_GPIO_Write(lcd->dc, 1u);

    return 0;
}

/**
 * @brief 关闭写显存帧：抬片选 + 归还总线
 * @param lcd 面板句柄，调用者保证非 NULL 且帧已打开
 * @note 与 Dev_ST7789_FrameOpen() 成对，成功失败都要调。
 */
static void Dev_ST7789_FrameClose(const dev_st7789_t *lcd)
{
    Dev_SPI_Deselect(lcd->dev);
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化面板：硬复位 → 软复位 → 上电时序 → 开显示
 * @param lcd      面板句柄，不能为 NULL
 * @param delay_ms 毫秒延时来源，不能为 NULL；整个时序共约 465 ms
 * @retval 0: 初始化成功
 * @retval -1: 句柄或成员为空、delay_ms 为 NULL，或面板几何越出 ST7789
 *         显存范围（列方向 240、行方向 320）
 * @retval -2: SPI 传输失败
 * @note 本函数不含任何 OS 调用：延时由 delay_ms 注入，本层只描述"按手册
 *       要等 N 毫秒"。调用方仍须在调度器启动后的线程里调用——
 *       注入 osDelay 时调度器没跑不会真的等，时序会全部落空；
 *       注入忙等则不会让出 CPU，低优先级任务在这 465 ms 内得不到执行。
 *       上电时序取自 ST7789V 的常见模组配置；若屏幕颜色发灰或出现残影，
 *       优先调 PORCTRL / VCOMS / 伽马三段，其余项一般不用动。
 *       结束后不清屏，第一帧由应用层自己刷。
 */
int Dev_ST7789_Init(const dev_st7789_t *lcd, dev_st7789_delay_t delay_ms)
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
    static const uint8_t colmod16[] = { 0x55u };
    static const uint8_t pvgam[]    = { 0xD0u, 0x04u, 0x0Du, 0x11u, 0x13u,
                                        0x2Bu, 0x3Fu, 0x54u, 0x4Cu, 0x18u,
                                        0x0Du, 0x0Bu, 0x1Fu, 0x23u };
    static const uint8_t nvgam[]    = { 0xD0u, 0x04u, 0x0Cu, 0x11u, 0x13u,
                                        0x2Cu, 0x3Fu, 0x44u, 0x51u, 0x2Fu,
                                        0x1Fu, 0x1Fu, 0x20u, 0x23u };

    if ((lcd == NULL) || (lcd->dev == NULL) || (lcd->dc == NULL) ||
        (delay_ms == NULL)) {
        return -1;
    }
    /* 面板几何必须落在 ST7789 显存 240×320 之内：配错会发出非法 CASET/
       RASET 参数，现象是画面错位或被截断，很难从现象反推配置 */
    if ((lcd->width == 0u) || (lcd->height == 0u) ||
        (((uint32_t)lcd->x_offset + lcd->width) > DEV_ST7789_RAM_WIDTH) ||
        (((uint32_t)lcd->y_offset + lcd->height) > DEV_ST7789_RAM_HEIGHT)) {
        return -1;
    }

    /* 重新初始化时先摘掉就绪标志：中途失败不会留下"自称已就绪"的状态 */
    lcd->rt->ready = 0u;

    /* ---------- 1. 硬复位：RST 低 20 ms 再拉高，等待内部复位完成 ---------- */
    if (lcd->rst != NULL) {
        BSP_GPIO_Write(lcd->rst, 1u);
        delay_ms(5u);
        BSP_GPIO_Write(lcd->rst, 0u);
        delay_ms(20u);
        BSP_GPIO_Write(lcd->rst, 1u);
        delay_ms(120u);
    }

    /* ---------- 2. 软复位与退出睡眠 ---------- */
    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_SWRESET, NULL, 0u) != 0) {
        return -2;
    }
    delay_ms(150u);
    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_SLPOUT, NULL, 0u) != 0) {
        return -2;
    }
    delay_ms(150u);        /* 手册要求 120 ms，留一档余量避免边界 */

    /* ---------- 3. 像素格式：16 bit RGB565 ---------- */
    if (DEV_ST7789_CMD_FAILED(lcd, (uint8_t)DEV_ST7789_CMD_COLMOD, colmod16)) {
        return -2;
    }

    /* ---------- 4. 时序、电源与帧率 ---------- */
    if (DEV_ST7789_CMD_FAILED(lcd, 0xB2u, porctrl) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xB7u, gctrl) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xBBu, vcoms) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xC0u, lcmctrl) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xC2u, vdvrvhen) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xC3u, vrhs) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xC4u, vdvs) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xC6u, frctrl2) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xD0u, pwctrl1)) {
        return -2;
    }

    /* ---------- 5. 伽马校正：正极性 + 负极性 ---------- */
    if (DEV_ST7789_CMD_FAILED(lcd, 0xE0u, pvgam) ||
        DEV_ST7789_CMD_FAILED(lcd, 0xE1u, nvgam)) {
        return -2;
    }

    /* ---------- 6. 显示相关开关：面板极性 → 正常模式 → 开显示 ---------- */
    /* 极性由面板玻璃决定，命令之外的初始化序列完全一样：
       IPS 玻璃为常黑型，必须 INVON 才正显；
       TN 玻璃为常白型，多发 INVON 会把整幅画面取反，
       表现就是白底黑字变成黑底白字——不是配色写错，也不是字节序错。 */
    if (Dev_ST7789_Cmd(lcd,
                       (lcd->inv != 0u) ? (uint8_t)DEV_ST7789_CMD_INVON
                                        : (uint8_t)DEV_ST7789_CMD_INVOFF,
                       NULL, 0u) != 0) {
        return -2;
    }
    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_NORON, NULL, 0u) != 0) {
        return -2;
    }
    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_DISPON, NULL, 0u) != 0) {
        return -2;
    }
    delay_ms(20u);

    /* ---------- 7. 默认方向：MADCTL 放在开显示之后，第一帧就是正确朝向 ---------- */
    if (Dev_ST7789_SetRotation(lcd, 0u) != 0) {
        return -2;
    }

    /* 全部成功才置就绪：中途任何一步失败时 lcd->rt->ready 保持 0，
       上层不会在半个配置好的面板上写数据 */
    lcd->rt->ready = 1u;
    return 0;
}

/**
 * @brief 设置显示方向
 * @param lcd      面板句柄，不能为 NULL
 * @param rotation 0/1/2/3 对应 0°/90°/180°/270°
 * @retval 0: 设置成功
 * @retval -1: lcd 为 NULL，或 rotation 超出 0~3
 * @retval -2: SPI 传输失败
 * @note MADCTL 取值：0x00 正向、0x60 交换行列并镜像、0xC0 双向镜像、
 *       0xA0 交换行列。交换行列的档位（1、3）逻辑宽高随之互换。
 *       句柄的 bgr 为 1 时额外置 bit3，用于修正模组走线导致的红蓝互换。
 *       命令发成功才更新 lcd->rt->width / lcd->rt->height：失败时不会留下一个"没生效
 *       的尺寸"，否则上层按新尺寸算出来的坐标全是错的。
 */
int Dev_ST7789_SetRotation(const dev_st7789_t *lcd, uint8_t rotation)
{
    uint8_t  madctl;
    uint16_t width;
    uint16_t height;

    if (lcd == NULL) {
        return -1;
    }

    switch (rotation) {
    case 0u:
        madctl = 0x00u;
        width  = lcd->width;
        height = lcd->height;
        break;
    case 1u:
        madctl = 0x60u;
        width  = lcd->height;
        height = lcd->width;
        break;
    case 2u:
        madctl = 0xC0u;
        width  = lcd->width;
        height = lcd->height;
        break;
    case 3u:
        madctl = 0xA0u;
        width  = lcd->height;
        height = lcd->width;
        break;
    default:
        return -1;
    }

    if (lcd->bgr != 0u) {
        madctl |= (uint8_t)DEV_ST7789_MADCTL_BGR;
    }

    if (Dev_ST7789_Cmd(lcd, (uint8_t)DEV_ST7789_CMD_MADCTL,
                       &madctl, 1u) != 0) {
        return -2;
    }

    /* 命令成功后才落盘：失败时不让 Width/Height 返回一个没生效的尺寸 */
    lcd->rt->width    = width;
    lcd->rt->height   = height;
    lcd->rt->rotation = rotation;
    return 0;
}

/**
 * @brief 查询当前逻辑宽度
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑宽度（像素）
 * @retval 0: lcd 为 NULL；或既未 Init 也从未 SetRotation
 */
uint16_t Dev_ST7789_Width(const dev_st7789_t *lcd)
{
    if (lcd == NULL) {
        return 0u;
    }

    return lcd->rt->width;
}

/**
 * @brief 查询当前逻辑高度
 * @param lcd 面板句柄，不能为 NULL
 * @retval 非 0: 逻辑高度（像素）
 * @retval 0: lcd 为 NULL；或既未 Init 也从未 SetRotation
 */
uint16_t Dev_ST7789_Height(const dev_st7789_t *lcd)
{
    if (lcd == NULL) {
        return 0u;
    }

    return lcd->rt->height;
}

/**
 * @brief 查询当前旋转档位
 * @param lcd 面板句柄，不能为 NULL
 * @retval 0~3: 当前旋转档位
 * @retval 0xFF: lcd 为 NULL
 * @note 供上层做布局适配时读取（横屏/竖屏两套排版的切换依据）。
 */
uint8_t Dev_ST7789_GetRotation(const dev_st7789_t *lcd)
{
    if (lcd == NULL) {
        return 0xFFu;
    }

    return lcd->rt->rotation;
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

    if ((lcd == NULL) || (lcd->rt->ready == 0u) || (lcd->rt->width == 0u)) {
        return -1;
    }
    if ((x0 > x1) || (y0 > y1) || (x1 >= lcd->rt->width) || (y1 >= lcd->rt->height)) {
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
 * @brief 向当前窗口写入一段连续像素
 * @param lcd  面板句柄，不能为 NULL
 * @param data 像素缓冲区，大端 RGB565，不能为 NULL
 * @param len  字节数，必须是 2 的整数倍
 * @retval 0: 写入成功
 * @retval -1: 参数非法（空指针、len 为 0 或奇数、未初始化）
 * @retval -2: 抢总线失败，或 SPI 传输失败
 * @note 一帧开闭 + 原样发送，与 Dev_ST7789_WritePattern() 结构对称，
 *       只是中间少了一层循环。缓冲区里的 len 个字节会被原样写进窗口，
 *       不做任何重复。
 *       不校验数据是否与窗口大小匹配：写多了会从窗口起点重新开始覆盖，
 *       写少了会留下上一帧的残留。窗口大小由调用者自己保证。
 */
int Dev_ST7789_WritePixels(const dev_st7789_t *lcd, const uint8_t *data,
                           uint32_t len)
{
    int status;

    if ((lcd == NULL) || (data == NULL) || (len == 0u) || (lcd->rt->ready == 0u)) {
        return -1;
    }
    if ((len & 1u) != 0u) {
        return -1;
    }

    status = Dev_ST7789_FrameOpen(lcd);
    if (status != 0) {
        return status;
    }

    status = Dev_ST7789_Raw(lcd, data, len);
    Dev_ST7789_FrameClose(lcd);
    return status;
}

/**
 * @brief 在当前窗口内重复写入同一段像素图案，直到写满指定字节数
 * @param lcd       面板句柄，不能为 NULL
 * @param pattern   图案缓冲区，大端 RGB565，不能为 NULL
 * @param pat_len   图案字节数，不能为 0，必须是 2 的倍数
 * @param total_len 要写满的总字节数，不能为 0，必须是 2 的倍数
 * @retval 0: 写入成功
 * @retval -1: 参数非法（空指针、长度为 0 或奇数、未初始化）
 * @retval -2: 抢总线失败，或 SPI 传输失败
 * @note 重复语义只存在于这一层：开一帧 → 连发图案直到写满 → 关帧。
 *       整帧只发一次 RAMWR，图案靠"继续送数据"重复，写指针不会复位。
 *       这正是不该用"循环调用 WritePixels"代替它的原因：每次 WritePixels
 *       都会重发 RAMWR，写指针被拉回窗口起点，只有最后一块会留下。
 *       图案越短，帧内 DMA 启动次数越多：启动次数 ≈ 总字节数 / 图案字节数，
 *       图案翻倍则次数减半。图案大小是"RAM 换时间"的旋钮。
 */
int Dev_ST7789_WritePattern(const dev_st7789_t *lcd, const uint8_t *pattern,
                            uint32_t pat_len, uint32_t total_len)
{
    int status;

    if ((lcd == NULL) || (pattern == NULL) || (lcd->rt->ready == 0u)) {
        return -1;
    }
    if ((pat_len == 0u) || (total_len == 0u) ||
        ((pat_len & 1u) != 0u) || ((total_len & 1u) != 0u)) {
        return -1;
    }

    status = Dev_ST7789_FrameOpen(lcd);
    if (status != 0) {
        return status;
    }

    while ((status == 0) && (total_len > 0u)) {
        uint32_t chunk = (total_len > pat_len) ? pat_len : total_len;

        status = Dev_ST7789_Raw(lcd, pattern, chunk);
        total_len -= chunk;
    }

    Dev_ST7789_FrameClose(lcd);
    return status;
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