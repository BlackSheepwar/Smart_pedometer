/**
 * @file bsp_spi.c
 * @brief SPI 硬件驱动层：阻塞/中断/DMA 字节搬运、分频切换、错误恢复
 * @author Ricardo
 * @date 2026-11-20
 *
 * @note 设计模型：
 *       1. 本层不持锁、不碰片选、不登记"当前帧"。总线独占与片选时序由
 *          设备层 Dev_SPI 负责，本层只接受"已经轮到我、片选已低"这个前提。
 *       2. 搬运方式按两路能力分档（0 无功能 / 1 阻塞 / 2 中断 / 3 DMA），
 *          能力在 Init 时从 HAL 句柄探测：方向允不允许、有没有挂 DMA 流，
 *          结果记在 rt 里。每笔传输按该方向的档位走；启动失败才向下退一档，
 *          退到阻塞档就在函数里同步搬完。中断与 DMA 档启动即返回，完成或
 *          出错时在中断里回调，对外契约完全相同。本层不等待、不通知、不依赖
 *          FreeRTOS，裸机也能直接用。超时由调用者判，判超时后调
 *          BSP_SPI_Abort()；Abort 自己也失败就 DeInit/Init 硬复位一次。
 *       3. 分频表是芯片事实，留在这里；"哪台设备要多快"由 Dev_SPI 传进来，
 *          本层不识别设备。总线句柄里的输入时钟是配置项，调用路径不读 RCC。
 *       4. 运行期状态（忙标志、回调登记、当前分频、两路探测结果）放在内部
 *          rt 对象里，总线句柄本身 const 进 .rodata，与框架的配置型句柄
 *          约定一致。
 *
 *       外设的 SPI 中断必须开：中断与 DMA 两档都靠 SPIx_IRQHandler 推进，
 *       关掉之后完成点永远等不到。
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
typedef struct {
    bsp_spi_done_t volatile done;      /* 本次传输的完成回调，空闲为 NULL */
    void *volatile     ctx;            /* 回调上下文，与 done 同生同灭 */
    volatile uint8_t  busy;            /* 1: 异步传输尚未收尾 */
    volatile uint8_t  initialized;     /* 1: 已完成初始化 */
    uint8_t           tx_mode;         /* 探测出的发送方式，见 BSP_SPI_CAP_* */
    uint8_t           rx_mode;         /* 探测出的接收方式，见 BSP_SPI_CAP_* */
    uint32_t          presc_code;      /* 当前生效的预分频，HAL 的 BR 编码 */
    uint32_t          presc_init;      /* 回落基准：CubeMX 给的预分频 */
} bsp_spi_rt_t;

/**
 * @brief SPI 总线对象（BSP 内部类型，不对外暴露；上层通过 Dev_SPI 使用）
 * @note hal 用 void *，避免头文件引入 HAL；rt 指向运行期状态，句柄保持
 *       const 才能进 .rodata。这里只放配置，两路能怎么搬由 Init 探测。
 */
struct bsp_spi {
    void          *hal;      /* HAL SPI 句柄（SPI_HandleTypeDef *） */
    uint32_t       pclk_hz;  /* 该外设的输入时钟（Hz），配置型 */
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
    .tx_mode     = 0U,
    .rx_mode     = 0U,
    .presc_code  = 0U,
    .presc_init  = 0U,
};

/**
 * @brief SPI1 总线句柄：这条总线怎么工作的配置都在这里
 * @note pclk_hz 是移植时要跟着改的一项：照 .ioc 的时钟树填该外设所属 APB
 *       的实际频率。收发两路能怎么搬不写在这里，Init 从 HAL 句柄探测：
 *       方向允许不允许、有没有挂 DMA 流，结果记进 rt。这样换板子改的是
 *       CubeMX 的配置，代码里不用再抄一遍能力值。
 *       分频计算只拿 pclk_hz 做除法，不去 RCC 现取。
 *       之所以写数值而不做宏：宏要在文件头改一次、实例里再引用一次，两处
 *       必须同改，漏一处就是静默错速；直接写在实例里，改的就是看得见的
 *       那一行。填错 pclk_hz 不会报错，只会让实际 SCK 与请求值成倍偏差，
 *       现象是"降速后仍然读不通"，所以必须按参考手册的时钟树核对，不能估。
 */
