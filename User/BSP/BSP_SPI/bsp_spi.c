/**
 * @file bsp_spi.c
 * @brief SPI 硬件驱动层：互斥量排队 + 完成中断唤醒，总线/设备两级句柄
 * @author Ricardo
 * @date 2026-10-29
 *
 * @note 设计模型：
 *       1. 每个 bsp_spi_t 绑一把互斥量，先抢锁再启动传输，抢不到就在锁上
 *          排队；配了 DMA 就启动即返回，完成中断只记录结果、唤醒持锁任务；
 *          没配 DMA 自动退回 HAL 阻塞接口，语义与返回值一致。
 *       2. 传输入口只有设备级一组，两种拼法：
 *            单段  BSP_SPI_Dev_Transfer()，一次调用走完 Select→写→Deselect；
 *            多段  Select() 抢锁 + 拉片选 → TransferLocked() × N →
 *                  Deselect() 抬片选 + 放锁，整帧独占总线。
 *          方向由 tx / rx 是否为 NULL 推断，不传方向参数。
 *       3. 锁与片选必须同生命周期。帧内再抢锁会在同一任务上二次 acquire
 *          非递归互斥量，等满 timeout 后 ERR_TIMEOUT、整帧作废；
 *          frame_owner + frame_dev 把这类误用变成 ERR_STATE。
 *
 *       OS 对象归属：SPI1_LOCK / SPI2_LOCK 由 Smart_pedometer.ioc 配置、
 *       freertos.c 生成，本文件只 extern。BSP_SPI_Init() 必须放
 *       USER CODE BEGIN RTOS_MUTEX，那里句柄才建好；HAL 回调是强符号，
 *       靠这次调用把本目标拉进镜像。
 *       移植：F1/F4 的 HAL_SPI_*_DMA 同名同语义；**SPI1 / SPI2 外设中断
 *       必须开**，TXE/RXNE 与 EOT 都靠 SPIx_IRQHandler 推进。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "bsp_spi.h"
// 功能包含
#include "spi.h"
#include "bsp_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

/*==============================================================================
 * 外部变量声明
 *============================================================================*/
/** @brief CubeMX 生成的互斥量句柄（Smart_pedometer.ioc 里配的 SPI1/SPI2_LOCK）
 *  @note 一条总线一把锁：屏与触摸共用 SPI1，所以它们互相排队；SD 卡在 SPI2
 *        上，写卡不会把刷新屏挡住。 */
extern osMutexId_t SPI1_LOCKHandle;
extern osMutexId_t SPI2_LOCKHandle;

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单次调用默认超时（ms），排队与传输各自受限 */
#define BSP_SPI_DEFAULT_TIMEOUT_MS  50U

/** @brief 方向能力位；TXRX 是 TX|RX 的组合位，不是第三个独立位
 *  @note 检查统一用 (caps & need) == need，组合位与单个位同一套判断。 */
#define BSP_SPI_CAP_TX    0x01U /* 可只发 */
#define BSP_SPI_CAP_RX    0x02U /* 可只收 */
#define BSP_SPI_CAP_TXRX  (BSP_SPI_CAP_TX | BSP_SPI_CAP_RX) /* 可全双工收发 */

/** @brief DMA 能力位；TX / RX 是两个独立硬件事实，TXRX 是组合表达式
 *  @note 全双工要求收发两路都在，缺一路就整体退回阻塞模式。 */
#define BSP_SPI_DMA_TX    0x01U /* 发送 DMA 已配置 */
#define BSP_SPI_DMA_RX    0x02U /* 接收 DMA 已配置 */
#define BSP_SPI_DMA_TXRX  (BSP_SPI_DMA_TX | BSP_SPI_DMA_RX) /* 收发两路都在 */

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 传输方向（内部类型）
 * @note 不进公共头文件；调用者靠 tx / rx 隐式表达，由 BSP_SPI_Prepare() 推断。
 */
typedef enum {
    BSP_SPI_DIR_TX   = 0, /* 只发：发命令、写寄存器、刷屏 */
    BSP_SPI_DIR_RX   = 1, /* 只收：读传感器、读状态 */
    BSP_SPI_DIR_TXRX = 2, /* 全双工同时收发 */
} bsp_spi_dir_t;

/**
 * @brief SPI 总线对象（BSP 内部类型，不对外暴露；上层只拿设备句柄）
 * @note hal 用 void *，避免头文件引入 HAL；lock 存句柄地址，句柄值要等
 *       freertos.c 运行时才产生，存地址才能在实例定义处一次配好。
 *       caps / dma_mask 是 Init 时的能力快照，传输路径只查位。
 */
