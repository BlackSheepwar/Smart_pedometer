/**
 * @file bsp_spi.c
 * @brief SPI 硬件驱动层：DMA/阻塞字节搬运、分频切换、错误恢复
 * @author Ricardo
 * @date 2026-11-20
 *
 * @note 设计模型：
 *       1. 本层不持锁、不碰片选、不登记"当前帧"。总线独占与片选时序由
 *          设备层 Dev_SPI 负责，本层只接受"已经轮到我、片选已低"这个前提。
 *       2. 搬运方式在编译期由 BSP_SPI_TRANSFER_MODE 三选一：阻塞方式在
 *          函数里同步搬完；中断与 DMA 方式启动即返回，完成或出错时在中断
 *          里回调调用者给的回调，两者对外契约完全相同。本层不等待、不
 *          通知、不依赖 FreeRTOS，裸机也能直接用。超时由调用者判，判超时
 *          后调 BSP_SPI_Abort()；Abort 自己也失败就 DeInit/Init 硬复位
 *          一次，保证总线能被下一次调用接手。
 *       3. 分频表是芯片事实，留在这里；"哪台设备要多快"由 Dev_SPI 传进来，
 *          本层不识别设备。总线句柄里的输入时钟是配置项，调用路径不读 RCC。
 *       4. 运行期状态（忙标志、回调登记、当前分频）放在内部 rt 对象里，
 *          总线句柄本身 const 进 .rodata，与框架的配置型句柄约定一致。
 *          方向能力与 DMA 能力也是句柄的配置项，Init 时逐项核对 HAL 实配。
 *
 *       HAL 回调是强符号，靠 BSP_SPI_Init() 把本目标拉进镜像。
 *       外设的 SPI 中断必须开：中断与 DMA 两种方式都靠 SPIx_IRQHandler
 *       推进，关掉之后完成点永远等不到。
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

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单次调用默认超时（ms） */
#define BSP_SPI_DEFAULT_TIMEOUT_MS  50U

/** @brief 方向能力位；TXRX 是 TX|RX 的组合位，不是第三个独立位
 *  @note 检查统一用 (caps & need) == need，组合位与单个位同一套判断。 */
#define BSP_SPI_CAP_TX    0x01U /* 可只发 */
#define BSP_SPI_CAP_RX    0x02U /* 可只收 */
#define BSP_SPI_CAP_TXRX  (BSP_SPI_CAP_TX | BSP_SPI_CAP_RX) /* 可全双工 */

/** @brief DMA 能力位；TX / RX 是两个独立硬件事实，TXRX 是组合表达式
 *  @note 全双工要求收发两路都在，缺一路就整体退回阻塞模式。 */
#define BSP_SPI_DMA_TX    0x01U /* 发送 DMA 已配置 */
#define BSP_SPI_DMA_RX    0x02U /* 接收 DMA 已配置 */
#define BSP_SPI_DMA_TXRX  (BSP_SPI_DMA_TX | BSP_SPI_DMA_RX) /* 两路都在 */

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
 * @brief 总线的运行期状态（内部类型，不进公共头文件）
 * @note 句柄本身 const，可变部分全在这里：中断写忙标志、任务登记回调、
 *       当前分频。presc_init 是 CubeMX 给的基准，presc_code 是当前生效值，
 *       两者必须分开存——设备改过速率之后当前值已经不是基准值了。
 *       done / ctx 用 volatile：任务写、中断读，中间没有同步动作。
 */
typedef struct bsp_spi_rt {
    bsp_spi_done_t volatile done;      /* 本次传输的完成回调，空闲为 NULL */
    void *volatile     ctx;            /* 回调上下文，与 done 同生同灭 */
    volatile uint8_t  busy;            /* 1: 异步传输尚未收尾 */
    volatile uint8_t  initialized;     /* 1: 已完成初始化 */
    uint32_t          presc_code;      /* 当前生效的预分频，HAL 的 BR 编码 */
    uint32_t          presc_init;      /* 回落基准：CubeMX 给的预分频 */
} bsp_spi_rt_t;

/**
 * @brief SPI 总线对象（BSP 内部类型，不对外暴露；上层通过 Dev_SPI 使用）
 * @note hal 用 void *，避免头文件引入 HAL；rt 指向运行期状态，句柄保持
 *       const 才能进 .rodata。caps 与 dma_mask 是配置：它们描述这条总线
 *       能做什么，Init 时与 HAL 实配核对，运行期不改。
 */
