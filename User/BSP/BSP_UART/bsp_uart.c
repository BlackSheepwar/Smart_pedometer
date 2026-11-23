/**
 * @file bsp_uart.c
 * @brief UART 芯片层：循环 DMA 收字节、窗口累计、分段 DMA 发送
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 设计模型：
 *       1. 缓冲归上层：接收缓冲由设备层持有，本层只登记指针。
 *       2. 接收进度：HT / TC / IDLE 给的 Size 是"自本次武装以来"的窗口
 *          位置，统一用 (pos - last) & (2N - 1) 求增量，三种事件一条路径，
 *          短帧、回绕、TC 之后补报的 IDLE 都不丢。
 *       3. 中断只发布：只更新 rx_total 并回传本实例句柄，不复制字节。
 *       4. 错误恢复：线路错误把接收标记为停止，重装在任务上下文的
 *          BSP_UART_RxRearm() 里做；中断不调用 HAL 收尾接口。
 *       5. DMA 配置只读：只校验 .ioc 把接收 DMA 配成了 circular，不改配置。
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
/** @brief 单个 DMA 节点的最小字节数 */
#define BSP_UART_NODE_MIN   16U

/** @brief 单个 DMA 节点的最大字节数；2N 仍可放进 uint16_t 窗口 */
#define BSP_UART_NODE_MAX   16384U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/** @brief 串口运行块：每路 UART 一份 */
typedef struct {
    bsp_uart_port_t   port;       /* 端口绑定：缓冲与两个通知 */
    volatile uint32_t rx_total;   /* 自武装以来累计接收的绝对字节数 */
    volatile uint16_t rx_last;    /* 上一个 HAL 窗口位置，用于求增量 */
    volatile uint8_t  tx_busy;    /* 1 = 有一段 DMA 发送在途 */
    volatile uint8_t  rx_on;      /* 1 = 接收 DMA 在跑 */
} bsp_uart_rt_t;