typedef struct bsp_spi {
    void             *hal;        /* HAL SPI 句柄（SPI_HandleTypeDef *） */
    osMutexId_t      *lock;       /* 指向 freertos.c 生成的互斥量句柄 */
    TaskHandle_t volatile waiter; /* 正在等完成的任务，空闲为 NULL */
    TaskHandle_t volatile frame_owner; /* 当前持有帧事务的任务，空闲为 NULL */
    const bsp_spi_dev_t *frame_dev;    /* 当前帧对应的设备，空闲为 NULL */
    volatile bsp_spi_status_t result;  /* 本次传输结果，由中断写入 */
    volatile uint8_t  initialized;     /* 1: 已完成初始化 */
    uint8_t           caps;            /* 方向能力位，见 BSP_SPI_CAP_* */
    uint8_t           dma_mask;        /* DMA 能力位，见 BSP_SPI_DMA_* */
    uint32_t          presc_code;      /* 当前生效的预分频，HAL 的 BR 编码值 */
    uint32_t          presc_init;      /* 回落基准：CubeMX 给的预分频 */
} bsp_spi_t;

/**
 * @brief SPI 设备对象
 * @note 只存总线、片选、极性、速率；不存 SCK/MOSI。const 进 .rodata。
 *
 *       sck_hz 是该器件能接受的 SCK 上限，同一条总线上各设备各写各的：
 *       屏要 21 MHz、触摸芯片只要 2 MHz，就分别填在各自的句柄里，本层在
 *       片选拉低之前切换，调用方完全无感。0 表示回到 CubeMX 里配的预分频，
 *       已有设备可以继续写 0，不必逐个补这个字段。
 */
struct bsp_spi_dev {
    bsp_spi_t        *bus;             /* 所属总线，不能为 NULL */
    const bsp_gpio_t *cs;              /* 片选引脚句柄，NULL 表示不用片选 */
    uint8_t           cs_active_level; /* 1: 高电平有效；0: 低电平有效 */
    uint32_t          sck_hz;          /* SCK 上限（Hz），0 = 用默认配置 */
};

/**
 * @brief 一次传输的内部描述
 * @note 只在本文件内使用。dir 由两个缓冲区指针推出，调用者不用填。
 */
typedef struct {
    const uint8_t *tx;  /* 发送缓冲区，NULL 表示不发 */
    uint8_t       *rx;  /* 接收缓冲区，NULL 表示不收 */
    uint16_t       len; /* 传输字节数，不能为 0 */
    uint8_t        dir; /* 方向，取 bsp_spi_dir_t */
} bsp_spi_msg_t;

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief SPI1 总线对象：屏与触摸共用，一把锁串行化，仅本文件可见 */
static bsp_spi_t bsp_spi1 = {
    .hal         = &hspi1,
    .lock        = &SPI1_LOCKHandle,
    .waiter      = NULL,
    .frame_owner = NULL,
    .frame_dev   = NULL,
    .result      = 0,
    .initialized = 0U,
    .caps        = 0U,
    .dma_mask    = 0U,
    .presc_code  = 0U,
    .presc_init  = 0U,
};

/** @brief SPI2 总线对象：留给 SPI 模式的 SD 卡，本轮只初始化总线、无设备 */
static bsp_spi_t bsp_spi2 = {
    .hal         = &hspi2,
    .lock        = &SPI2_LOCKHandle,
    .waiter      = NULL,
    .frame_owner = NULL,
    .frame_dev   = NULL,
    .result      = 0,
    .initialized = 0U,
    .caps        = 0U,
    .dma_mask    = 0U,
    .presc_code  = 0U,
    .presc_init  = 0U,
};

/**
 * @brief 挂在 SPI1 上的 LCD 设备对象
 * @note sck_hz 填 0：屏的速率由 CubeMX 里的预分频决定（APB2 84 MHz ÷ 4 =
 *       21 MHz），每次 Select 都把总线恢复到这一档。
 */
const bsp_spi_dev_t bsp_spi1_dev_lcd = {
    .bus             = &bsp_spi1,
    .cs              = &bsp_gpio_pa8,
    .cs_active_level = 0U,
    .sck_hz          = 42000000U,
};

/**
 * @brief 挂在 SPI1 上的触摸设备对象
 * @note HR2046（与 XPT2046 / TSC2046 / ADS7843 命令兼容）的 DCLK 上限是
 *       2 MHz。APB2 84 MHz 下算出的分频是 64，实际 1.31 MHz——留足余量，
 *       因为该芯片对命令与数据之间的间隔也敏感。
 *       与屏共用 SCK/MOSI/MISO，只有片选不同：同一时刻只会有一根 CS 被拉低。
 */
const bsp_spi_dev_t bsp_spi1_dev_touch = {
    .bus             = &bsp_spi1,
    .cs              = &bsp_gpio_pa4,
    .cs_active_level = 0U,
    .sck_hz          = 2000000U,
};

/** @brief 驱动管理的总线表；增加 SPI 时只在这里追加实例，
 *         初始化和中断反查都会自动轮询到它 */
static bsp_spi_t *const s_buses[] = {
    &bsp_spi1,
    &bsp_spi2,
};

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 按 HAL 句柄查找对应的 BSP 总线对象
 * @param hspi HAL SPI 句柄
 * @retval 非 NULL: 对应的总线对象
 * @retval NULL: 不是本驱动管理的句柄
 */
static bsp_spi_t *BSP_SPI_FindBus(SPI_HandleTypeDef *hspi)
{
    size_t i;

    if (hspi == NULL) {
        return NULL;
    }

    for (i = 0U; i < sizeof(s_buses) / sizeof(s_buses[0]); i++) {
        if (s_buses[i]->hal == (void *)hspi) {
            return s_buses[i];
        }
    }
    return NULL;
}

