/**
 * @file    bsp_uart.c
 * @brief   串口芯片层实现：档位探测、三档搬运、交付新数据、向上层取数。
 * @author  Ricardo
 * @date    2026-11-22
 *
 * 设计模型：
 *  1. 缓冲归上层：接收缓冲（两节点 DMA 缓冲）与发送队列都由设备层持有，
 *     本层只登记指针，不申请、不缓存、不二次搬运字节。
 *  2. 接收进度：DMA 半满/满事件累计已完成节点，空闲事件只唤醒；
 *     Dev_UART_Service() 在任务上下文拷贝已完成节点到接收 FIFO。
 *  3. 发送取数：向上层要一段连续区间的指针直接交给外设，搬完只归还长度。
 *  4. DMA 节点中断必须及时处理，不得屏蔽超过一个节点接收时间，否则硬件标志可能合并。
 *  5. 档位在 Init 探测（DMA 流是否存在），启动失败逐级向下退；退档只发生在
 *     启动那一刻，不影响已经在途的数据。阻塞档完全不进中断。
 */

#include "bsp_uart.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

#include <stddef.h>

/*==============================================================================
 * 配置
 *============================================================================*/

/** @brief 阻塞档一次轮询最多交付的字节数：太小跟不上线速率，太大占任务时间 */
#define BSP_UART_RX_POLL_MAX   32U

/** @brief 单个 DMA 节点的最小字节数；再小就没有余量容纳一次中断到下一次交付 */
#define BSP_UART_DMA_NODE_MIN  16U

/*==============================================================================
 * 类型定义
 *============================================================================*/

/**
 * @brief 串口运行块：每路 UART 一份
 */