struct bsp_spi {
    void          *hal;      /* HAL SPI 句柄（SPI_HandleTypeDef *） */
    uint32_t       pclk_hz;  /* 该外设的输入时钟（Hz），配置型 */
    uint8_t        caps;     /* 方向能力位，见 BSP_SPI_CAP_* */
    uint8_t        dma_mask; /* DMA 能力位，见 BSP_SPI_DMA_* */
    bsp_spi_rt_t  *rt;       /* 运行期状态，定义在本文件内 */
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
/** @brief SPI1 的运行期状态，仅本文件可见 */
static bsp_spi_rt_t bsp_spi1_rt = {
    .done        = NULL,
    .ctx         = NULL,
    .busy        = 0U,
    .initialized = 0U,
    .presc_code  = 0U,
    .presc_init  = 0U,
};

/**
 * @brief SPI1 总线句柄：这条总线怎么工作的四项配置都在这里
 * @note pclk_hz、caps、dma_mask 就是移植时要跟着改的三项：照 .ioc 的时钟树
 *       填该外设所属 APB 的实际频率，方向与 DMA 照该外设的配置填。分频计算
 *       只拿 pclk_hz 做除法，不去 RCC 现取；Init 会把三项与 HAL 实配核对，
 *       对不上就拒绝初始化，不会带着错误的能力位跑。
 *       之所以写数值而不做宏：宏要在文件头改一次、实例里再引用一次，两处
 *       必须同改，漏一处就是静默错速；直接写在实例里，改的就是看得见的
 *       那一行。填错 pclk_hz 不会报错，只会让实际 SCK 与请求值成倍偏差，
 *       现象是"降速后仍然读不通"，所以必须按参考手册的时钟树核对，不能估。
 */
const bsp_spi_t bsp_spi1 = {
    .hal      = &hspi1,
    .pclk_hz  = 84000000U,          /* APB2 = HCLK / 2，HCLK 168 MHz */
    .caps     = BSP_SPI_CAP_TXRX,   /* 2LINES：可只发、可只收、可全双工 */
    .dma_mask = BSP_SPI_DMA_TXRX,   /* 发送、接收两路 DMA 都已配置 */
    .rt       = &bsp_spi1_rt,
};

/** @brief 驱动管理的总线表：初始化和中断反查都按它轮询 */
static const bsp_spi_t *const s_buses[] = {
    &bsp_spi1,
};

/** @brief 总线表长度：由表的行数算出 */
static const size_t s_bus_count = sizeof(s_buses) / sizeof(s_buses[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 按 HAL 句柄查找对应的总线对象
 * @param hspi HAL SPI 句柄
 * @retval 非 NULL: 对应的总线对象
 * @retval NULL: 不是本驱动管理的句柄
 */
static const bsp_spi_t *BSP_SPI_FindBus(SPI_HandleTypeDef *hspi)
{
    size_t i;

    if (hspi == NULL) {
        return NULL;
    }

    for (i = 0U; i < s_bus_count; i++) {
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
    return (uint8_t)BSP_SPI_CAP_TXRX; /* 2LINES：三种方向都能做 */
}

/**
 * @brief 由 HAL 句柄里挂接的 DMA 通道推出本总线的 DMA 能力
 * @param hspi HAL SPI 句柄
 * @retval DMA 能力位组合，见 BSP_SPI_DMA_*
 * @note 句柄里有哪个方向的 DMA 流就算哪一路可用；两路都没有就是 0。
 */
static uint8_t BSP_SPI_DmaOf(const SPI_HandleTypeDef *hspi)
{
    uint8_t mask = 0U;

    if (hspi->hdmatx != NULL) {
        mask |= BSP_SPI_DMA_TX;
    }
    if (hspi->hdmarx != NULL) {
        mask |= BSP_SPI_DMA_RX;
    }
    return mask;
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

#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_DMA)
/**
 * @brief 把传输方向翻译成需要的 DMA 通道位
 * @param dir 方向，取 bsp_spi_dir_t
 * @retval 对应的 BSP_SPI_DMA_* 位组合
 * @note 全双工要收发两路 DMA 都在，缺一路就整体退回阻塞方式。
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
#endif /* BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_DMA */

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

#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_DMA)
/**
 * @brief 按命令方向启动一次 HAL DMA 传输
 * @param bus 总线对象
 * @param msg 传输命令
 * @retval HAL_OK: 启动成功
 * @retval 其他: HAL 启动失败
 * @note 只由传输路径调用；启动即返回，完成点在 HAL_SPI_*CpltCallback。
 */
static HAL_StatusTypeDef BSP_SPI_StartDma(const bsp_spi_t *bus,
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
#endif /* BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_DMA */

#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_IT)
/**
 * @brief 按命令方向启动一次 HAL 中断传输
 * @param bus 总线对象
 * @param msg 传输命令
 * @retval HAL_OK: 启动成功
 * @retval 其他: HAL 启动失败
 * @note 只由传输路径调用；启动即返回，完成点与 DMA 方式共用同一组
 *       HAL_SPI_*CpltCallback。
 */
static HAL_StatusTypeDef BSP_SPI_StartIt(const bsp_spi_t *bus,
                                         const bsp_spi_msg_t *msg)
{
    SPI_HandleTypeDef *hspi = (SPI_HandleTypeDef *)bus->hal;

    if (msg->dir == (uint8_t)BSP_SPI_DIR_TX) {
        return HAL_SPI_Transmit_IT(hspi, msg->tx, msg->len);
    }
    if (msg->dir == (uint8_t)BSP_SPI_DIR_RX) {
        return HAL_SPI_Receive_IT(hspi, msg->rx, msg->len);
    }
    return HAL_SPI_TransmitReceive_IT(hspi, msg->tx, msg->rx, msg->len);
}
#endif /* BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_IT */

#if (BSP_SPI_TRANSFER_MODE != BSP_SPI_MODE_IT)
/**
 * @brief 用 HAL 阻塞接口搬完
 * @param bus        总线对象
 * @param msg        传输命令
 * @param timeout_ms 本次传输的超时预算
 * @retval BSP_SPI_OK: 传输成功
 * @retval BSP_SPI_ERR_TIMEOUT: 传输超时
 * @retval BSP_SPI_ERR_HAL: HAL 返回错误
 * @note 调用者已持总线；这里只做启动 + 等完成，不碰片选。忙等，会占住 CPU。
 *       阻塞方式用它搬字节；DMA 方式下缺通道时退回它也走这里。
 */
static bsp_spi_status_t BSP_SPI_RunBlocking(const bsp_spi_t *bus,
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
#endif /* BSP_SPI_TRANSFER_MODE != BSP_SPI_MODE_IT */

/**
 * @brief 中止当前传输、清掉回调登记，并让外设回到 READY
 * @param bus 总线对象，不能为 NULL
 * @retval BSP_SPI_OK: 已回到空闲
 * @retval BSP_SPI_ERR_PARAM: 总线或运行期对象为空
 * @retval BSP_SPI_ERR_HAL: HAL_SPI_Abort 失败，已用 DeInit/Init 硬复位
 * @note 仅任务上下文、已持总线时调用。Abort 自身失败（DMA 已跑飞）时用
 *       DeInit/Init 硬复位一次，保证总线能交给下一个调用者。
 *       先撤回调登记再中止：之后哪怕完成中断迟到，也不会再惊动调用者；
 *       忙标志也在这里清掉，避免一个"永远不会来"的完成把总线永久占住。
 */
bsp_spi_status_t BSP_SPI_Abort(const bsp_spi_t *bus)
{
    SPI_HandleTypeDef *hspi;

    if ((bus == NULL) || (bus->rt == NULL)) {
        return BSP_SPI_ERR_PARAM;
    }

    bus->rt->done = NULL;
    bus->rt->ctx  = NULL;
    bus->rt->busy = 0U;

    hspi = (SPI_HandleTypeDef *)bus->hal;
    if (HAL_SPI_Abort(hspi) != HAL_OK) {
        (void)HAL_SPI_DeInit(hspi);
        (void)HAL_SPI_Init(hspi);
        return BSP_SPI_ERR_HAL;
    }
    return BSP_SPI_OK;
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
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @note 不碰外设，也不查调度器状态——本层不认识"任务"这个概念，
 *       没初始化就启动 DMA 才真正危险，所以只挡这一条。
 */
static bsp_spi_status_t BSP_SPI_Prepare(const bsp_spi_t *bus,
                                        const uint8_t *tx, uint8_t *rx,
                                        uint16_t len, bsp_spi_msg_t *msg)
{
    uint8_t need_cap;

    if ((bus == NULL) || (bus->rt == NULL)) {
        return BSP_SPI_ERR_PARAM;
    }
    if (bus->rt->initialized == 0U) {
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
 * @brief 启动一次搬运：中断 / DMA 方式异步返回，阻塞方式同步搬完
 * @param bus        总线对象，不能为 NULL
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 超时预算（毫秒），只对阻塞方式生效，0 视为 50 ms
 * @param done       异步完成回调，可为 NULL
 * @param ctx        回调上下文，原样传回 done
 * @retval BSP_SPI_OK: 异步方式已启动（尚未完成）；阻塞方式已完成且成功
 * @retval BSP_SPI_ERR_PARAM: 句柄、长度或方向非法
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @retval BSP_SPI_ERR_TIMEOUT: 阻塞方式等待超时
 * @retval BSP_SPI_ERR_HAL: 启动失败，或阻塞方式传输出错
 * @retval BSP_SPI_ERR_BUS: 阻塞方式遇到总线错误
 * @note 调用前总线必须已由 Dev_SPI 取得、片选已低、速率已切好。
 *       异步方式返回 OK 时传输尚未完成，完成点一定在 done 里；阻塞方式
 *       不调用 done，返回值就是最终结果。启动失败顺手 Abort，避免外设
 *       卡在 BUSY 把后面的传输全堵死。
 *       DMA 方式下若本方向缺 DMA 通道，退回阻塞方式同步搬完。
 */
bsp_spi_status_t BSP_SPI_StartTransfer(const bsp_spi_t *bus,
                                       const uint8_t *tx, uint8_t *rx,
                                       uint16_t len, uint32_t timeout_ms,
                                       bsp_spi_done_t done, void *ctx)
{
    bsp_spi_rt_t     *rt;
    bsp_spi_msg_t     msg;
    bsp_spi_status_t  status;
#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_DMA)
    uint8_t           need_dma;
#endif
#if (BSP_SPI_TRANSFER_MODE != BSP_SPI_MODE_BLOCKING)
    HAL_StatusTypeDef hal_status;
#endif

    status = BSP_SPI_Prepare(bus, tx, rx, len, &msg);
    if (status != BSP_SPI_OK) {
        return status;
    }
    rt = bus->rt;

#if (BSP_SPI_TRANSFER_MODE != BSP_SPI_MODE_BLOCKING)
    if (timeout_ms == 0U) {
        timeout_ms = BSP_SPI_DEFAULT_TIMEOUT_MS;
    }
#endif

#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_BLOCKING)
    /* 阻塞方式：同步搬完，done 在这条路径上不会被调用 */
    (void)done;
    (void)ctx;
    rt->done = NULL;
    rt->ctx  = NULL;
    rt->busy = 0U;
    status = BSP_SPI_RunBlocking(bus, &msg, timeout_ms);
    if (status != BSP_SPI_OK) {
        (void)BSP_SPI_Abort(bus);
    }
    return status;
#else
#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_IT)
    (void)timeout_ms; /* 中断方式自己会结束，超时由调用者用 Abort 兜底 */
#endif

#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_DMA)
    need_dma = BSP_SPI_DmaOfDir(msg.dir);
    if ((bus->dma_mask & need_dma) != need_dma) {
        /* 这条总线缺本次方向的 DMA 通道：退回阻塞，同步返回 */
        rt->done = NULL;
        rt->ctx  = NULL;
        rt->busy = 0U;
        status = BSP_SPI_RunBlocking(bus, &msg, timeout_ms);
        if (status != BSP_SPI_OK) {
            (void)BSP_SPI_Abort(bus);
        }
        return status;
    }
#endif

    /* 先登记回调再启动：中断可能紧跟着就来，晚登记会丢一次完成 */
    rt->done = done;
    rt->ctx  = ctx;
    rt->busy = 1U;

#if (BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_DMA)
    hal_status = BSP_SPI_StartDma(bus, &msg);
#else
    hal_status = BSP_SPI_StartIt(bus, &msg);
#endif
    if (hal_status != HAL_OK) {
        /* 启动就失败：撤掉登记、复位外设，结果由返回值直接给出 */
        (void)BSP_SPI_Abort(bus);
        return BSP_SPI_ERR_HAL;
    }
    return BSP_SPI_OK; /* 尚未完成：等 done，或轮询 IsBusy() */
#endif /* BSP_SPI_TRANSFER_MODE == BSP_SPI_MODE_BLOCKING */
}

/**
 * @brief 中断收尾：清忙标志、撤回调登记、把结果交给调用者
 * @param bus    总线对象
 * @param result 结果码
 * @retval 无
 * @note 只由 HAL 完成/错误回调调用。先把回调取到局部变量再清登记：
 *       回调里可能立刻发起下一次传输，登记会被重写，不能让本层的清理
 *       反过来踩掉它。done 为 NULL 说明调用者没给回调，或者已经超时
 *       Abort 掉了，此时只清忙标志、不通知。
 */
static void BSP_SPI_CompleteFromISR(const bsp_spi_t *bus,
                                    bsp_spi_status_t result)
{
    bsp_spi_rt_t   *rt   = bus->rt;
    bsp_spi_done_t  done = rt->done;
    void           *ctx  = rt->ctx;

    rt->busy = 0U;
    rt->done = NULL;
    rt->ctx  = NULL;

    if (done != NULL) {
        done(ctx, result);
    }
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化 SPI 驱动：轮询总线表，核对句柄配置与 HAL 实配
 * @retval BSP_SPI_OK: 初始化成功（已初始化的总线会被跳过）
 * @retval BSP_SPI_ERR_PARAM: 句柄漏填输入时钟（pclk_hz 为 0），或句柄登记的
 *         方向能力 / DMA 能力与 HAL 实配不一致
 * @retval BSP_SPI_ERR_STATE: 缺 HAL 句柄、外设非 READY、Mode 不是 Master，
 *         或数据宽度不是 8 bit
 * @note 必须 MX_SPIx_Init() 已执行过，且不能在中断里调用。总线互斥量
 *       不属于本层，Dev_SPI_Init() 在它之后单独检查自己的锁。
 */
bsp_spi_status_t BSP_SPI_Init(void)
{
    size_t i;

    /* ---------- 1. 逐路登记：句柄、外设状态 ---------- */
    for (i = 0U; i < s_bus_count; i++) {
        const bsp_spi_t  *bus = s_buses[i];
        bsp_spi_rt_t     *rt  = bus->rt;
        SPI_HandleTypeDef *hspi;

        if (rt->initialized != 0U) {
            continue;
        }
        if (bus->hal == NULL) {
            return BSP_SPI_ERR_STATE;
        }
        /* 输入时钟必须已在句柄里填好：它是分频计算的唯一依据，空了就没法
           算速率，而且这类缺失在运行期只表现为"SCK 不对"，很难查 */
        if (bus->pclk_hz == 0U) {
            return BSP_SPI_ERR_PARAM;
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

        /* ---------- 2. 核对句柄登记的能力与 HAL 实配 ---------- */
        /* 两项都是事实：句柄写的是"这条总线能做什么"，HAL 给的是"CubeMX 实际
           配了什么"。只要有一项不等，后面就可能在运行期返回来源不明的
           ERR_HAL / ERR_MODE，所以在这里拒绝初始化，不带着错误能力位往下走 */
        if ((bus->caps != BSP_SPI_CapsOf(hspi)) ||
            (bus->dma_mask != BSP_SPI_DmaOf(hspi))) {
            return BSP_SPI_ERR_PARAM;
        }

        /* 记下 CubeMX 给的预分频：它既是"当前值"的初值，也是请求速率为
           0 时回落到的基准值。两者必须分开存——有设备改过速率之后，
           当前值已经不是 CubeMX 给的那个了 */
        rt->presc_code = hspi->Init.BaudRatePrescaler;
        rt->presc_init = hspi->Init.BaudRatePrescaler;

        /* 清零上一轮可能残留的运行时状态，再标记本路可用 */
        rt->done = NULL;
        rt->ctx  = NULL;
        rt->busy = 0U;
        rt->initialized = 1U;
    }
    return BSP_SPI_OK;
}

/**
 * @brief 把总线 SCK 切换到"不超过请求值"的档位
 * @param bus    总线句柄，不能为 NULL
 * @param sck_hz 请求的 SCK 上限（Hz），0 表示回落到 CubeMX 配的预分频
 * @retval BSP_SPI_OK: 已生效（与当前档位相同时是零开销的空操作）
 * @retval BSP_SPI_ERR_PARAM: 总线句柄为空，或句柄里没填输入时钟
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化
 * @retval BSP_SPI_ERR_HAL: HAL_SPI_Init 失败
 * @note 只在算出的分频与当前值不同时才动外设：同速率连续传输零开销。
 *       分频由句柄里的 pclk_hz 算出，不在调用路径里读 RCC——输入时钟是
 *       板级配置，不是每次调用都要问一次的运行期事实。
 *       改 BR 必须关掉 SPE，所以只能在总线空闲、片选全高时调用；
 *       调用者（Dev_SPI 的 Select）负责保证这个时机，本层不做检查。
 *       HAL_SPI_Init 只重写 CR1/CR2，不会重跑 MspInit（那只在 State 为
 *       RESET 时发生），所以 GPIO 与 DMA 配置不受影响。
 */
bsp_spi_status_t BSP_SPI_SetRate(const bsp_spi_t *bus, uint32_t sck_hz)
{
    SPI_HandleTypeDef *hspi;
    bsp_spi_rt_t      *rt;
    uint32_t           code;

    if ((bus == NULL) || (bus->rt == NULL)) {
        return BSP_SPI_ERR_PARAM;
    }
    rt = bus->rt;
    if (rt->initialized == 0U) {
        return BSP_SPI_ERR_STATE;
    }
    if (bus->pclk_hz == 0U) {
        return BSP_SPI_ERR_PARAM; /* 句柄漏填输入时钟，算出来的分频必然错 */
    }

    hspi = (SPI_HandleTypeDef *)bus->hal;
    if (sck_hz == 0U) {
        code = rt->presc_init; /* 没有速率要求：回到 CubeMX 配置 */
    } else {
        code = BSP_SPI_PrescalerCode(BSP_SPI_DivFor(bus->pclk_hz, sck_hz));
    }

    if (code == rt->presc_code) {
        return BSP_SPI_OK; /* 与当前一致，省掉一次关/开外设 */
    }

    hspi->Init.BaudRatePrescaler = code;
    if (HAL_SPI_Init(hspi) != HAL_OK) {
        return BSP_SPI_ERR_HAL;
    }

    rt->presc_code = code;
    return BSP_SPI_OK;
}

/**
 * @brief 查询总线上是否还有未收尾的传输
 * @param bus 总线句柄，不能为 NULL
 * @retval 1: 异步传输还在进行
 * @retval 0: 空闲，或句柄为空
 * @note 没给 done 回调的调用者用它轮询完成；给了回调通常不必再查。
 *       阻塞方式在 StartTransfer() 返回时已完成，这里恒为 0；
 *       异步方式也可能在查询之前就跑完，所以"返回 0"不等于"没启动"。
 */
uint8_t BSP_SPI_IsBusy(const bsp_spi_t *bus)
{
    if ((bus == NULL) || (bus->rt == NULL)) {
        return 0U;
    }
    return bus->rt->busy;
}

/*==============================================================================
 * HAL 回调覆盖
 *
 * HAL 只为 Transmit / Transmit_IT / Transmit_DMA 调 TxCplt，为接收侧三套
 * 接口调 RxCplt，为全双工三套接口调 TxRxCplt，三个完成点不能互相替代。
 * 这里覆盖的是 HAL 的 __weak 完成回调，不是 SPI 中断入口本身；
 * IRQ 向量仍由 Src/stm32f4xx_it.c 的 HAL_SPI_IRQHandler() 分发。
 *============================================================================*/
/**
 * @brief 发送完成回调
 * @param hspi HAL SPI 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；DIR_TX 路径的完成点。
 */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    const bsp_spi_t *bus = BSP_SPI_FindBus(hspi);

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
    const bsp_spi_t *bus = BSP_SPI_FindBus(hspi);

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
    const bsp_spi_t *bus = BSP_SPI_FindBus(hspi);

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
    const bsp_spi_t *bus = BSP_SPI_FindBus(hspi);
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