/**
 * @brief 由 CubeMX 配置的方向推出本总线支持哪些传输方向
 * @param hspi HAL SPI 句柄
 * @retval 方向能力位组合，见 BSP_SPI_CAP_*
 * @note 2LINES 全双工三种都能做；1LINE 只有 MOSI 一根线，只能发；
 *       2LINES_RXONLY 只开了接收通道，只能收。
 */
static uint8_t BSP_SPI_CapsOf(const SPI_HandleTypeDef *hspi)
{
    if (hspi->Init.Direction == SPI_DIRECTION_1LINE) {
        return BSP_SPI_CAP_TX;
    }
    if (hspi->Init.Direction == SPI_DIRECTION_2LINES_RXONLY) {
        return BSP_SPI_CAP_RX;
    }
    return (uint8_t)BSP_SPI_CAP_TXRX; /* 2LINES：TX、RX、TXRX 三种方向都能做 */
}

/**
 * @brief 把传输方向翻译成需要的能力位
 * @param dir 方向，取 bsp_spi_dir_t
 * @retval 对应的 BSP_SPI_CAP_* 位
 */
static uint8_t BSP_SPI_CapOfDir(uint8_t dir)
{
    if (dir == (uint8_t)BSP_SPI_DIR_TX) {
        return BSP_SPI_CAP_TX;
    }
    if (dir == (uint8_t)BSP_SPI_DIR_RX) {
        return BSP_SPI_CAP_RX;
    }
    return (uint8_t)BSP_SPI_CAP_TXRX;
}

/**
 * @brief 把传输方向翻译成需要的 DMA 通道位
 * @param dir 方向，取 bsp_spi_dir_t
 * @retval 对应的 BSP_SPI_DMA_* 位组合
 * @note 全双工要收发两路 DMA 都在，缺一路就整体退回阻塞模式。
 */
static uint8_t BSP_SPI_DmaOfDir(uint8_t dir)
{
    if (dir == (uint8_t)BSP_SPI_DIR_TX) {
        return BSP_SPI_DMA_TX;
    }
    if (dir == (uint8_t)BSP_SPI_DIR_RX) {
        return BSP_SPI_DMA_RX;
    }
    return (uint8_t)BSP_SPI_DMA_TXRX;
}

/**
 * @brief 取总线 SCK 的时钟源频率
 * @param hspi HAL SPI 句柄
 * @retval 该外设所属 APB 总线的时钟频率（Hz）
 * @note F4 上 SPI1 挂 APB2、SPI2/SPI3 挂 APB1，两条总线的分频可以不同，
 *       所以不能统一取一个值。换系列必须按参考手册的时钟树重新核对这张表：
 *       算错的后果是实际 SCK 与请求值成倍偏差，现象是"降速后仍然读不通"。
 *       将来接 SPI4/5/6 要一并补进 APB2 分支，否则会被按 APB1 折算。
 */
static uint32_t BSP_SPI_Pclk_Hz(const SPI_HandleTypeDef *hspi)
{
    return (hspi->Instance == SPI1) ? HAL_RCC_GetPCLK2Freq()
                                    : HAL_RCC_GetPCLK1Freq();
}

/**
 * @brief 把分频倍数翻译成 HAL 的 BR 编码
 * @param div 分频倍数，只能取 2/4/8/16/32/64/128/256
 * @retval 可直接写进 SPI_CR1 的 BR 位，与 HAL 的 BaudRatePrescaler 同值
 * @note 不认识的倍数一律落到 256：宁可慢，不可超速——超速是器件收错字节，
 *       慢只是吞吐下降，二者的故障代价不对称。
 */
static uint32_t BSP_SPI_PrescalerCode(uint32_t div)
{
    switch (div) {
    case 2U:
        return SPI_BAUDRATEPRESCALER_2;
    case 4U:
        return SPI_BAUDRATEPRESCALER_4;
    case 8U:
        return SPI_BAUDRATEPRESCALER_8;
    case 16U:
        return SPI_BAUDRATEPRESCALER_16;
    case 32U:
        return SPI_BAUDRATEPRESCALER_32;
    case 64U:
        return SPI_BAUDRATEPRESCALER_64;
    case 128U:
        return SPI_BAUDRATEPRESCALER_128;
    default:
        return SPI_BAUDRATEPRESCALER_256;
    }
}

/**
 * @brief 求"不超过给定速率"的最小分频倍数
 * @param pclk_hz 总线时钟频率（Hz）
 * @param sck_hz  器件能接受的 SCK 上限（Hz），不能为 0
 * @retval 2/4/8/16/32/64/128/256 之一，保证 pclk_hz / 返回值 <= sck_hz
 * @note 取满足条件的最大值而不是随手挑一档：速率只影响帧率，不影响正确性，
 *       多降一档就是白白少一半吞吐。上限高于总线能力时落到 2，低于
 *       256 分频后的频率时落到 256。
 */
