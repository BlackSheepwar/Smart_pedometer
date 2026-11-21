/**
 * @file    bsp_uart.c
 * @brief   串口芯片层实现：档位探测、三档搬运、中断只记位置。
 * @author  Ricardo
 * @date    2026-11-21
 *
 * 设计模型：
 *  1. 缓冲归上层：接收缓冲由 BSP_UART_Start() 传入，DMA 直接写它；
 *     发送数据不拷贝，向上层要一段连续区间的指针，搬完只归还长度。
 *  2. 中断只做索引与状态：接收中断累计绝对计数，发送完成中断归还上一段、
 *     再取下一段指针，不在这里搬字节，也不在这里等待。
 *  3. 档位在 Init 探测（DMA 流是否存在），启动失败逐级向下退；
 *     退档只发生在启动那一刻，不影响已经在途的数据。
 *  4. 阻塞档完全不进中断：接收靠 BSP_UART_RxPoll() 轮询，发送在
 *     BSP_UART_TxKick() 里同步搬完，两个接口都不等待超过发送预算。
 */

#include "bsp_uart.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

#include <stddef.h>

/*==============================================================================
 * 配置
 *============================================================================*/

/** @brief 阻塞档一次轮询最多搬走的字节数：太小会跟不上线速率，太大占中断关断时间 */
#define BSP_UART_RX_POLL_MAX   32U

/** @brief 接收缓冲的最小字节数；再小就没有余量容纳一次中断到下一次取数之间的数据 */
#define BSP_UART_RX_SIZE_MIN   16U

/*==============================================================================
 * 类型定义
 *============================================================================*/

/**
 * @brief 串口运行块：每路 UART 一份
 */
typedef struct
{
    uint8_t          *rx_buf;      /**< 上层给的接收缓冲，DMA 直接写入 */
    uint16_t          rx_size;     /**< 接收缓冲字节数，2 的幂 */
    bsp_uart_tx_ops_t tx;          /**< 发送侧取数 / 归还回调 */
    volatile uint32_t rx_total;    /**< 累计收到的绝对字节数 */
    volatile uint16_t rx_last;     /**< 上一次记录的 DMA 写入位置 */
    volatile uint16_t tx_len;      /**< 在途那段的字节数 */
    volatile uint8_t  tx_busy;     /**< 1 = 有一段在途 */
    volatile uint8_t  rx_byte;     /**< 中断档的逐字节接收暂存 */
    volatile uint8_t  ready;       /**< 1 = 接收已武装 */
    volatile uint8_t  initialized; /**< 1 = 档位已探测 */
    uint8_t           tx_mode;     /**< 生效的发送档位 */
    uint8_t           rx_mode;     /**< 生效的接收档位 */
} bsp_uart_rt_t;

/**
 * @brief 串口总线句柄：只登记外设，运行期状态在运行块里
 */
struct bsp_uart
{
    void          *hal;   /**< UART_HandleTypeDef 指针 */
    bsp_uart_rt_t *rt;    /**< 本实例的运行块 */
};

/*==============================================================================
 * 运行块与句柄实例
 *============================================================================*/

/** @brief USART1 的运行块，与句柄实例成对定义 */
static bsp_uart_rt_t bsp_uart1_rt;

/**
 * @brief USART1 的总线句柄
 *
 * @note 本实例只登记"用哪个外设"；接收缓冲、发送回调、档位在
 *       BSP_UART_Init() 与 BSP_UART_Start() 里填进运行块。
 */
const bsp_uart_t bsp_uart1 =
{
    .hal = &huart1,
    .rt  = &bsp_uart1_rt
};

/** @brief 已注册的串口句柄表：回调靠它把 HAL 句柄换回运行块 */
static const bsp_uart_t *const s_handles[] =
{
    &bsp_uart1,
};

