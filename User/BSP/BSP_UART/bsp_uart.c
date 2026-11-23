/**
 * @file bsp_uart.c
 * @brief 串口芯片层：循环 DMA 收字节、按窗口位置累计进度
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 设计模型：
 *       1. 缓冲归上层：接收缓冲与收发 FIFO 都由设备层持有，本层只登记
 *          指针，不申请、不缓存、不二次搬运字节。
 *       2. 接收进度：HAL 在 HT / TC / IDLE 三种事件里给出的 Size 是"自本次
 *          武装以来"的绝对窗口位置；本层用 (pos - last) & (2N - 1) 求增量，
 *          三种事件共用同一条路径，因此短帧、回绕、TC 之后的补报 IDLE 都
 *          不丢。
 *       3. 中断只发布：回调只带事件位与累计字节数，不复制数据；搬运与信号量
 *          通知由设备层在任务上下文完成。
 *       4. 错误恢复：接收错误由 HAL 回调标记为"停止接收"，重新武装放在
 *          任务上下文的 BSP_UART_RxRearm() 里完成，中断不调用 HAL 收尾接口。
 *       5. DMA 配置只读：Init 只校验 .ioc 是否把接收 DMA 配成 circular，
 *          不修改 CubeMX 生成的外设配置。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "bsp_uart.h"
// 功能包含
#include "usart.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单个 DMA 节点的最小字节数，再小就没有容纳一次中断的余量 */
#define BSP_UART_DMA_NODE_MIN   16U

/** @brief 单个 DMA 节点的最大字节数；2N 仍可放进 uint16_t 窗口 */
#define BSP_UART_DMA_NODE_MAX   16384U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 串口运行块：每路 UART 一份
 */
typedef struct {
    bsp_uart_port_t   port;        /* 端口绑定：DMA 缓冲、两个通知回调 */
    volatile uint32_t rx_total;    /* 自武装以来累计接收的绝对字节数 */
    volatile uint32_t errors;      /* 累计的 HAL 错误位，读后清零 */
    volatile uint16_t rx_last;     /* 上一个 HAL 窗口位置，用于求增量 */
    volatile uint16_t tx_len;      /* 在途那一段的字节数 */
    volatile uint8_t  tx_busy;     /* 1 = 有一段 DMA 发送在途 */
    volatile uint8_t  ready;       /* 1 = 接收 DMA 已武装 */
    volatile uint8_t  started;     /* 1 = 端口绑定已完成 */
} bsp_uart_rt_t;

/**
 * @brief 串口总线句柄：只登记外设，运行期状态在运行块里
 */