static uint32_t BSP_SPI_DivFor(uint32_t pclk_hz, uint32_t sck_hz)
{
    uint32_t div = 2U;

    while ((div < 256U) && ((pclk_hz / div) > sck_hz)) {
        div <<= 1;
    }
    return div;
}

/**
 * @brief 按设备要求切换总线速率（帧开始前调用）
 * @param bus 总线对象，调用者必须已持有总线锁
 * @param dev 设备句柄
 * @retval 0: 已生效（sck_hz 为 0 时是回落到默认值，不是空操作）
 * @retval -1: HAL_SPI_Init 失败，外设可能停在 RESET，下一帧会在状态检查被挡住
 * @note 只在算出的分频与当前值不同时才动外设：同速率连续传输零开销，
 *       不会每帧都重配一次。改 BR 必须关掉 SPE，所以这件事只能发生在
 *       片选拉低之前——此时帧还没开始、总线空闲，正是合法时机。
 *       HAL_SPI_Init 只重写 CR1/CR2，不会重跑 MspInit（那只在 State 为
 *       RESET 时发生），所以 GPIO 与 DMA 配置不受影响。
 *       sck_hz 为 0 的设备回落到 presc_init（CubeMX 的配置），而不是"保持
 *       当前值"：否则触摸把总线降到 1.31 MHz 之后，屏会继续用这个低速，
 *       帧率掉一个数量级，而且从现象完全看不出是速率被动过。
 */
static int BSP_SPI_ApplyRate(bsp_spi_t *bus, const bsp_spi_dev_t *dev)
{
    SPI_HandleTypeDef *hspi = (SPI_HandleTypeDef *)bus->hal;
    uint32_t           code;

    if (dev->sck_hz == 0U) {
        code = bus->presc_init;    /* 没有速率要求的设备：回到 CubeMX 配置 */
    } else {
        code = BSP_SPI_PrescalerCode(BSP_SPI_DivFor(BSP_SPI_Pclk_Hz(hspi),
                                                    dev->sck_hz));
    }

    if (code == bus->presc_code) {
        return 0;   /* 与当前一致，省掉一次关/开外设 */
    }

    hspi->Init.BaudRatePrescaler = code;
    if (HAL_SPI_Init(hspi) != HAL_OK) {
        return -1;
    }

    bus->presc_code = code;
    return 0;
}

/**
 * @brief 按命令方向启动一次 HAL DMA 传输
 * @param bus 总线对象
 * @param msg 传输命令
 * @retval HAL_OK: 启动成功
 * @retval 其他: HAL 启动失败
 * @note 只由已持锁的传输路径调用；启动即返回，完成点在 HAL_SPI_*CpltCallback。
 */
static HAL_StatusTypeDef BSP_SPI_StartDma(bsp_spi_t *bus,
                                          const bsp_spi_msg_t *msg)
{
    SPI_HandleTypeDef *hspi = (SPI_HandleTypeDef *)bus->hal;

    if (msg->dir == (uint8_t)BSP_SPI_DIR_TX) {
        return HAL_SPI_Transmit_DMA(hspi, msg->tx, msg->len);
    }
    if (msg->dir == (uint8_t)BSP_SPI_DIR_RX) {
        return HAL_SPI_Receive_DMA(hspi, msg->rx, msg->len);
    }
    return HAL_SPI_TransmitReceive_DMA(hspi, msg->tx, msg->rx, msg->len);
}

/**
 * @brief 没有 DMA 时用 HAL 阻塞接口搬完
 * @param bus        总线对象
 * @param msg        传输命令
 * @param timeout_ms 本次传输的超时预算
 * @retval BSP_SPI_OK: 传输成功
 * @retval BSP_SPI_ERR_TIMEOUT: 传输超时
 * @retval BSP_SPI_ERR_HAL: HAL 返回错误
 * @note 调用者已持锁；这里只做启动 + 等完成，不碰片选。忙等，会占住 CPU。
 */
static bsp_spi_status_t BSP_SPI_RunBlocking(bsp_spi_t *bus,
                                            const bsp_spi_msg_t *msg,
                                            uint32_t timeout_ms)
{
    SPI_HandleTypeDef *hspi = (SPI_HandleTypeDef *)bus->hal;
    HAL_StatusTypeDef  st;

    if (msg->dir == (uint8_t)BSP_SPI_DIR_TX) {
        st = HAL_SPI_Transmit(hspi, msg->tx, msg->len, timeout_ms);
    } else if (msg->dir == (uint8_t)BSP_SPI_DIR_RX) {
        st = HAL_SPI_Receive(hspi, msg->rx, msg->len, timeout_ms);
    } else {
        st = HAL_SPI_TransmitReceive(hspi, msg->tx, msg->rx, msg->len,
                                     timeout_ms);
    }

    if (st == HAL_OK) {
        return BSP_SPI_OK;
    }
    if (st == HAL_TIMEOUT) {
        return BSP_SPI_ERR_TIMEOUT;
    }
    return BSP_SPI_ERR_HAL;
}

/**
 * @brief 超时后中止传输并让外设回到 READY
 * @param bus 总线对象
 * @retval 无
 * @note 仅任务上下文、已持锁时调用。Abort 自身失败（DMA 已跑飞）时用
 *       DeInit/Init 硬复位一次，保证总线能交给下一个任务。
 */