const bsp_spi_t bsp_spi1 = {
    .hal     = &hspi1,
    .pclk_hz = 84000000U,           /* APB2 = HCLK / 2，HCLK 168 MHz */
    .rt      = &bsp_spi1_rt,
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
 * @brief 由 HAL 实配推出发送方向真正能做到的能力档位
 * @param hspi HAL SPI 句柄
 * @retval BSP_SPI_CAP_* 之一
 * @note 阻塞与中断都走 HAL 的软件路径，方向被允许就存在；DMA 还要句柄上
 *       真的挂了发送流。1LINE 只有 MOSI 一根线，能发；2LINES_RXONLY 与
 *       2LINES 之外的方向在 HAL 的主机发送接口里不被接受。
 */
static uint8_t BSP_SPI_ActualTx(const SPI_HandleTypeDef *hspi)
{
    if ((hspi->Init.Direction != SPI_DIRECTION_2LINES) &&
        (hspi->Init.Direction != SPI_DIRECTION_1LINE)) {
        return BSP_SPI_CAP_NONE;
    }
    return (hspi->hdmatx != NULL) ? BSP_SPI_CAP_DMA : BSP_SPI_CAP_IT;
}

/**
 * @brief 由 HAL 实配推出接收方向真正能做到的能力档位
 * @param hspi HAL SPI 句柄
 * @retval BSP_SPI_CAP_* 之一
 * @note 收必须 2LINES：HAL 的 Receive / Receive_IT / Receive_DMA 都只接受
 *       2LINES，RXONLY 方向虽然硬件上只收，但那些接口直接拒绝它，所以按
 *       "无功能"处理；DMA 还要句柄上挂了接收流。
 */
static uint8_t BSP_SPI_ActualRx(const SPI_HandleTypeDef *hspi)
{
    if (hspi->Init.Direction != SPI_DIRECTION_2LINES) {
        return BSP_SPI_CAP_NONE;
    }
    return (hspi->hdmarx != NULL) ? BSP_SPI_CAP_DMA : BSP_SPI_CAP_IT;
}

/**
 * @brief 取两个能力档位中较低的一档
 * @param a 档位一，见 BSP_SPI_CAP_*
 * @param b 档位二，见 BSP_SPI_CAP_*
 * @retval 较低的档位
 * @note 全双工要两路都能做到，取收发两路的小值。
 */
static uint8_t BSP_SPI_Lesser(uint8_t a, uint8_t b)
{
    return (a < b) ? a : b;
}

/**
 * @brief 探测两路实际能做到的搬运方式，记进运行期对象
 * @param bus  总线对象，不能为 NULL
 * @param hspi HAL SPI 句柄
 * @retval 无
 * @note 只读 HAL 的实配，不接受任何人工登记值：方向由 Init.Direction 决定，
 *       DMA 由句柄上挂没挂流决定。探测结果只写 rt，句柄保持 const。
 */
static void BSP_SPI_DetectModes(const bsp_spi_t *bus,
                                const SPI_HandleTypeDef *hspi)
{
    bus->rt->tx_mode = BSP_SPI_ActualTx(hspi);
    bus->rt->rx_mode = BSP_SPI_ActualRx(hspi);
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
        /* 中止本身就失败：说明 DMA 已经跑飞，只能关掉重开一次 */
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
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化，或上一笔异步传输还没收尾
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @note 不碰外设，也不查调度器状态——本层不认识"任务"这个概念，
 *       没初始化就启动 DMA 才真正危险，所以只挡这一条。
 */
static bsp_spi_status_t BSP_SPI_Prepare(const bsp_spi_t *bus,
                                        const uint8_t *tx, uint8_t *rx,
                                        uint16_t len, bsp_spi_msg_t *msg)
{
    if ((bus == NULL) || (bus->rt == NULL)) {
        return BSP_SPI_ERR_PARAM;
    }
    if (bus->rt->initialized == 0U) {
        return BSP_SPI_ERR_STATE;
    }
    /* 上一笔还在进行时拒绝：登记只有一份，第二笔会把第一笔的回调顶掉，
       第一笔的完成就无人接收，表现为"数据少了一帧"这种难查的错 */
    if (bus->rt->busy != 0U) {
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

    /* 按方向分别判：某一路定档为"无功能"就是本次传输做不到。
       全双工要两路都能做，任一方向缺就整体拒绝。 */
    if (((msg->dir == (uint8_t)BSP_SPI_DIR_TX) ||
         (msg->dir == (uint8_t)BSP_SPI_DIR_TXRX)) &&
        (bus->rt->tx_mode == BSP_SPI_CAP_NONE)) {
        return BSP_SPI_ERR_MODE;
    }
    if (((msg->dir == (uint8_t)BSP_SPI_DIR_RX) ||
         (msg->dir == (uint8_t)BSP_SPI_DIR_TXRX)) &&
        (bus->rt->rx_mode == BSP_SPI_CAP_NONE)) {
        return BSP_SPI_ERR_MODE;
    }

    return BSP_SPI_OK;
}

/**
 * @brief 按指定方式启动一次搬运
 * @param bus        总线对象，不能为 NULL
 * @param mode       方式，取 BSP_SPI_CAP_BLOCKING / IT / DMA
 * @param msg        待搬运的命令（方向、缓冲、长度）
 * @param timeout_ms 超时预算，只对阻塞方式生效
 * @param done       异步完成回调，可为 NULL
 * @param ctx        回调上下文，原样传回 done
 * @retval BSP_SPI_OK: 异步档已启动（尚未完成）；阻塞档已完成且成功
 * @retval 其他状态码: 启动失败或阻塞档的最终结果
 * @note 只做"登记 + 启动"两件事：异步档先登记再启动，完成中断可能紧跟着
 *       就来，晚登记会丢一次完成；阻塞档自己会结束，顺手撤掉上一轮的登记。
 *       DMA 档要求调用者已确认该方向真的挂了 DMA 流，本函数不再复查。
 */
static bsp_spi_status_t BSP_SPI_StartOnce(const bsp_spi_t *bus, uint8_t mode,
                                          const bsp_spi_msg_t *msg,
                                          uint32_t timeout_ms,
                                          bsp_spi_done_t done, void *ctx)
{
    bsp_spi_rt_t     *rt = bus->rt;
    HAL_StatusTypeDef hal_status;

    if (mode == (uint8_t)BSP_SPI_CAP_BLOCKING) {
        rt->done = NULL;
        rt->ctx  = NULL;
        rt->busy = 0U;
        return BSP_SPI_RunBlocking(bus, msg, timeout_ms);
    }

    rt->done = done;
    rt->ctx  = ctx;
    rt->busy = 1U;

    if (mode == (uint8_t)BSP_SPI_CAP_DMA) {
        hal_status = BSP_SPI_StartDma(bus, msg);
    } else {
        hal_status = BSP_SPI_StartIt(bus, msg);
    }
    if (hal_status != HAL_OK) {
        /* 启动就失败：撤登记、复位外设，避免外设卡在 BUSY */
        (void)BSP_SPI_Abort(bus);
        return BSP_SPI_ERR_HAL;
    }
    return BSP_SPI_OK; /* 尚未完成：等 done，或轮询 IsBusy() */
}

/**
 * @brief 启动一次搬运：按生效档位异步或同步完成
 * @param bus        总线对象，不能为 NULL
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 超时预算（毫秒），只对阻塞方式生效，0 视为 50 ms
 * @param done       异步完成回调，可为 NULL
 * @param ctx        回调上下文，原样传回 done
 * @retval BSP_SPI_OK: 异步方式已启动（尚未完成）；阻塞方式已完成且成功
 * @retval BSP_SPI_ERR_PARAM: 句柄、长度或方向非法
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化，或上一笔异步传输还没收尾
 * @retval BSP_SPI_ERR_MODE: 该方向没有可用方式，或生效档位为无功能
 * @retval BSP_SPI_ERR_TIMEOUT: 阻塞方式等待超时
 * @retval BSP_SPI_ERR_HAL: 各档位启动都失败，或阻塞方式传输出错
 * @retval BSP_SPI_ERR_BUS: 阻塞方式遇到总线错误
 * @note 调用前总线必须已由 Dev_SPI 取得、片选已低、速率已切好。
 *       本次用哪一档由准备阶段定下的生效档位决定：单方向看那一路，全双工
 *       取收发两路中较低的一档——同一次传输只会走一条路径，不会混用。
 *       所选档位启动失败就原地退一档重试（DMA → 中断 → 阻塞），退档只
 *       发生在这一处、只发生在启动失败时；某一档启动成功，本次传输就
 *       固定在该档，中途不再换。
 *       异步方式返回 OK 时传输尚未完成，完成点一定在 done 里；阻塞方式
 *       不调用 done，返回值就是最终结果。
 */
bsp_spi_status_t BSP_SPI_StartTransfer(const bsp_spi_t *bus,
                                       const uint8_t *tx, uint8_t *rx,
                                       uint16_t len, uint32_t timeout_ms,
                                       bsp_spi_done_t done, void *ctx)
{
    bsp_spi_rt_t     *rt;
    bsp_spi_msg_t     msg;
    bsp_spi_status_t  status;
    uint8_t           mode;

    status = BSP_SPI_Prepare(bus, tx, rx, len, &msg);
    if (status != BSP_SPI_OK) {
        return status;
    }
    rt = bus->rt;
    if (timeout_ms == 0U) {
        timeout_ms = BSP_SPI_DEFAULT_TIMEOUT_MS;
    }

    if (msg.dir == (uint8_t)BSP_SPI_DIR_TX) {
        mode = rt->tx_mode;
    } else if (msg.dir == (uint8_t)BSP_SPI_DIR_RX) {
        mode = rt->rx_mode;
    } else {
        mode = BSP_SPI_Lesser(rt->tx_mode, rt->rx_mode);
    }
    if (mode == (uint8_t)BSP_SPI_CAP_NONE) {
        return BSP_SPI_ERR_MODE; /* 理论到不了这里，Prepare 已经挡掉 */
    }

    /* 启动失败向下退一档：DMA → 中断 → 阻塞 */
    while (mode >= (uint8_t)BSP_SPI_CAP_BLOCKING) {
        status = BSP_SPI_StartOnce(bus, mode, &msg, timeout_ms, done, ctx);
        if (status != BSP_SPI_ERR_HAL) {
            return status;
        }
        if (mode == (uint8_t)BSP_SPI_CAP_BLOCKING) {
            break; /* 已经是最低一档，确实搬不动 */
        }
        mode--;
    }
    return BSP_SPI_ERR_HAL;
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
 * @brief 初始化 SPI 驱动：轮询总线表，核对句柄配置并探测两路能力
 * @retval BSP_SPI_OK: 初始化成功（已初始化的总线会被跳过）
 * @retval BSP_SPI_ERR_PARAM: 句柄漏填输入时钟（pclk_hz 为 0）
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

        /* ---------- 2. 探测两路实际能做到的搬运方式 ---------- */
        /* 能力不靠人工登记：方向与 DMA 流都是 CubeMX 已经配好的事实，这里
           只把它们读出来记进 rt。换板子改 .ioc，代码不用跟着抄一遍。 */
        BSP_SPI_DetectModes(bus, hspi);

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
 * 四个回调是同一个"本次传输结束"的四个出口，只是结束条件不同。它们的
 * 入口按该总线生效的搬运方式分两类，阻塞方式一个都不经过：
 * - 中断方式：SPI 中断（TXE / RXNE / ERR 三个事件共用一个 SPIx 中断向量，
 *   由 HAL 的 HAL_SPI_IRQHandler() 按事件分派）；
 * - DMA 方式：该外设 DMA 发送流 / 接收流的传输完成中断与错误中断。
 * 这里覆盖的是 HAL 的 __weak 回调，不是中断入口本身；中断向量与使能
 * 属板级配置（SPIx_IRQHandler / DMA 流的 IRQHandler 由工程提供）。
 * 三个完成回调都只调 BSP_SPI_CompleteFromISR(bus, BSP_SPI_OK)，错误回调
 * 先翻译 ErrorCode 再调它——完成与失败的收尾路径共用一处。
 *============================================================================*/
/**
 * @brief 发送完成回调
 * @param hspi HAL SPI 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；DIR_TX 路径的完成点。
 *       触发时机：只发方向的最后一个字节搬完（中途不会触发）。
 *       中断来源：
 *       - 中断方式：SPI 中断的 TXE 事件 → SPI 的发送 ISR 把最后一字节写进
 *         DR，等 TXE/BSY 收尾后调到这里；
 *       - DMA 方式：DMA 发送流的传输完成中断 → 等 EOT/BSY 收尾后调到这里。
 *       阻塞方式不调本回调，结果由 StartTransfer() 返回值给出。
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
 *       触发时机：只收方向的最后一个字节读完（DR 空、计数归零）。
 *       中断来源：
 *       - 中断方式：SPI 中断的 RXNE 事件 → SPI 的接收 ISR 搬完最后一字节
 *         后调到这里；
 *       - DMA 方式：DMA 接收流的传输完成中断 → SPI 的 DMA 接收收尾后调到这里。
 *       阻塞方式不调本回调，结果由 StartTransfer() 返回值给出。
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
 *       触发时机：收发两边计数都归零才触发一次，发完但没收完时 HAL 不会
 *       先给一次 TxCplt（全双工 ISR 里两边各有一道"对侧计数为 0"的闸）。
 *       中断来源：
 *       - 中断方式：SPI 中断的 TXE 与 RXNE 事件交替进入，收发都归零后由
 *         全双工收尾调到这里；
 *       - DMA 方式：发送流与接收流中最后完成的那个流的完成中断 → 全双工
 *         DMA 收尾（收发两路都判过）后调到这里。
 *       阻塞方式不调本回调，结果由 StartTransfer() 返回值给出。
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
 * @note 触发时机：HAL 检测到通信错误（OVR 溢出、MODF 多主机冲突、CRC 校验
 *       失败、FRE 帧错误）或 DMA 传输错误时进入。同一次传输可能被报不止
 *       一次（中止收尾会再走一遍错误出口），本层只在登记着回调时收下第一
 *       个结果，重复的报错无副作用。
 *       中断来源：
 *       - 中断方式：SPI 中断的 ERR 事件 → HAL 的 IRQ 分派走错误分支；
 *         另外各完成收尾路径发现 ErrorCode 非零时也走本回调；
 *       - DMA 方式：DMA 流的错误中断；若 SPI 错误中断先到，HAL 会先中止
 *         DMA，再从 DMA 中止完成回调进入本函数。
 *       阻塞方式由 HAL 的返回值报错，不调本回调。
 *       MODF（多主机冲突）/ OVR（溢出丢字节）/ CRC 都算总线问题；
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