struct bsp_uart {
    void          *hal;   /* UART_HandleTypeDef 指针 */
    bsp_uart_rt_t *rt;    /* 本实例的运行块 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief USART1 的运行块，与句柄实例成对定义 */
static bsp_uart_rt_t bsp_uart1_rt;

/**
 * @brief USART1 的总线句柄
 *
 * @note 本实例只登记"用哪个外设"；DMA 缓冲与回调在 BSP_UART_Start() 里
 *       填进运行块。
 */
const bsp_uart_t bsp_uart1 = {
    .hal = &huart1,
    .rt  = &bsp_uart1_rt
};

/** @brief 已注册的串口句柄表：回调靠它把 HAL 句柄换回运行块 */
static const bsp_uart_t *const s_handles[] = {
    &bsp_uart1,
};

/** @brief 句柄表长度：空表时也成立，避免 sizeof 表达式被判恒假 */
static const size_t s_uart_count = sizeof(s_handles) / sizeof(s_handles[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把 HAL 状态码映射为 BSP 状态码
 * @param st HAL 操作状态
 * @retval BSP_UART_OK: HAL_OK
 * @retval BSP_UART_ERR_STATE: HAL_BUSY
 * @retval BSP_UART_ERR_TIMEOUT: HAL_TIMEOUT
 * @retval BSP_UART_ERR_HAL: 其它 HAL 状态
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
 * @retval 非空: 已登记且含运行块
 * @retval NULL: 句柄无效、未登记或无运行块
 */
static bsp_uart_rt_t *BSP_UART_Find(const UART_HandleTypeDef *hal)
{
    size_t i;

    if (hal == NULL)
    {
        return NULL;
    }

    for (i = 0U; i < s_uart_count; i++)
    {
        if ((s_handles[i] != NULL) && (s_handles[i]->hal == (const void *)hal))
        {
            return s_handles[i]->rt;
        }
    }

    return NULL;
}

/**
 * @brief 校验设备层提供的端口绑定
 * @param port 端口绑定
 * @retval 1: 合法
 * @retval 0: 缓冲、节点长度或接收回调非法
 *
 * @note 节点长度必须是 16 至 16384 的 2 次幂：环形窗口按位掩码求增量，
 *       且 2*dma_node 必须能放进 uint16_t。
 */
static uint8_t BSP_UART_PortValid(const bsp_uart_port_t *port)
{
    uint32_t node;

    if ((port == NULL) || (port->dma_buf == NULL) ||
        (port->on_rx_event == NULL))
    {
        return 0U;
    }

    node = (uint32_t)port->dma_node;
    if ((node < BSP_UART_DMA_NODE_MIN) || (node > BSP_UART_DMA_NODE_MAX))
    {
        return 0U;
    }

    return ((node & (node - 1U)) == 0U) ? 1U : 0U;
}

/**
 * @brief 武装接收：从窗口位置 0 重新开始循环 DMA 接收
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 已武装
 * @retval BSP_UART_ERR_PARAM: 运行块或端口非法
 * @retval BSP_UART_ERR_STATE: HAL 正忙
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 只重置物理窗口位置，不清 rx_total；调用者负责把上层基准对齐到
 *       本函数成功后的 RxTotal。
 */
static bsp_uart_status_t BSP_UART_RxArm(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt;
    UART_HandleTypeDef *hal;
    HAL_StatusTypeDef st;
    uint16_t len;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt = uart->rt;
    hal = (UART_HandleTypeDef *)uart->hal;
    if ((hal->hdmarx == NULL) || (hal->hdmarx->Init.Mode != DMA_CIRCULAR))
    {
        return BSP_UART_ERR_STATE;
    }

    len = (uint16_t)((uint32_t)rt->port.dma_node * 2U);
    rt->rx_last = 0U;
    st = HAL_UARTEx_ReceiveToIdle_DMA(hal, rt->port.dma_buf, len);
    if (st != HAL_OK)
    {
        return BSP_UART_MapHal(st);
    }

    rt->ready = 1U;
    return BSP_UART_OK;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 校验端口实例：HAL 外设就绪、收发 DMA 均已配置
 * @retval BSP_UART_OK: 所有注册实例均可用
 * @retval BSP_UART_ERR_PARAM: 实例、HAL 外设或运行块为空
 * @retval BSP_UART_ERR_STATE: HAL 收发状态非 READY，或接收 DMA 非循环模式
 *
 * @note 只读检查，不修改 CubeMX 生成的 DMA 配置；接收 DMA 的循环模式由
 *       .ioc 负责，本层不擅自重配。
 */
bsp_uart_status_t BSP_UART_Init(void)
{
    size_t i;

    for (i = 0U; i < s_uart_count; i++)
    {
        const bsp_uart_t *uart = s_handles[i];
        UART_HandleTypeDef *hal;

        if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL))
        {
            return BSP_UART_ERR_PARAM;
        }

        hal = (UART_HandleTypeDef *)uart->hal;
        if ((hal->gState != HAL_UART_STATE_READY) ||
            (hal->RxState != HAL_UART_STATE_READY))
        {
            return BSP_UART_ERR_STATE;
        }

        if ((hal->hdmarx == NULL) || (hal->hdmatx == NULL))
        {
            return BSP_UART_ERR_STATE;
        }

        if (hal->hdmarx->Init.Mode != DMA_CIRCULAR)
        {
            return BSP_UART_ERR_STATE;
        }
    }

    return BSP_UART_OK;
}

/**
 * @brief 绑定设备层缓冲与回调，并启动循环 DMA 接收
 * @param uart 总线句柄
 * @param port 端口绑定
 * @retval BSP_UART_OK: 接收已启动
 * @retval BSP_UART_ERR_PARAM: 句柄、HAL、缓冲或回调非法
 * @retval BSP_UART_ERR_STATE: 已经启动或 HAL 正忙
 * @retval BSP_UART_ERR_HAL: HAL 接收启动失败
 *
 * @note 任务上下文调用；启动后本层不再等待，进度只由中断回调报告。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 const bsp_uart_port_t *port)
{
    bsp_uart_rt_t *rt;
    bsp_uart_status_t st;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL) ||
        (BSP_UART_PortValid(port) == 0U))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt = uart->rt;
    if ((rt->started != 0U) || (rt->ready != 0U))
    {
        return BSP_UART_ERR_STATE;
    }

    rt->port      = *port;
    rt->rx_total  = 0U;
    rt->errors    = 0U;
    rt->rx_last   = 0U;
    rt->tx_len    = 0U;
    rt->tx_busy   = 0U;

    st = BSP_UART_RxArm(uart);
    if (st != BSP_UART_OK)
    {
        return st;
    }

    rt->started = 1U;
    return BSP_UART_OK;
}

/**
 * @brief 读取自武装以来累计接收的绝对字节数
 * @param uart 总线句柄
 * @retval 0 至 UINT32_MAX 累计字节数；句柄无效时为 0
 *
 * @note 半满、全满、空闲三种事件都会推进该计数；调用者用它计算需要从
 *       DMA 缓冲搬运的增量，不得假设它只按节点对齐。
 */
uint32_t BSP_UART_RxTotal(const bsp_uart_t *uart)
{
    if ((uart == NULL) || (uart->rt == NULL))
    {
        return 0U;
    }

    return uart->rt->rx_total;
}

/**
 * @brief 查询接收是否处于武装状态
 * @param uart 总线句柄
 * @retval 1: 接收 DMA 已武装
 * @retval 0: 句柄无效、尚未启动或接收已被错误停止
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart)
{
    if ((uart == NULL) || (uart->rt == NULL))
    {
        return 0U;
    }

    return (uart->rt->ready != 0U) ? 1U : 0U;
}

/**
 * @brief 接收错误后重新武装循环 DMA
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 接收已重新武装
 * @retval BSP_UART_ERR_PARAM: 句柄、端口或运行块非法
 * @retval BSP_UART_ERR_STATE: 尚未启动或 HAL 仍忙
 * @retval BSP_UART_ERR_HAL: HAL 重新武装失败
 *
 * @note 任务上下文调用。先中止旧接收（丢弃未发布的窗口残留），再以窗口
 *       位置 0 重新武装；rx_total 保持连续，调用者必须以"本次调用成功时
 *       的 BSP_UART_RxTotal()"为新的物理基准重新对齐 DMA 缓冲位置。
 */
bsp_uart_status_t BSP_UART_RxRearm(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt;
    HAL_StatusTypeDef st;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt = uart->rt;
    if (rt->started == 0U)
    {
        return BSP_UART_ERR_STATE;
    }

    st = HAL_UART_AbortReceive((UART_HandleTypeDef *)uart->hal);
    if ((st != HAL_OK) && (st != HAL_BUSY))
    {
        return BSP_UART_MapHal(st);
    }

    if (st == HAL_BUSY)
    {
        return BSP_UART_ERR_STATE;
    }

    return BSP_UART_RxArm(uart);
}

/**
 * @brief 启动一段 DMA 发送
 * @param uart 总线句柄
 * @param data 数据起点，回调前必须保持有效
 * @param len  字节数，1 至 65535
 * @retval BSP_UART_OK: DMA 已启动
 * @retval BSP_UART_ERR_PARAM: 句柄、数据或长度非法
 * @retval BSP_UART_ERR_STATE: 尚未启动或已有一段在途
 * @retval BSP_UART_ERR_TIMEOUT: HAL 正忙
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 任务上下文调用。先置在途标志再交给 HAL，避免一个字节的短帧在
 *       置标志之前就完成中断；HAL 启动失败时回滚标志。
 */
bsp_uart_status_t BSP_UART_TxStart(const bsp_uart_t *uart,
                                   const uint8_t *data, uint16_t len)
{
    bsp_uart_rt_t *rt;
    HAL_StatusTypeDef st;
    uint32_t primask;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL) ||
        (data == NULL) || (len == 0U))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt = uart->rt;
    if (rt->started == 0U)
    {
        return BSP_UART_ERR_STATE;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if (rt->tx_busy != 0U)
    {
        __set_PRIMASK(primask);
        return BSP_UART_ERR_STATE;
    }
    rt->tx_busy = 1U;
    rt->tx_len  = len;
    __set_PRIMASK(primask);

    st = HAL_UART_Transmit_DMA((UART_HandleTypeDef *)uart->hal,
                               (uint8_t *)(uintptr_t)data, len);
    if (st != HAL_OK)
    {
        primask = __get_PRIMASK();
        __disable_irq();
        rt->tx_busy = 0U;
        rt->tx_len  = 0U;
        __set_PRIMASK(primask);
        return BSP_UART_MapHal(st);
    }

    return BSP_UART_OK;
}

/**
 * @brief 查询发送是否空闲
 * @param uart 总线句柄
 * @retval 1: 句柄无效或当前无在途段
 * @retval 0: 仍有一段 DMA 在途
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart)
{
    if ((uart == NULL) || (uart->rt == NULL))
    {
        return 1U;
    }

    return (uart->rt->tx_busy == 0U) ? 1U : 0U;
}

/**
 * @brief 中止收发并结束在途发送段
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 中止成功
 * @retval BSP_UART_ERR_PARAM: 句柄、HAL 或运行块为空
 * @retval BSP_UART_ERR_HAL: HAL 中止失败
 *
 * @note 任务上下文调用；仅供停机或故障恢复使用。中止后在途发送段不会
 *       再收到完成回调，设备层必须自行丢弃未确认段落并重新武装接收。
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt;
    HAL_StatusTypeDef st;
    uint32_t primask;

    if ((uart == NULL) || (uart->hal == NULL) || (uart->rt == NULL))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt = uart->rt;
    st = HAL_UART_Abort((UART_HandleTypeDef *)uart->hal);
    if (st != HAL_OK)
    {
        return BSP_UART_MapHal(st);
    }

    primask = __get_PRIMASK();
    __disable_irq();
    rt->tx_busy = 0U;
    rt->tx_len  = 0U;
    rt->ready   = 0U;
    __set_PRIMASK(primask);

    return BSP_UART_OK;
}

/**
 * @brief 原子读取并清零累计硬件错误位
 * @param uart 总线句柄
 * @param out  错误位输出地址；为空时不操作
 * @retval 无
 *
 * @note 位定义与 HAL_UART_ERROR_* 一致；读后清零，避免同一错误被重复统计。
 */
void BSP_UART_Errors(const bsp_uart_t *uart, uint32_t *out)
{
    bsp_uart_rt_t *rt;
    uint32_t primask;
    uint32_t errors;

    if ((uart == NULL) || (uart->rt == NULL))
    {
        return;
    }

    rt = uart->rt;
    primask = __get_PRIMASK();
    __disable_irq();
    errors = rt->errors;
    rt->errors = 0U;
    __set_PRIMASK(primask);

    if (out != NULL)
    {
        *out = errors;
    }
}

/*==============================================================================
 * HAL 回调覆盖
 *============================================================================*/
/**
 * @brief 接收事件回调：把 HAL 窗口位置换算为增量，累加后通知设备层
 * @param huart HAL 外设句柄
 * @param Size  自本次武装以来的绝对窗口位置：0 至 2*dma_node
 * @retval 无
 *
 * @note HT / TC / IDLE 共用本回调，事件类型由 HAL_UARTEx_GetRxEventType()
 *       区分。TC 之后 HAL 可能再补报一次 IDLE（Size=2*dma_node），此时
 *       增量为 0，处理是幂等的。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    bsp_uart_rt_t *rt;
    uint32_t ring;
    uint32_t pos;
    uint32_t last;
    uint32_t delta;
    uint32_t primask;
    uint32_t events;

    rt = BSP_UART_Find(huart);
    if ((rt == NULL) || (rt->ready == 0U) || (rt->port.dma_buf == NULL))
    {
        return;
    }

    ring = (uint32_t)rt->port.dma_node * 2U;
    pos  = ((uint32_t)Size >= ring) ? 0U : (uint32_t)Size;

    primask = __get_PRIMASK();
    __disable_irq();
    last  = (uint32_t)rt->rx_last;
    delta = (pos - last) & (ring - 1U);
    if (delta != 0U)
    {
        rt->rx_total += delta;
        rt->rx_last   = (uint16_t)pos;
    }
    __set_PRIMASK(primask);

    switch (HAL_UARTEx_GetRxEventType(huart))
    {
        case HAL_UART_RXEVENT_HT:
            events = BSP_UART_EV_RX_HALF;
            break;
        case HAL_UART_RXEVENT_TC:
            events = BSP_UART_EV_RX_FULL;
            break;
        case HAL_UART_RXEVENT_IDLE:
            events = BSP_UART_EV_RX_IDLE;
            break;
        default:
            events = 0U;
            break;
    }

    if (rt->port.on_rx_event != NULL)
    {
        rt->port.on_rx_event(rt->port.ctx, events, rt->rx_total);
    }
}

/**
 * @brief 发送完成回调：线路最后一位已移出（USART TC）
 * @param huart HAL 外设句柄
 * @retval 无
 *
 * @note 本回调对应 USART 的 TC，而不是 DMA 的传输完成；切 RS485 方向等
 *       需要"最后一位离开发送器"的语义由这里保证。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt;

    rt = BSP_UART_Find(huart);
    if (rt == NULL)
    {
        return;
    }

    rt->tx_busy = 0U;
    rt->tx_len  = 0U;

    if (rt->port.on_tx_done != NULL)
    {
        rt->port.on_tx_done(rt->port.ctx, 1U);
    }
}

/**
 * @brief 错误回调：累计错误位，按错误性质停止接收或结束在途发送
 * @param huart HAL 外设句柄
 * @retval 无
 *
 * @note 接收线路错误（溢出、帧、噪声、校验）会把接收标记为停止，重装由
 *       任务上下文的 BSP_UART_RxRearm() 完成；DMA 错误会结束在途发送段
 *       并以失败通知设备层。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt;
    uint32_t errors;

    rt = BSP_UART_Find(huart);
    if (rt == NULL)
    {
        return;
    }

    errors = huart->ErrorCode;
    rt->errors |= errors;

    if ((errors & HAL_UART_ERROR_DMA) != 0U && rt->tx_busy != 0U)
    {
        rt->tx_busy = 0U;
        rt->tx_len  = 0U;
        if (rt->port.on_tx_done != NULL)
        {
            rt->port.on_tx_done(rt->port.ctx, 0U);
        }
    }

    if (((errors & (HAL_UART_ERROR_ORE | HAL_UART_ERROR_FE |
                    HAL_UART_ERROR_NE | HAL_UART_ERROR_PE)) != 0U) ||
        (huart->RxState != HAL_UART_STATE_BUSY_RX))
    {
        if (rt->ready != 0U)
        {
            rt->ready = 0U;
            if (rt->port.on_rx_event != NULL)
            {
                rt->port.on_rx_event(rt->port.ctx, BSP_UART_EV_RX_ERROR,
                                     rt->rx_total);
            }
        }
    }
}