static void BSP_SPI_Abort(bsp_spi_t *bus)
{
    SPI_HandleTypeDef *hspi = (SPI_HandleTypeDef *)bus->hal;

    if (HAL_SPI_Abort(hspi) != HAL_OK) {
        (void)HAL_SPI_DeInit(hspi);
        (void)HAL_SPI_Init(hspi);
    }
}

/**
 * @brief 传输前的参数与能力检查，并把方向填进消息描述
 * @param bus 总线对象，不能为 NULL
 * @param tx  发送缓冲区，NULL 表示本次不发
 * @param rx  接收缓冲区，NULL 表示本次不收
 * @param len 传输字节数，不能为 0
 * @param msg 输出：填好的传输描述
 * @retval BSP_SPI_OK: 参数合法，msg 已填好
 * @retval BSP_SPI_ERR_PARAM: 句柄或长度非法，或两个缓冲区都为 NULL
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化，或调度器未运行
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @note 不碰锁、不碰外设。调度器检查也在这里：pxCurrentTCB 为 NULL 时
 *       后面的 ulTaskNotifyTake() 会解引用空指针。
 */
static bsp_spi_status_t BSP_SPI_Prepare(bsp_spi_t *bus, const uint8_t *tx,
                                        uint8_t *rx, uint16_t len,
                                        bsp_spi_msg_t *msg)
{
    uint8_t need_cap;

    if (bus == NULL) {
        return BSP_SPI_ERR_PARAM;
    }
    if (bus->initialized == 0U) {
        return BSP_SPI_ERR_STATE;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return BSP_SPI_ERR_STATE;
    }
    if ((len == 0U) || ((tx == NULL) && (rx == NULL))) {
        return BSP_SPI_ERR_PARAM;
    }

    msg->tx  = tx;
    msg->rx  = rx;
    msg->len = len;
    if ((tx != NULL) && (rx != NULL)) {
        msg->dir = (uint8_t)BSP_SPI_DIR_TXRX;
    } else if (tx != NULL) {
        msg->dir = (uint8_t)BSP_SPI_DIR_TX;
    } else {
        msg->dir = (uint8_t)BSP_SPI_DIR_RX;
    }

    /* 组合位按"全含"判断：TXRX 必须 TX、RX 两位都在 */
    need_cap = BSP_SPI_CapOfDir(msg->dir);
    if ((bus->caps & need_cap) != need_cap) {
        return BSP_SPI_ERR_MODE;
    }

    return BSP_SPI_OK;
}

/**
 * @brief 在已持有的总线上完成一次搬运（不抢锁、不碰片选）
 * @param bus        总线对象
 * @param msg        传输描述，由 BSP_SPI_Prepare() 填好
 * @param timeout_ms 传输预算（毫秒），不含排队，0 已在上层归一化
 * @retval BSP_SPI_OK: 传输成功
 * @retval BSP_SPI_ERR_TIMEOUT: 等完成通知超时
 * @retval BSP_SPI_ERR_BUS: 总线错误
 * @retval BSP_SPI_ERR_HAL: HAL 启动失败或 DMA 错误
 * @note 调用者必须已持有总线锁。DMA 路径睡等完成中断，无 DMA 走阻塞接口；
 *       两条路径失败都会中止并复位外设，保证总线能交给下一个任务。
 */
static bsp_spi_status_t BSP_SPI_DoTransfer(bsp_spi_t *bus,
                                           const bsp_spi_msg_t *msg,
                                           uint32_t timeout_ms)
{
    bsp_spi_status_t status;
    uint8_t          need_dma = BSP_SPI_DmaOfDir(msg->dir);
    TickType_t       timeout  = pdMS_TO_TICKS(timeout_ms);

    /* ---------- 1. 有 DMA：启动后睡等完成中断 ---------- */
    if ((bus->dma_mask & need_dma) == need_dma) {
        /* 清掉上次残留的通知，登记等待者 */
        (void)ulTaskNotifyTake(pdTRUE, 0U);
        bus->result = BSP_SPI_ERR_HAL;
        bus->waiter = xTaskGetCurrentTaskHandle();

        if (BSP_SPI_StartDma(bus, msg) != HAL_OK) {
            bus->waiter = NULL;
            BSP_SPI_Abort(bus); /* 启动失败多半是外设卡住，顺手复位 */
            return BSP_SPI_ERR_HAL;
        }

        if (ulTaskNotifyTake(pdTRUE, timeout) == 0U) {
            bus->waiter = NULL;
            BSP_SPI_Abort(bus);
            status = BSP_SPI_ERR_TIMEOUT;
        } else {
            status = bus->result;
        }
    } else {
        /* ---------- 2. 没有 DMA：直接阻塞搬完 ---------- */
        status = BSP_SPI_RunBlocking(bus, msg, timeout_ms);
        if (status != BSP_SPI_OK) {
            BSP_SPI_Abort(bus);
        }
    }

    bus->waiter = NULL;
    return status;
}