/** @brief 串口总线句柄：只登记外设，运行期状态在运行块里 */
struct bsp_uart {
    void          *hal;   /* UART_HandleTypeDef 指针 */
    bsp_uart_rt_t *rt;    /* 本实例的运行块 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief USART1 的运行块，与句柄实例成对定义 */
static bsp_uart_rt_t s_uart1_rt;

/** @brief USART1 的总线句柄 */
const bsp_uart_t bsp_uart1 = {
    .hal = &huart1,
    .rt  = &s_uart1_rt
};

/** @brief 已注册的串口句柄表：回调靠它把 HAL 句柄换回实例 */
static const bsp_uart_t *const s_handles[] = {
    &bsp_uart1,
};

/** @brief 句柄表长度：空表时也成立，避免 sizeof 表达式被判恒假 */
static const size_t s_uart_count = sizeof(s_handles) / sizeof(s_handles[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把 HAL 状态码映射为芯片层状态码
 * @param st HAL 操作状态
 * @retval BSP_UART_OK: HAL_OK
 * @retval BSP_UART_ERR_STATE: HAL_BUSY
 * @retval BSP_UART_ERR_HAL: 其它状态（含超时）
 */
static bsp_uart_status_t BSP_UART_MapHal(HAL_StatusTypeDef st)
{
    if (st == HAL_OK)
    {
        return BSP_UART_OK;
    }

    return (st == HAL_BUSY) ? BSP_UART_ERR_STATE : BSP_UART_ERR_HAL;
}

/**
 * @brief 按 HAL 句柄查找总线句柄
 * @param hal 外设句柄
 * @retval 非空: 已登记的实例
 * @retval NULL: 未登记
 *
 * @note 中断上下文调用；只做指针比较，结果原样回传给上层。
 */
static const bsp_uart_t *BSP_UART_Find(const UART_HandleTypeDef *hal)
{
    size_t i;

    for (i = 0U; i < s_uart_count; i++)
    {
        if (s_handles[i]->hal == (const void *)hal)
        {
            return s_handles[i];
        }
    }

    return NULL;
}

/**
 * @brief 从窗口位置 0 武装循环 DMA 接收
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 已武装
 * @retval BSP_UART_ERR_STATE: 接收 DMA 未配置成 circular
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 只重置物理窗口位置，不清 rx_total。
 */
static bsp_uart_status_t BSP_UART_RxArm(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = uart->rt;
    UART_HandleTypeDef *hal = (UART_HandleTypeDef *)uart->hal;
    HAL_StatusTypeDef st;

    if ((hal->hdmarx == NULL) || (hal->hdmarx->Init.Mode != DMA_CIRCULAR))
    {
        return BSP_UART_ERR_STATE;
    }

    /* 先摘掉"在收"标志再清窗口位置：两者之间进来的事件直接忽略，
       避免拿旧窗口位置对新基准算出一个假增量。 */
    rt->rx_on   = 0U;
    rt->rx_last = 0U;

    st = HAL_UARTEx_ReceiveToIdle_DMA(hal, rt->port.buf,
                                      (uint16_t)((uint32_t)rt->port.node * 2U));
    if (st != HAL_OK)
    {
        return BSP_UART_MapHal(st);
    }

    rt->rx_on = 1U;

    return BSP_UART_OK;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 绑定接收缓冲与通知，启动循环 DMA 接收
 * @param uart 总线句柄
 * @param port 端口绑定
 * @retval BSP_UART_OK: 接收已启动
 * @retval BSP_UART_ERR_PARAM: 缓冲、通知或节点长度非法
 * @retval BSP_UART_ERR_STATE: 接收 DMA 未配置成 circular
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 任务上下文调用一次；之后本层不再等待，进度只由中断累计。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 const bsp_uart_port_t *port)
{
    bsp_uart_rt_t *rt = uart->rt;
    uint32_t node = (uint32_t)port->node;

    if ((port->buf == NULL) || (port->on_rx == NULL) ||
        (node < BSP_UART_NODE_MIN) || (node > BSP_UART_NODE_MAX) ||
        ((node & (node - 1U)) != 0U))
    {
        return BSP_UART_ERR_PARAM;
    }

    rt->port     = *port;
    rt->rx_total = 0U;
    rt->tx_busy  = 0U;

    return BSP_UART_RxArm(uart);
}

/**
 * @brief 读取自武装以来累计接收的绝对字节数
 * @param uart 总线句柄
 * @retval 0 至 UINT32_MAX 累计字节数
 *
 * @note 半满、全满、空闲三种事件都会推进该计数，不得假设它按节点对齐。
 */
uint32_t BSP_UART_RxTotal(const bsp_uart_t *uart)
{
    return uart->rt->rx_total;
}

/**
 * @brief 查询接收是否在武装状态
 * @param uart 总线句柄
 * @retval 1: 接收 DMA 在跑
 * @retval 0: 尚未启动或已被线路错误停止
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart)
{
    return uart->rt->rx_on;
}

/**
 * @brief 接收出错后重新武装循环 DMA
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 已重新武装
 * @retval BSP_UART_ERR_STATE: 中止旧接收失败，或接收 DMA 不是 circular
 * @retval BSP_UART_ERR_HAL: HAL 重新武装失败
 *
 * @note 任务上下文调用。HAL 出错时通常已把接收收尾成 READY，此时
 *       HAL_UART_AbortReceive() 会返回 HAL_BUSY（没有可中止的接收），
 *       所以只在仍处于 BUSY_RX 时才先中止；把 BUSY 当失败会让接收
 *       永远装不回去。rx_total 保持连续，调用者必须以本次成功时的
 *       RxTotal() 为新基准重新对齐缓冲位置。
 */
bsp_uart_status_t BSP_UART_RxRearm(const bsp_uart_t *uart)
{
    UART_HandleTypeDef *hal = (UART_HandleTypeDef *)uart->hal;

    if (hal->RxState == HAL_UART_STATE_BUSY_RX)
    {
        if (HAL_UART_AbortReceive(hal) != HAL_OK)
        {
            return BSP_UART_ERR_STATE;
        }
    }

    return BSP_UART_RxArm(uart);
}

/**
 * @brief 启动一段 DMA 发送
 * @param uart 总线句柄
 * @param data 数据起点，回调前必须保持有效
 * @param len  字节数
 * @retval BSP_UART_OK: DMA 已启动
 * @retval BSP_UART_ERR_STATE: 已有一段在途
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 任务上下文与发送完成中断都会调用，但两者不并发：上层只在"无在途
 *       段"时启动，中断续链时标志已由 TC 清掉。不加临界区，靠这条约定
 *       保证同一时刻只有一段 DMA。
 */
bsp_uart_status_t BSP_UART_TxStart(const bsp_uart_t *uart,
                                   const uint8_t *data, uint16_t len)
{
    bsp_uart_rt_t *rt = uart->rt;
    HAL_StatusTypeDef st;

    if (rt->tx_busy != 0U)
    {
        return BSP_UART_ERR_STATE;
    }
    rt->tx_busy = 1U;

    st = HAL_UART_Transmit_DMA((UART_HandleTypeDef *)uart->hal,
                               (uint8_t *)(uintptr_t)data, len);
    if (st != HAL_OK)
    {
        rt->tx_busy = 0U;
        return BSP_UART_MapHal(st);
    }

    return BSP_UART_OK;
}

/*==============================================================================
 * HAL 回调覆盖
 *============================================================================*/
/**
 * @brief 接收事件回调：换算窗口增量、累计后通知
 * @param huart HAL 外设句柄
 * @param Size  自本次武装以来的绝对窗口位置：0 至 2*node
 * @retval 无
 *
 * @note HT / TC / IDLE 共用本回调，不区分类型：三种都只推进 rx_total，
 *       对上层是同一个"有字节"。TC 之后 HAL 可能补报一次 IDLE
 *       （Size = 2*node，折算为 0），此时增量为 0，处理是幂等的。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    const bsp_uart_t *uart = BSP_UART_Find(huart);
    bsp_uart_rt_t *rt;
    uint32_t ring;
    uint32_t pos;
    uint32_t last;
    uint32_t delta;

    if ((uart == NULL) || (uart->rt->rx_on == 0U))
    {
        return;
    }

    rt = uart->rt;
    ring = (uint32_t)rt->port.node * 2U;
    pos  = ((uint32_t)Size >= ring) ? 0U : (uint32_t)Size;

    /* rx_last / rx_total 只在中断里改：武装期间 rx_on 为 0，本回调直接
       返回，任务侧的复位不会和这里抢；32 位累加读取也不会撕裂。 */
    last  = (uint32_t)rt->rx_last;
    delta = (pos - last) & (ring - 1U);
    if (delta != 0U)
    {
        rt->rx_total += delta;
        rt->rx_last   = (uint16_t)pos;
    }

    if (rt->port.on_rx != NULL)
    {
        rt->port.on_rx(uart);
    }
}

/**
 * @brief 发送完成回调：线路最后一位已移出（USART TC）
 * @param huart HAL 外设句柄
 * @retval 无
 *
 * @note 对应 USART 的 TC，而不是 DMA 的传输完成；切 RS485 方向需要的
 *       "最后一位离开发送器"由这里保证。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    const bsp_uart_t *uart = BSP_UART_Find(huart);
    bsp_uart_rt_t *rt;

    if (uart == NULL)
    {
        return;
    }

    rt = uart->rt;
    rt->tx_busy = 0U;

    if (rt->port.on_tx_done != NULL)
    {
        rt->port.on_tx_done(uart, 1U);
    }
}

/**
 * @brief 错误回调：按错误性质结束在途发送或停止接收
 * @param huart HAL 外设句柄
 * @retval 无
 *
 * @note 线路错误（溢出、帧、噪声、校验）或接收 DMA 出错都会把接收标记为
 *       停止，重装由任务上下文的 BSP_UART_RxRearm() 完成；发送 DMA 出错
 *       则结束在途段并以失败通知上层。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    const bsp_uart_t *uart = BSP_UART_Find(huart);
    bsp_uart_rt_t *rt;
    uint32_t errors;

    if (uart == NULL)
    {
        return;
    }

    rt = uart->rt;
    errors = huart->ErrorCode;

    /* 收发两个 DMA 流都用 HAL_UART_ERROR_DMA 上报，只能靠 DMA 句柄自己的
       错误码区分：发送流出错才结束在途段，接收流出错交给下面的停机分支。 */
    if ((rt->tx_busy != 0U) && (huart->hdmatx != NULL) &&
        (huart->hdmatx->ErrorCode != HAL_DMA_ERROR_NONE))
    {
        rt->tx_busy = 0U;
        if (rt->port.on_tx_done != NULL)
        {
            rt->port.on_tx_done(uart, 0U);
        }
    }

    if ((rt->rx_on != 0U) &&
        (((errors & (HAL_UART_ERROR_ORE | HAL_UART_ERROR_FE |
                     HAL_UART_ERROR_NE | HAL_UART_ERROR_PE)) != 0U) ||
         (huart->RxState != HAL_UART_STATE_BUSY_RX)))
    {
        rt->rx_on = 0U;
        if (rt->port.on_rx != NULL)
        {
            rt->port.on_rx(uart);
        }
    }
}