/** @brief 句柄表长度：空表时也成立，避免 sizeof 表达式在 -Wtype-limits 下告警 */
static const size_t s_uart_count = sizeof(s_handles) / sizeof(s_handles[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/

/**
 * @brief 把 HAL 返回码翻成本层返回码
 * @param st        HAL 返回值
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        成功
 * @retval BSP_UART_ERR_STATE 外设忙
 * @retval BSP_UART_ERR_TIMEOUT 等待超时
 * @retval BSP_UART_ERR_HAL   其它失败
 */
static bsp_uart_status_t BSP_UART_MapHal(HAL_StatusTypeDef st)
{
    switch (st)
    {
        case HAL_OK:
            return BSP_UART_OK;
        case HAL_BUSY:
            return BSP_UART_ERR_STATE;
        case HAL_TIMEOUT:
            return BSP_UART_ERR_TIMEOUT;
        default:
            return BSP_UART_ERR_HAL;
    }
}

/**
 * @brief 按 HAL 句柄找回运行块
 * @param hal       外设句柄
 * @return bsp_uart_rt_t* 找到的运行块；没有登记过时返回 NULL
 */
static bsp_uart_rt_t *BSP_UART_Find(const UART_HandleTypeDef *hal)
{
    size_t i;

    for (i = 0U; i < s_uart_count; i++)
    {
        if ((s_handles[i]->hal == hal) && (s_handles[i]->rt != NULL))
        {
            return s_handles[i]->rt;
        }
    }

    return NULL;
}

/**
 * @brief 探测发送侧可用的最高档位
 * @param hal       外设句柄
 * @return uint8_t  BSP_UART_CAP_DMA 或 BSP_UART_CAP_IT
 *
 * @note 发送侧只区分"挂没挂 DMA 流"；阻塞档恒可用，不参与探测。
 */
static uint8_t BSP_UART_ActualTx(const UART_HandleTypeDef *hal)
{
    return (hal->hdmatx != NULL) ? BSP_UART_CAP_DMA : BSP_UART_CAP_IT;
}

/**
 * @brief 探测接收侧可用的最高档位
 * @param hal       外设句柄
 * @return uint8_t  BSP_UART_CAP_DMA 或 BSP_UART_CAP_IT
 */
static uint8_t BSP_UART_ActualRx(const UART_HandleTypeDef *hal)
{
    return (hal->hdmarx != NULL) ? BSP_UART_CAP_DMA : BSP_UART_CAP_IT;
}

/**
 * @brief 累计 DMA 已经写进接收缓冲的字节数
 * @param hal       外设句柄
 * @param rt        运行块
 *
 * @note 只做索引运算：读 DMA 剩余计数换算成写指针，与上次记录的位置求差，
 *       差值与 rx_size 取模避免回绕误判。中断与任务上下文都调用，
 *       任务上下文调用前必须关中断。
 */
static void BSP_UART_RxAccumulate(const UART_HandleTypeDef *hal, bsp_uart_rt_t *rt)
{
    uint16_t rem;
    uint16_t pos;
    uint16_t mask;
    uint16_t delta;

    if (hal->hdmarx == NULL)
    {
        return;
    }

    mask = (uint16_t)(rt->rx_size - 1U);
    rem  = (uint16_t)__HAL_DMA_GET_COUNTER(hal->hdmarx);
    if (rem > rt->rx_size)
    {
        rem = rt->rx_size;
    }

    pos   = (uint16_t)((rt->rx_size - rem) & mask);
    delta = (uint16_t)((pos - rt->rx_last) & mask);

    if (delta != 0U)
    {
        rt->rx_total += (uint32_t)delta;
        rt->rx_last   = pos;
    }
}

/**
 * @brief 按当前接收档位武装一次接收
 * @param hal       外设句柄
 * @param rt        运行块
 * @return HAL 状态
 * @retval HAL_OK   已经武装
 * @retval HAL_BUSY 外设还在收上一轮
 * @retval HAL_ERROR 外设拒绝
 *
 * @note 阻塞档不武装，直接返回 HAL_OK。
 */
static HAL_StatusTypeDef BSP_UART_RxArm(const UART_HandleTypeDef *hal, bsp_uart_rt_t *rt)
{
    HAL_StatusTypeDef st;

    switch (rt->rx_mode)
    {
        case BSP_UART_CAP_DMA:
            rt->rx_last = 0U;
            st = HAL_UARTEx_ReceiveToIdle_DMA((UART_HandleTypeDef *)hal,
                                              rt->rx_buf,
                                              rt->rx_size);
            break;

        case BSP_UART_CAP_IT:
            st = HAL_UART_Receive_IT((UART_HandleTypeDef *)hal,
                                     (uint8_t *)&rt->rx_byte,
                                     1U);
            break;

        default:
            st = HAL_OK;
            break;
    }

    return st;
}

/**
 * @brief 组装一次发送的预算时间
 * @param hal       外设句柄
 * @param len       本次要发的字节数
 * @return uint32_t 毫秒
 *
 * @note 8N1 每字节 10 位，向上取整后再留 2 ms 给外设状态切换。
 */
static uint32_t BSP_UART_SendBudgetMs(const UART_HandleTypeDef *hal, uint16_t len)
{
    uint32_t baud = hal->Init.BaudRate;

    if (baud == 0U)
    {
        baud = 1U;
    }

    return (((uint32_t)len * 10U * 1000U) + baud - 1U) / baud + 2U;
}

/**
 * @brief 从上层取一段数据并交给外设
 * @param hal       外设句柄
 * @param rt        运行块
 * @param mode      本次使用的档位
 * @return HAL 状态
 *
 * @note 阻塞档在这里同步搬完；中断与 DMA 档只启动，完成点在回调里。
 */
static HAL_StatusTypeDef BSP_UART_TxStart(UART_HandleTypeDef *hal,
                                          uint8_t mode,
                                          const uint8_t *data,
                                          uint16_t len)
{
    switch (mode)
    {
        case BSP_UART_CAP_DMA:
            return HAL_UART_Transmit_DMA(hal, data, len);

        case BSP_UART_CAP_IT:
            return HAL_UART_Transmit_IT(hal, data, len);

        default:
            return HAL_UART_Transmit(hal, data, len, BSP_UART_SendBudgetMs(hal, len));
    }
}

/**
 * @brief 在中断上下文里接着发下一段
 * @param hal       外设句柄
 * @param rt        运行块
 *
 * @note 只做"取指针 + 启动"，不拷贝、不等待；启动失败就把档位降一级留给
 *       下一次任务上下文重试，绝不在中断里调用阻塞接口。
 */
static void BSP_UART_TxStartNext(UART_HandleTypeDef *hal, bsp_uart_rt_t *rt)
{
    const uint8_t *data = NULL;
    uint16_t len;
    HAL_StatusTypeDef st;

    if ((rt->tx.acquire == NULL) || (rt->rx_buf == NULL))
    {
        return;
    }

    while (rt->tx_busy == 0U)
    {
        len = rt->tx.acquire(rt->tx.ctx, &data);
        if ((len == 0U) || (data == NULL))
        {
            return;
        }

        rt->tx_len  = len;
        rt->tx_busy = 1U;
        st = BSP_UART_TxStart(hal, rt->tx_mode, data, len);

        if (st == HAL_OK)
        {
            return;
        }

        rt->tx_busy = 0U;
        rt->tx_len  = 0U;

        if (rt->tx_mode <= BSP_UART_CAP_IT)
        {
            rt->tx_mode = BSP_UART_CAP_BLOCKING;
            return;
        }

        rt->tx_mode--;
    }
}

/**
 * @brief 发送完成：归还上一段，再启动下一段
 * @param hal       外设句柄
 * @param rt        运行块
 */
static void BSP_UART_TxDone(UART_HandleTypeDef *hal, bsp_uart_rt_t *rt)
{
    uint16_t len = rt->tx_len;

    rt->tx_len  = 0U;
    rt->tx_busy = 0U;

    if ((len != 0U) && (rt->tx.release != NULL))
    {
        rt->tx.release(rt->tx.ctx, len);
    }

    if (rt->tx_mode <= BSP_UART_CAP_BLOCKING)
    {
        return;
    }

    BSP_UART_TxStartNext(hal, rt);
}

/*==============================================================================
 * 对外接口
 *============================================================================*/

/**
 * @brief 初始化串口芯片层：探测两路搬运档位
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        探测完成
 * @retval BSP_UART_ERR_PARAM 某个句柄的外设为空
 * @retval BSP_UART_ERR_STATE 外设不在 READY
 * @retval BSP_UART_ERR_HAL   接收 DMA 流重新配置失败
 */
bsp_uart_status_t BSP_UART_Init(void)
{
    size_t i;
    UART_HandleTypeDef *hal;
    bsp_uart_rt_t *rt;

    for (i = 0U; i < s_uart_count; i++)
    {
        hal = (UART_HandleTypeDef *)s_handles[i]->hal;
        rt  = s_handles[i]->rt;

        if ((hal == NULL) || (rt == NULL))
        {
            return BSP_UART_ERR_PARAM;
        }

        if ((hal->gState != HAL_UART_STATE_READY) ||
            (hal->RxState != HAL_UART_STATE_READY))
        {
            return BSP_UART_ERR_STATE;
        }

        if (hal->hdmarx != NULL)
        {
            hal->hdmarx->Init.Mode = DMA_CIRCULAR;
            if (HAL_DMA_Init(hal->hdmarx) != HAL_OK)
            {
                return BSP_UART_ERR_HAL;
            }
        }

        rt->tx_mode = BSP_UART_ActualTx(hal);
        rt->rx_mode = BSP_UART_ActualRx(hal);
        rt->ready   = 0U;
        rt->initialized = 1U;
    }

    return BSP_UART_OK;
}

/**
 * @brief 启动接收，并登记发送侧回调
 * @param uart      串口总线句柄
 * @param rx_buf    上层的接收缓冲
 * @param rx_size   接收缓冲字节数，2 的幂且不小于 16
 * @param tx_ops    发送侧取数 / 归还回调
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        接收已武装
 * @retval BSP_UART_ERR_PARAM 指针为空、缓冲不合规、回调为空
 * @retval BSP_UART_ERR_STATE 未初始化、已经在跑、外设忙
 * @retval BSP_UART_ERR_HAL   三档都启动失败
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 uint8_t *rx_buf,
                                 uint16_t rx_size,
                                 const bsp_uart_tx_ops_t *tx_ops)
{
    UART_HandleTypeDef *hal;
    bsp_uart_rt_t *rt;
    HAL_StatusTypeDef st;

    if ((uart == NULL) || (uart->rt == NULL) || (rx_buf == NULL) || (tx_ops == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    if ((tx_ops->acquire == NULL) || (tx_ops->release == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    if ((rx_size < BSP_UART_RX_SIZE_MIN) || ((rx_size & (uint16_t)(rx_size - 1U)) != 0U))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt  = uart->rt;
    hal = (UART_HandleTypeDef *)uart->hal;

    if ((rt->initialized == 0U) || (rt->ready != 0U))
    {
        return BSP_UART_ERR_STATE;
    }

    rt->rx_buf    = rx_buf;
    rt->rx_size   = rx_size;
    rt->tx        = *tx_ops;
    rt->rx_total  = 0U;
    rt->rx_last   = 0U;
    rt->tx_len    = 0U;
    rt->tx_busy   = 0U;
    rt->rx_mode   = BSP_UART_ActualRx(hal);
    rt->tx_mode   = BSP_UART_ActualTx(hal);

    while (rt->rx_mode > BSP_UART_CAP_BLOCKING)
    {
        st = BSP_UART_RxArm(hal, rt);
        if (st == HAL_OK)
        {
            rt->ready = 1U;
            return BSP_UART_OK;
        }

        if (st == HAL_BUSY)
        {
            return BSP_UART_ERR_STATE;
        }

        rt->rx_mode--;
    }

    rt->ready = 1U;

    return BSP_UART_OK;
}

/**
 * @brief 查询接收到的绝对字节数
 * @param uart      串口总线句柄
 * @return uint32_t 累计字节数
 */
uint32_t BSP_UART_RxCount(const bsp_uart_t *uart)
{
    uint32_t total;
    uint32_t primask;

    if ((uart == NULL) || (uart->rt == NULL))
    {
        return 0U;
    }

    if (uart->rt->rx_mode != BSP_UART_CAP_DMA)
    {
        return uart->rt->rx_total;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    BSP_UART_RxAccumulate((const UART_HandleTypeDef *)uart->hal, uart->rt);
    total = uart->rt->rx_total;
    if (primask == 0U)
    {
        __enable_irq();
    }

    return total;
}

/**
 * @brief 查询接收是否就绪
 * @param uart      串口总线句柄
 * @return uint8_t
 * @retval 1        可以取数
 * @retval 0        未启动或已中止
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart)
{
    if ((uart == NULL) || (uart->rt == NULL))
    {
        return 0U;
    }

    return uart->rt->ready;
}

/**
 * @brief 把接收缓冲里的字节搬进外设（仅阻塞档使用）
 * @param uart      串口总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        搬完当前可读字节
 * @retval BSP_UART_ERR_PARAM uart 为空
 * @retval BSP_UART_ERR_STATE 未启动
 */
bsp_uart_status_t BSP_UART_RxPoll(const bsp_uart_t *uart)
{
    UART_HandleTypeDef *hal;
    bsp_uart_rt_t *rt;
    uint32_t n = 0U;
    uint32_t mask;
    uint8_t byte;

    if ((uart == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt = uart->rt;

    if (rt->rx_mode != BSP_UART_CAP_BLOCKING)
    {
        return BSP_UART_OK;
    }

    if (rt->ready == 0U)
    {
        return BSP_UART_ERR_STATE;
    }

    hal  = (UART_HandleTypeDef *)uart->hal;
    mask = (uint32_t)(rt->rx_size - 1U);

    while ((n < BSP_UART_RX_POLL_MAX) &&
           (__HAL_UART_GET_FLAG(hal, UART_FLAG_RXNE) != RESET))
    {
        byte = (uint8_t)hal->Instance->DR;
        rt->rx_buf[rt->rx_total & mask] = byte;
        rt->rx_total++;
        n++;
    }

    return BSP_UART_OK;
}

/**
 * @brief 请求发送队列继续外发
 * @param uart      串口总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        已启动或队列为空
 * @retval BSP_UART_ERR_PARAM uart 为空
 * @retval BSP_UART_ERR_STATE 未启动或外设忙
 * @retval BSP_UART_ERR_TIMEOUT 阻塞档超时（数据仍在队列）
 * @retval BSP_UART_ERR_HAL   所有档位都启动失败（数据仍在队列）
 */
bsp_uart_status_t BSP_UART_TxKick(const bsp_uart_t *uart)
{
    UART_HandleTypeDef *hal;
    bsp_uart_rt_t *rt;
    const uint8_t *data = NULL;
    uint16_t len;
    HAL_StatusTypeDef st;
    uint32_t primask;

    if ((uart == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt  = uart->rt;
    hal = (UART_HandleTypeDef *)uart->hal;

    if (rt->ready == 0U)
    {
        return BSP_UART_ERR_STATE;
    }

    if (rt->tx_mode <= BSP_UART_CAP_BLOCKING)
    {
        for (;;)
        {
            len = rt->tx.acquire(rt->tx.ctx, &data);
            if ((len == 0U) || (data == NULL))
            {
                return BSP_UART_OK;
            }

            st = BSP_UART_TxStart(hal, BSP_UART_CAP_BLOCKING, data, len);
            if (st != HAL_OK)
            {
                return BSP_UART_MapHal(st);
            }

            rt->tx.release(rt->tx.ctx, len);
        }
    }

    for (;;)
    {
        primask = __get_PRIMASK();
        __disable_irq();

        if (rt->tx_busy != 0U)
        {
            if (primask == 0U)
            {
                __enable_irq();
            }
            return BSP_UART_OK;
        }

        len = rt->tx.acquire(rt->tx.ctx, &data);
        if ((len == 0U) || (data == NULL))
        {
            if (primask == 0U)
            {
                __enable_irq();
            }
            return BSP_UART_OK;
        }

        rt->tx_len  = len;
        rt->tx_busy = 1U;

        if (primask == 0U)
        {
            __enable_irq();
        }

        st = BSP_UART_TxStart(hal, rt->tx_mode, data, len);
        if (st == HAL_OK)
        {
            return BSP_UART_OK;
        }

        primask = __get_PRIMASK();
        __disable_irq();
        rt->tx_busy = 0U;
        rt->tx_len  = 0U;
        if (primask == 0U)
        {
            __enable_irq();
        }

        if (st == HAL_BUSY)
        {
            return BSP_UART_OK;
        }

        if (rt->tx_mode <= BSP_UART_CAP_IT)
        {
            rt->tx_mode = BSP_UART_CAP_BLOCKING;
            return BSP_UART_OK;
        }

        rt->tx_mode--;
    }
}

/**
 * @brief 查询发送是否已经停稳
 * @param uart      串口总线句柄
 * @return uint8_t
 * @retval 1        队列空且在途数据已收尾
 * @retval 0        还有数据在途
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart)
{
    UART_HandleTypeDef *hal;

    if ((uart == NULL) || (uart->rt == NULL))
    {
        return 1U;
    }

    hal = (UART_HandleTypeDef *)uart->hal;

    if ((uart->rt->tx_busy != 0U) || (uart->rt->tx_len != 0U))
    {
        return 0U;
    }

    return (hal->gState == HAL_UART_STATE_READY) ? 1U : 0U;
}

/**
 * @brief 中止收发并清掉就绪标记
 * @param uart      串口总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        已中止
 * @retval BSP_UART_ERR_PARAM uart 为空
 * @retval BSP_UART_ERR_HAL   外设拒绝中止
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart)
{
    HAL_StatusTypeDef st;

    if ((uart == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    st = HAL_UART_Abort((UART_HandleTypeDef *)uart->hal);

    uart->rt->ready   = 0U;
    uart->rt->tx_busy = 0U;
    uart->rt->tx_len  = 0U;

    return (st == HAL_OK) ? BSP_UART_OK : BSP_UART_ERR_HAL;
}

/*==============================================================================
 * HAL 回调覆盖
 *
 * 三个完成回调与一个错误回调是 HAL 的 __weak 出口，本层把它们覆盖成
 * "只记状态、只启动"：接收方向不碰数据（DMA 已经写进缓冲），发送方向
 * 只归还长度、取下一段指针；任何回调都不做字节拷贝、不做等待。
 * 阻塞档不经过这里。
 *============================================================================*/

/**
 * @brief 接收事件：DMA 半满 / 满 / 空闲
 * @param huart     外设句柄
 * @param Size      本次事件的字节数（未使用，位置从 DMA 计数换算）
 *
 * @note 触发点：DMA 传输完成（半满、满）与线路空闲；只累计绝对计数。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);

    (void)Size;

    if ((rt == NULL) || (rt->rx_mode != BSP_UART_CAP_DMA))
    {
        return;
    }

    BSP_UART_RxAccumulate(huart, rt);
}

/**
 * @brief 中断档接收完成：取走 1 字节后重新武装
 * @param huart     外设句柄
 *
 * @note 触发点：RXNE 中断，每次只来 1 字节；这里是唯一在中断里写接收缓冲的地方。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);

    if ((rt == NULL) || (rt->rx_mode != BSP_UART_CAP_IT))
    {
        return;
    }

    rt->rx_buf[rt->rx_total & (uint32_t)(rt->rx_size - 1U)] = rt->rx_byte;
    rt->rx_total++;
    (void)HAL_UART_Receive_IT(huart, (uint8_t *)&rt->rx_byte, 1U);
}

/**
 * @brief 发送完成：归还上一段并启动下一段
 * @param huart     外设句柄
 *
 * @note 触发点：中断 / DMA 方式下最后一线数据搬完；阻塞方式不经过。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);

    if (rt == NULL)
    {
        return;
    }

    BSP_UART_TxDone(huart, rt);
}

/**
 * @brief 通信错误：必要时重新武装接收
 * @param huart     外设句柄
 *
 * @note 触发点：ORE / FE / NE / PE 等错误中断，以及 DMA 传输错误。
 *       接收还在跑时只清错误不动它；接收已停才重装，避免把正在收的
 *       DMA 重新起一遍而丢掉当前半区。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);

    if ((rt == NULL) || (rt->rx_mode == BSP_UART_CAP_BLOCKING))
    {
        return;
    }

    if (huart->RxState == HAL_UART_STATE_BUSY_RX)
    {
        return;
    }

    if (BSP_UART_RxArm(huart, rt) != HAL_OK)
    {
        rt->ready = 0U;
    }
}