/**
 * @brief 中断收尾：记录结果并唤醒等待者
 * @param bus    总线对象
 * @param result 结果码
 * @retval 无
 * @note 只由 HAL 完成/错误回调调用。先写 result 再发通知，任务被唤醒时
 *       结果一定已落盘。waiter 为 NULL 说明已超时放弃，只记录不通知。
 */
static void BSP_SPI_CompleteFromISR(bsp_spi_t *bus, bsp_spi_status_t result)
{
    TaskHandle_t waiter = bus->waiter;
    BaseType_t   woken = pdFALSE;

    bus->result = result;

    if (waiter != NULL) {
        vTaskNotifyGiveFromISR(waiter, &woken);
    }

    portYIELD_FROM_ISR(woken);
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化 SPI 驱动：轮询总线表，登记句柄与方向能力
 * @retval BSP_SPI_OK: 初始化成功（已初始化的总线会被跳过）
 * @retval BSP_SPI_ERR_STATE: 缺 HAL 句柄或互斥量、外设非 READY、Mode 不是
 *         Master，或数据宽度不是 8 bit
 * @note 必须等 freertos.c 建完 SPI_LOCK 再调用（USER CODE BEGIN RTOS_MUTEX
 *       内），且 MX_SPIx_Init() 已执行过。不能在中断里调用。
 *       新增一路 SPI 只在 s_buses[] 追加实例，本函数不用改。
 */
bsp_spi_status_t BSP_SPI_Init(void)
{
    size_t i;

    /* ---------- 1. 逐路登记：句柄、互斥量、外设状态 ---------- */
    for (i = 0U; i < sizeof(s_buses) / sizeof(s_buses[0]); i++) {
        bsp_spi_t        *bus = s_buses[i];
        SPI_HandleTypeDef *hspi;

        if (bus->initialized != 0U) {
            continue;
        }
        if (bus->hal == NULL || bus->lock == NULL || *bus->lock == NULL) {
            return BSP_SPI_ERR_STATE;
        }

        hspi = (SPI_HandleTypeDef *)bus->hal;
        if (HAL_SPI_GetState(hspi) != HAL_SPI_STATE_READY) {
            return BSP_SPI_ERR_STATE;
        }
        /* 本层只服务主机模式：从机是"等主机给时钟"，语义与超时都不同 */
        if (hspi->Init.Mode != SPI_MODE_MASTER) {
            return BSP_SPI_ERR_STATE;
        }
        /* 本层只按字节数收发，16 bit 宽度会让 len 的语义变成半字数 */
        if (hspi->Init.DataSize != SPI_DATASIZE_8BIT) {
            return BSP_SPI_ERR_STATE;
        }

        /* ---------- 2. 记下方向能力与 DMA 能力 ---------- */
        bus->caps = BSP_SPI_CapsOf(hspi);
        bus->dma_mask = 0U;
        if (hspi->hdmatx != NULL) {
            bus->dma_mask |= BSP_SPI_DMA_TX;
        }
        if (hspi->hdmarx != NULL) {
            bus->dma_mask |= BSP_SPI_DMA_RX;
        }

        /* 记下 CubeMX 给的预分频：它既是"当前值"的初值，也是 sck_hz 为 0
           的设备要回落到的基准值。两者必须分开存——同一条总线上有设备改过
           速率之后，当前值已经不是 CubeMX 给的那个了 */
        bus->presc_code = hspi->Init.BaudRatePrescaler;
        bus->presc_init = hspi->Init.BaudRatePrescaler;

        /* 清零上一轮可能残留的运行时状态，再标记本路可用 */
        bus->waiter = NULL;
        bus->frame_owner = NULL;
        bus->frame_dev = NULL;
        bus->result = BSP_SPI_OK;
        bus->initialized = 1U;
    }
    return BSP_SPI_OK;
}

/**
 * @brief 在设备所属的已持有总线上搬一段字节（不抢锁、不碰片选）
 * @param dev        设备句柄，不能为 NULL，且必须是 Select() 选中的那台
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 本次传输的超时预算，0 表示用默认 50 ms
 * @retval BSP_SPI_OK: 传输成功
 * @retval BSP_SPI_ERR_PARAM: 句柄或长度非法，或两个缓冲区都为 NULL
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化、调度器未运行，或当前帧不是
 *         本任务、本设备开的
 * @retval BSP_SPI_ERR_TIMEOUT: 传输超时（不存在排队，只有传输）
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @retval BSP_SPI_ERR_BUS: 总线错误
 * @retval BSP_SPI_ERR_HAL: HAL 启动失败或 DMA 错误
 * @note 只给帧内分段用：调用前必须 BSP_SPI_Dev_Select() 同一个设备，与
 *       Deselect() 成对。总线由 dev->bus 取得，调用者不用单独持有总线句柄。
 *       内部校验 frame_owner 与 frame_dev，不是"本任务 + 本设备"就返回
 *       ERR_STATE，把"忘了开帧""开了别的设备的帧""跨任务误用"变成可查的
 *       错误码。
 *       方向由 tx / rx 推出：只发 / 只收 / 全双工 / ERR_PARAM。不得在中断调用。
 */
bsp_spi_status_t BSP_SPI_TransferLocked(const bsp_spi_dev_t *dev,
                                        const uint8_t *tx, uint8_t *rx,
                                        uint16_t len, uint32_t timeout_ms)
{
    bsp_spi_msg_t    msg;
    bsp_spi_status_t status;
    bsp_spi_t       *bus;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return BSP_SPI_ERR_PARAM;
    }
    bus = dev->bus;

    status = BSP_SPI_Prepare(bus, tx, rx, len, &msg);
    if (status != BSP_SPI_OK) {
        return status;
    }

    /* 帧外误用：没开帧、帧是别的任务开的，或帧开在别的设备上 */
    if ((bus->frame_owner != xTaskGetCurrentTaskHandle()) ||
        (bus->frame_dev != dev)) {
        return BSP_SPI_ERR_STATE;
    }

    if (timeout_ms == 0U) {
        timeout_ms = BSP_SPI_DEFAULT_TIMEOUT_MS;
    }

    return BSP_SPI_DoTransfer(bus, &msg, timeout_ms);
}