typedef struct
{
    bsp_uart_port_t   port;        /**< 端口绑定：DMA 缓冲、交付回调、发送取数与归还 */
    volatile uint32_t rx_total;    /**< 已完成 DMA 节点或已交付字节总量 */
    volatile uint32_t errors;      /**< HAL UART 错误位 */
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
 * @note 本实例只登记"用哪个外设"；DMA 缓冲、交付回调、发送取数与归还
 *       在 BSP_UART_Start() 里填进运行块。
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
 * @brief 把 HAL 状态码映射为 BSP 状态码
 * @param st HAL 操作状态
 * @retval BSP_UART_OK HAL_OK
 * @retval BSP_UART_ERR_STATE HAL_BUSY
 * @retval BSP_UART_ERR_TIMEOUT HAL_TIMEOUT
 * @retval BSP_UART_ERR_HAL 其它 HAL 状态
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
 * @brief 按 HAL 句柄查找运行块
 * @param hal 外设句柄
 * @retval 非空 已登记且含运行块
 * @retval NULL 句柄无效、未登记或无运行块
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
 * @brief 查询发送侧绑定的搬运档位
 * @param hal 外设句柄
 * @retval BSP_UART_CAP_DMA 已绑定 DMA
 * @retval BSP_UART_CAP_IT 未绑定 DMA
 *
 * @note 本函数只报告异步档位偏好；同步阻塞发送作为启动失败后的回退路径。
 */
static uint8_t BSP_UART_ActualTx(const UART_HandleTypeDef *hal)
{
    return (hal->hdmatx != NULL) ? BSP_UART_CAP_DMA : BSP_UART_CAP_IT;
}

/**
 * @brief 查询接收侧绑定的搬运档位
 * @param hal 外设句柄
 * @retval BSP_UART_CAP_DMA 存在接收 DMA 流
 * @retval BSP_UART_CAP_IT 未绑定接收 DMA
 */
static uint8_t BSP_UART_ActualRx(const UART_HandleTypeDef *hal)
{
    return (hal->hdmarx != NULL) ? BSP_UART_CAP_DMA : BSP_UART_CAP_IT;
}

/**
 * @brief 通知设备层发生接收进度事件
 * @param rt 运行块
 * @param event 事件位
 * @retval 无
 */
static void BSP_UART_RxEvent(bsp_uart_rt_t *rt, uint32_t event)
{
    if (rt->port.on_event != NULL)
    {
        rt->port.on_event(rt->port.ctx, event);
    }
}

/**
 * @brief 按当前接收档位武装一次接收
 * @param hal 外设句柄
 * @param rt 运行块
 * @retval HAL_OK 接收已经武装或阻塞档无需武装
 * @retval HAL_BUSY HAL 仍在处理上一轮接收
 * @retval HAL_ERROR HAL 无法启动接收
 *
 * @note 阻塞档不武装，直接返回 HAL_OK。DMA 档重新启动接收时清零累计量，
 *       因为 DMA 计数器从缓冲长度重新开始倒数；IT 档保留累计字节数。
 */
static HAL_StatusTypeDef BSP_UART_RxArm(const UART_HandleTypeDef *hal, bsp_uart_rt_t *rt)
{
    HAL_StatusTypeDef st;

    switch (rt->rx_mode)
    {
        case BSP_UART_CAP_DMA:
            rt->rx_total = 0U;
            st = HAL_UARTEx_ReceiveToIdle_DMA((UART_HandleTypeDef *)hal,
                                              rt->port.dma_buf,
                                              (uint16_t)(rt->port.dma_node * 2U));
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
 * @param hal 外设句柄
 * @param len 本次要发的字节数
 * @retval 计算所得的毫秒预算
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
 * @brief 启动一次数据发送
 * @param hal 外设句柄
 * @param mode 本次使用的档位
 * @param data 数据起点
 * @param len 字节数
 * @retval HAL_OK 已启动或同步发送完成
 * @retval HAL_BUSY HAL 外设忙
 * @retval HAL_TIMEOUT 阻塞发送超时
 * @retval HAL_ERROR HAL 拒绝发送
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
 * @brief 发送完成：归还已经发送的段
 * @param rt 运行块
 * @retval 无
 */
static void BSP_UART_TxDone(bsp_uart_rt_t *rt)
{
    uint16_t len = rt->tx_len;

    rt->tx_len  = 0U;
    rt->tx_busy = 0U;

    if ((len != 0U) && (rt->port.tx_release != NULL))
    {
        rt->port.tx_release(rt->port.ctx, len);
    }
}

/*==============================================================================
 * 对外接口
 *============================================================================*/

/**
 * @brief 初始化串口芯片层并探测收发搬运档位
 * @retval BSP_UART_OK 所有注册实例均已探测
 * @retval BSP_UART_ERR_PARAM 实例句柄、HAL 外设或运行块为空
 * @retval BSP_UART_ERR_STATE 任一 HAL 收发状态非 READY
 * @retval BSP_UART_ERR_HAL 任一接收 DMA 流重新配置失败
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

        rt->tx_mode     = BSP_UART_ActualTx(hal);
        rt->rx_mode     = BSP_UART_ActualRx(hal);
        rt->ready       = 0U;
        rt->initialized = 1U;
    }

    return BSP_UART_OK;
}

/**
 * @brief 启动接收并登记端口绑定
 * @param uart 串口总线句柄
 * @param port 端口绑定
 * @retval BSP_UART_OK 接收已启动
 * @retval BSP_UART_ERR_PARAM 句柄、HAL 外设、缓冲或回调非法
 * @retval BSP_UART_ERR_STATE BSP 未初始化、已经启动或 HAL 正忙
 * @retval BSP_UART_ERR_HAL DMA 与 IT 接收启动均失败
 *
 * @note DMA 中断不得屏蔽超过一个节点接收时长，以免 HT/TC 标志合并导致进度低估。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 const bsp_uart_port_t *port)
{
    UART_HandleTypeDef *hal;
    bsp_uart_rt_t *rt;
    HAL_StatusTypeDef st;
    uint16_t node;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL) ||
        (port == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    node = port->dma_node;

    if ((port->dma_buf == NULL) || (port->on_event == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }


    if ((node < BSP_UART_DMA_NODE_MIN) || ((node & (uint16_t)(node - 1U)) != 0U))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt  = uart->rt;
    hal = (UART_HandleTypeDef *)uart->hal;

    if ((hal == NULL) || (rt->initialized == 0U) || (rt->ready != 0U))
    {
        return (hal == NULL) ? BSP_UART_ERR_PARAM : BSP_UART_ERR_STATE;
    }

    rt->port     = *port;
    rt->rx_total = 0U;
    rt->errors   = 0U;
    rt->tx_len   = 0U;
    rt->tx_busy  = 0U;
    rt->rx_mode  = BSP_UART_ActualRx(hal);
    rt->tx_mode  = BSP_UART_ActualTx(hal);

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
 * @brief 读取已提交的累计接收字节数
 * @param uart 串口总线句柄
 * @retval 0 至 UINT32_MAX 累计字节数；句柄无效时为 0
 */
uint32_t BSP_UART_RxTotal(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt;

    if ((uart == NULL) || (uart->rt == NULL) || (uart->hal == NULL))
    {
        return 0U;
    }

    rt = uart->rt;
    return rt->rx_total;
}

/**
 * @brief 原子读取并清零累计 UART 错误位
 * @param uart 串口总线句柄
 * @param out 错误位输出地址；为空时函数不操作
 * @retval 无
 */
void BSP_UART_Errors(const bsp_uart_t *uart, uint32_t *out)
{
    uint32_t primask;

    if ((uart == NULL) || (uart->rt == NULL) || (out == NULL))
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    *out = uart->rt->errors;
    uart->rt->errors = 0U;
    if (primask == 0U)
    {
        __enable_irq();
    }
}

/**
 * @brief 查询 UART 当前收发搬运档位
 * @param uart 串口总线句柄
 * @param tx_mode 发送档位输出，可为空
 * @param rx_mode 接收档位输出，可为空
 * @retval 无
 *
 * @note 输出值为 BSP_UART_CAP_BLOCKING、BSP_UART_CAP_IT 或 BSP_UART_CAP_DMA；句柄无效时不操作。
 */
void BSP_UART_Modes(const bsp_uart_t *uart, uint8_t *tx_mode, uint8_t *rx_mode)
{
    if ((uart == NULL) || (uart->rt == NULL))
    {
        return;
    }

    if (tx_mode != NULL)
    {
        *tx_mode = uart->rt->tx_mode;
    }
    if (rx_mode != NULL)
    {
        *rx_mode = uart->rt->rx_mode;
    }
}

/**
 * @brief 查询 UART 是否已完成启动
 * @param uart 串口总线句柄
 * @retval 1 UART 已就绪
 * @retval 0 句柄无效或 UART 未就绪
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
 * @brief 推进接收输入（阻塞档轮询，其它档无需轮询）
 * @param uart 串口总线句柄
 * @retval BSP_UART_OK 异步档无需轮询，阻塞档已检查输入
 * @retval BSP_UART_ERR_PARAM 句柄、运行块或 HAL 外设为空
 * @retval BSP_UART_ERR_STATE 未启动或端口回调无效
 */
bsp_uart_status_t BSP_UART_RxService(const bsp_uart_t *uart)
{
    UART_HandleTypeDef *hal;
    bsp_uart_rt_t *rt;
    uint16_t n = 0U;

    if ((uart == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt = uart->rt;

    if (uart->hal == NULL)
    {
        return BSP_UART_ERR_PARAM;
    }

    if (rt->ready == 0U)
    {
        return BSP_UART_ERR_STATE;
    }

    if (rt->port.on_event == NULL)
    {
        return BSP_UART_ERR_STATE;
    }

    if (rt->rx_mode != BSP_UART_CAP_BLOCKING)
    {
        return BSP_UART_OK;
    }

    hal = (UART_HandleTypeDef *)uart->hal;

    while ((n < BSP_UART_RX_POLL_MAX) &&
           (__HAL_UART_GET_FLAG(hal, UART_FLAG_RXNE) != RESET))
    {
        uint32_t index = rt->rx_total % ((uint32_t)rt->port.dma_node * 2U);
        rt->port.dma_buf[index] = (uint8_t)hal->Instance->DR;
        rt->rx_total++;
        n++;
    }

    if (n != 0U)
    {
        rt->port.on_event(rt->port.ctx, BSP_UART_EV_RX_BYTE);
    }

    return BSP_UART_OK;
}

/**
 * @brief 请求发送队列继续外发
 * @param uart      串口总线句柄
 * @retval BSP_UART_OK          已启动或队列为空
 * @retval BSP_UART_ERR_PARAM   uart 为空
 * @retval BSP_UART_ERR_STATE   未启动或外设忙
 * @retval BSP_UART_ERR_TIMEOUT 阻塞档超时（数据仍在队列）
 * @retval BSP_UART_ERR_HAL     所有档位都启动失败（数据仍在队列）
 */
bsp_uart_status_t BSP_UART_TxKick(const bsp_uart_t *uart)
{
    UART_HandleTypeDef *hal;
    bsp_uart_rt_t *rt;
    const uint8_t *data = NULL;
    uint16_t len;
    HAL_StatusTypeDef st;
    uint32_t primask;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt  = uart->rt;
    hal = (UART_HandleTypeDef *)uart->hal;

    if (rt->ready == 0U)
    {
        return BSP_UART_ERR_STATE;
    }

    if ((rt->port.tx_acquire == NULL) || (rt->port.tx_release == NULL))
    {
        return BSP_UART_ERR_STATE;
    }

    if (rt->tx_mode == BSP_UART_CAP_BLOCKING)
    {
        for (;;)
        {
            len = rt->port.tx_acquire(rt->port.ctx, &data);
            if ((len == 0U) || (data == NULL))
            {
                return BSP_UART_OK;
            }

            st = BSP_UART_TxStart(hal, BSP_UART_CAP_BLOCKING, data, len);
            if (st != HAL_OK)
            {
                return BSP_UART_MapHal(st);
            }

            rt->port.tx_release(rt->port.ctx, len);
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

        len = rt->port.tx_acquire(rt->port.ctx, &data);
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

        if (rt->tx_mode > BSP_UART_CAP_IT)
        {
            rt->tx_mode--;
            continue;
        }

        rt->tx_mode = BSP_UART_CAP_BLOCKING;
        st = BSP_UART_TxStart(hal, rt->tx_mode, data, len);
        if (st != HAL_OK)
        {
            return BSP_UART_MapHal(st);
        }
        rt->port.tx_release(rt->port.ctx, len);
        return BSP_UART_OK;
    }
}

/**
 * @brief 查询发送是否已经停稳
 * @param uart      串口总线句柄
 * @retval 1 句柄无效或发送已空闲
 * @retval 0 发送仍在进行
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart)
{
    UART_HandleTypeDef *hal;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL))
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
 * @brief 中止 UART 收发并归还在途发送段
 * @param uart      串口总线句柄
 * @retval BSP_UART_OK        已中止
 * @retval BSP_UART_ERR_PARAM uart 为空
 * @retval BSP_UART_ERR_HAL   外设拒绝中止
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart)
{
    HAL_StatusTypeDef st;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    st = HAL_UART_Abort((UART_HandleTypeDef *)uart->hal);
    if (st != HAL_OK)
    {
        return BSP_UART_ERR_HAL;
    }

    uart->rt->ready = 0U;
    BSP_UART_TxDone(uart->rt);
    return BSP_UART_OK;
}

/*==============================================================================
 * HAL 回调覆盖
 *
 * 接收 DMA 回调累计已完成节点，IDLE 只通知活动；IT 档保存收到的单字节。
 * 发送完成回调只归还已完成段并通知上层，不在中断里启动下一段。
 * 阻塞档不经过这里。
 *============================================================================*/

/**
 * @brief 接收事件：DMA 半满 / 满 / 空闲
 * @param huart 外设句柄
 * @param Size HAL 报告的环形缓冲写入位置
 * @retval 无
 *
 * @note 仅 HT/TC 推进可安全复制的完整节点计数；IDLE 只通知，不发布活动节点。
 *       DMA 缓冲满端 TC 的 Size 可与 TC 同时出现；不将其视为第二次回绕。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);
    HAL_UART_RxEventTypeTypeDef event_type;
    uint32_t primask;
    uint32_t event;

    if ((rt == NULL) || (rt->rx_mode != BSP_UART_CAP_DMA))
    {
        return;
    }

    event_type = HAL_UARTEx_GetRxEventType(huart);
    event = (event_type == HAL_UART_RXEVENT_HT) ? BSP_UART_EV_RX_HALF :
            ((event_type == HAL_UART_RXEVENT_TC) ? BSP_UART_EV_RX_FULL :
             BSP_UART_EV_RX_IDLE);

    primask = __get_PRIMASK();
    __disable_irq();
    if ((event_type == HAL_UART_RXEVENT_HT) ||
        (event_type == HAL_UART_RXEVENT_TC))
    {
        rt->rx_total += rt->port.dma_node;
    }
    (void)Size;
    if (primask == 0U)
    {
        __enable_irq();
    }
    BSP_UART_RxEvent(rt, event);
}

/**
 * @brief 中断档接收完成：交出 1 字节后重新武装
 * @param huart     外设句柄
 * @retval 无
 *
 * @note 触发点：RXNE 中断，每次只来 1 字节；这是中断档唯一的逐字节路径，
 *       交付回调返回后再装下一字节。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);

    if ((rt == NULL) || (rt->rx_mode != BSP_UART_CAP_IT))
    {
        return;
    }

    if (rt->port.dma_buf != NULL)
    {
        uint32_t ring_size = (uint32_t)rt->port.dma_node * 2U;
        rt->port.dma_buf[rt->rx_total % ring_size] = rt->rx_byte;
        rt->rx_total++;
    }
    if (rt->port.on_event != NULL)
    {
        rt->port.on_event(rt->port.ctx, BSP_UART_EV_RX_BYTE);
    }

    (void)HAL_UART_Receive_IT(huart, (uint8_t *)&rt->rx_byte, 1U);
}

/**
 * @brief 发送完成：归还已完成段并通知上层
 * @param huart 外设句柄
 * @retval 无
 *
 * @note 触发点：中断 / DMA 方式下发送搬运完成；阻塞方式不经过。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);

    if (rt == NULL)
    {
        return;
    }

    BSP_UART_TxDone(rt);
    if (rt->port.on_event != NULL)
    {
        rt->port.on_event(rt->port.ctx, BSP_UART_EV_TX_DONE);
    }
}

/**
 * @brief 通信错误：必要时重新武装接收
 * @param huart     外设句柄
 * @retval 无
 *
 * @note 触发点：ORE / FE / NE / PE 等错误中断，以及 DMA 传输错误。
 *       DMA 错误回调由 HAL 在结束对应收发状态后调用；TX DMA 错误时丢弃
 *       当前已提交段（部分字节可能已上线路），RX 还在跑时不重装。接收已停
 *       才重新武装，避免把正在收的 DMA 重新起一遍而丢掉当前半区。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(huart);

    if (rt == NULL)
    {
        return;
    }

    rt->errors |= huart->ErrorCode;
    if (((huart->ErrorCode & HAL_UART_ERROR_DMA) != 0U) &&
        (rt->tx_busy != 0U) &&
        (huart->gState == HAL_UART_STATE_READY))
    {
        BSP_UART_TxDone(rt);
    }
    if (rt->port.on_event != NULL)
    {
        rt->port.on_event(rt->port.ctx, BSP_UART_EV_ERROR);
    }

    if (rt->rx_mode == BSP_UART_CAP_BLOCKING)
    {
        return;
    }

    if (huart->RxState == HAL_UART_STATE_BUSY_RX)
    {
        return;
    }

    if (BSP_UART_RxArm(huart, rt) == HAL_OK)
    {
        rt->ready = 1U;
    }
    else
    {
        rt->ready = 0U;
    }
}