/**
 * @brief 取得总线所有权并拉有效片选（帧开始）
 * @param dev        设备句柄，不能为 NULL
 * @param timeout_ms 排队等锁的预算，0 表示用默认 50 ms
 * @retval BSP_SPI_OK: 已持有总线，片选已拉有效
 * @retval BSP_SPI_ERR_PARAM: 句柄或总线为空
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化、调度器未运行，或本任务已在帧中
 * @retval BSP_SPI_ERR_TIMEOUT: 等锁超时，片选没有被拉低
 * @retval BSP_SPI_ERR_HAL: 速率切换失败，片选没有被拉低
 * @note 电平按 cs_active_level 换算。抢锁成功才拉片选，不存在"CS 已低、
 *       总线还在别人手里"的窗口。与 Deselect() 成对，帧内只能调
 *       TransferLocked()。失败后不要调 Deselect。会阻塞，不得在中断中调用。
 *       本函数同时完成速率切换：句柄的 sck_hz 非 0 时按"不超过该值"算出
 *       分频并写入外设，片选拉低之前生效。因此同一总线上的两个器件可以
 *       各用各的速率，调用方不需要为此做任何事；sck_hz 为 0 则回落到
 *       CubeMX 配的预分频。
 */
bsp_spi_status_t BSP_SPI_Dev_Select(const bsp_spi_dev_t *dev,
                                    uint32_t timeout_ms)
{
    bsp_spi_t *bus;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return BSP_SPI_ERR_PARAM;
    }
    bus = dev->bus;

    if (bus->initialized == 0U) {
        return BSP_SPI_ERR_STATE;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return BSP_SPI_ERR_STATE;
    }
    /* 同一任务重复开帧是调用错误；别的任务正持帧则照常排队等锁 */
    if (bus->frame_owner == xTaskGetCurrentTaskHandle()) {
        return BSP_SPI_ERR_STATE;
    }

    if (timeout_ms == 0U) {
        timeout_ms = BSP_SPI_DEFAULT_TIMEOUT_MS;
    }
    if (osMutexAcquire(*bus->lock, pdMS_TO_TICKS(timeout_ms)) != osOK) {
        return BSP_SPI_ERR_TIMEOUT;
    }

    /* 外设状态检查放在拿到锁之后：DMA 传输期间 State 是 BUSY_TX/BUSY_RX，
       锁外的检查会把"别的任务正在合法传输"误判成"外设卡死"，第二个设备
       直接拿到 ERR_STATE 而不是排队。持锁之后总线必然空闲，此时仍不是
       READY，才是上一帧 Abort 失败的残留，值得挡住。 */
    if (HAL_SPI_GetState((SPI_HandleTypeDef *)bus->hal)
        != HAL_SPI_STATE_READY) {
        (void)osMutexRelease(*bus->lock);
        return BSP_SPI_ERR_STATE;
    }

    /* 速率按设备切换：同一总线上不同器件各要各的，切换点在片选拉低之前。
       失败时片选还没动、帧也没登记，把锁还回去即可，不能走 Deselect——
       那里会校验 frame_dev，此时它还是上一帧的残留值 */
    if (BSP_SPI_ApplyRate(bus, dev) != 0) {
        (void)osMutexRelease(*bus->lock);
        return BSP_SPI_ERR_HAL;
    }

    /* 锁到手之后才登记所有权、拉片选，两步之间不会有别的任务插进来 */
    bus->frame_owner = xTaskGetCurrentTaskHandle();
    bus->frame_dev   = dev;
    if (dev->cs != NULL) {
        BSP_GPIO_Write(dev->cs, dev->cs_active_level);
    }
    return BSP_SPI_OK;
}

/**
 * @brief 放无效片选并归还总线（帧结束）
 * @param dev 设备句柄，不能为 NULL
 * @retval 无
 * @note 与 Select() 配对。只对"本任务、本设备"开的帧生效：任务不是帧主、
 *       或 dev 不是本帧选中的那台，都直接返回，避免放掉别人的锁或抬错片选。
 *       必须校验设备：否则 Select(dev1) 后误调 Deselect(dev2) 会放锁而
 *       dev1 的 CS 仍低，接下来用 dev2 的传输会被 dev1 一并收走。
 */
void BSP_SPI_Dev_Deselect(const bsp_spi_dev_t *dev)
{
    bsp_spi_t   *bus;
    TaskHandle_t owner;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return;
    }
    bus = dev->bus;

    /* 调度器未运行时 xTaskGetCurrentTaskHandle() 返回 NULL，会和初始的
       frame_owner 撞上，所以先把 NULL 挡掉，避免误放一把没持有的锁。 */
    owner = xTaskGetCurrentTaskHandle();
    if ((owner == NULL) || (bus->frame_owner != owner) || (bus->frame_dev != dev)) {
        return;
    }

    if (dev->cs != NULL) {
        BSP_GPIO_Write(dev->cs, (dev->cs_active_level == 0U) ? 1U : 0U);
    }
    bus->frame_dev   = NULL;
    bus->frame_owner = NULL;
    (void)osMutexRelease(*bus->lock);
}

/**
 * @brief 在指定设备上提交一次传输，自动拉放片选
 * @param dev        设备句柄，不能为 NULL
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 超时预算，0 表示用默认 50 ms
 * @retval BSP_SPI_OK: 传输成功
 * @retval BSP_SPI_ERR_PARAM: 句柄或长度非法，或两个缓冲区都为 NULL
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化
 * @retval BSP_SPI_ERR_TIMEOUT: 排队或传输超时
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @retval BSP_SPI_ERR_BUS: 总线错误
 * @retval BSP_SPI_ERR_HAL: HAL 错误
 * @note 一次完整帧事务：Select → TransferLocked → Deselect，成功失败都
 *       把片选与总线还回去。方向推断规则与 TransferLocked() 一致。
 *       一帧要分多段发时别用本函数，改用三个底层函数自己拼。
 */
bsp_spi_status_t BSP_SPI_Dev_Transfer(const bsp_spi_dev_t *dev,
                                      const uint8_t *tx, uint8_t *rx,
                                      uint16_t len, uint32_t timeout_ms)
{
    bsp_spi_status_t status;

    /* ---------- 1. 抢总线并拉片选，两步合成一个原子动作 ---------- */
    status = BSP_SPI_Dev_Select(dev, timeout_ms);
    if (status != BSP_SPI_OK) {
        return status;
    }

    /* ---------- 2. 搬字节，成功失败都要把片选与总线还回去 ---------- */
    status = BSP_SPI_TransferLocked(dev, tx, rx, len, timeout_ms);
    BSP_SPI_Dev_Deselect(dev);
    return status;
}

/*==============================================================================
 * HAL 回调覆盖
 *
 * HAL 只为 Transmit_DMA 调 TxCplt、只为 Receive_DMA 调 RxCplt、只为
 * TransmitReceive_DMA 调 TxRxCplt，三个完成点不能互相替代。
 * 这里覆盖的是 HAL 的 __weak 完成回调，不是 SPI2_IRQHandler 本身；
 * IRQ 向量仍由 Src/stm32f4xx_it.c 的 HAL_SPI_IRQHandler() 分发。
 *============================================================================*/
/**
 * @brief 发送完成回调
 * @param hspi HAL SPI 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；DIR_TX 路径的完成点。
 */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    bsp_spi_t *bus = BSP_SPI_FindBus(hspi);

    if (bus != NULL) {
        BSP_SPI_CompleteFromISR(bus, BSP_SPI_OK);
    }
}

/**
 * @brief 接收完成回调
 * @param hspi HAL SPI 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；DIR_RX 路径的完成点。
 */
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    bsp_spi_t *bus = BSP_SPI_FindBus(hspi);

    if (bus != NULL) {
        BSP_SPI_CompleteFromISR(bus, BSP_SPI_OK);
    }
}

/**
 * @brief 全双工收发完成回调
 * @param hspi HAL SPI 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；DIR_TXRX 路径的完成点。
 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    bsp_spi_t *bus = BSP_SPI_FindBus(hspi);

    if (bus != NULL) {
        BSP_SPI_CompleteFromISR(bus, BSP_SPI_OK);
    }
}

/**
 * @brief 错误回调：把 HAL ErrorCode 翻译成 BSP 返回码
 * @param hspi HAL SPI 句柄
 * @note MODF（多主机冲突）/ OVR（溢出丢字节）/ CRC 都算总线问题；
 *       DMA 与 FLAG 类错误归到 HAL。主机模式下 MODF 说明 NSS 被外部
 *       拉低（总线上还有第二个主机），复位外设即可恢复。
 */
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    bsp_spi_t       *bus = BSP_SPI_FindBus(hspi);
    bsp_spi_status_t result;

    if (bus == NULL) {
        return;
    }

    if ((hspi->ErrorCode &
         (HAL_SPI_ERROR_MODF | HAL_SPI_ERROR_OVR | HAL_SPI_ERROR_CRC)) != 0U) {
        result = BSP_SPI_ERR_BUS;
    } else {
        result = BSP_SPI_ERR_HAL;
    }

    BSP_SPI_CompleteFromISR(bus, result);
}
