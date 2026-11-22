/**
 * @file    dev_uart.c
 * @brief   UART 虚拟外设缓冲、收发服务与线程同步。
 * @author  Ricardo
 * @date    2026-11-22
 */

#include "dev_uart.h"
#include "bsp_uart.h"
#include "dev_uart_port.h"

#include <stddef.h>
#include <string.h>

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/

/** @brief DMA 双节点接收缓冲区总字节数 */
#define DEV_UART_RX_DMA_BYTES  256U
/** @brief 接收 FIFO 的容量，必须是 2 的幂 */
#define DEV_UART_RX_FIFO_BYTES 512U
/** @brief 发送 FIFO 的容量，必须是 2 的幂 */
#define DEV_UART_TX_FIFO_BYTES 512U

/*==============================================================================
 * 类型定义
 *============================================================================*/

/** @brief UART 虚拟外设的运行块 */
typedef struct
{
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    volatile uint16_t tx_head;
    volatile uint16_t tx_tail;
    volatile uint32_t rx_overflow;
    volatile uint32_t rx_dma_lost;
    uint32_t rx_done;
    uint8_t ready;
    uint8_t started;
} dev_uart_rt_t;

/** @brief UART 虚拟外设句柄布局 */
struct dev_uart
{
    const bsp_uart_t *bsp;
    dev_uart_port_lock_t **lock_ref;
    uint8_t *dma_buf;
    uint8_t *rx_buf;
    uint8_t *tx_buf;
    uint16_t dma_node;
    uint16_t rx_size;
    uint16_t tx_size;
    dev_uart_rt_t *rt;
};

/*==============================================================================
 * 运行块与句柄实例
 *============================================================================*/

/** @brief UART1 的 DMA 双节点接收缓冲 */
static uint8_t s_uart1_dma[DEV_UART_RX_DMA_BYTES] __attribute__((aligned(4)));
/** @brief UART1 的接收 FIFO */
static uint8_t s_uart1_rx_buf[DEV_UART_RX_FIFO_BYTES] __attribute__((aligned(4)));
/** @brief UART1 的发送 FIFO */
static uint8_t s_uart1_tx_buf[DEV_UART_TX_FIFO_BYTES];
/** @brief UART1 的运行块，与句柄实例成对定义 */
static dev_uart_rt_t dev_uart1_rt;

/** @brief UART1 虚拟外设配置句柄 */
const dev_uart_t dev_uart1 =
{
    .bsp = &bsp_uart1,
    .lock_ref = &UART1_LOCKHandle_port,
    .dma_buf = s_uart1_dma,
    .rx_buf = s_uart1_rx_buf,
    .tx_buf = s_uart1_tx_buf,
    .dma_node = DEV_UART_RX_DMA_BYTES / 2U,
    .rx_size = DEV_UART_RX_FIFO_BYTES,
    .tx_size = DEV_UART_TX_FIFO_BYTES,
    .rt = &dev_uart1_rt
};

/*==============================================================================
 * 内部函数
 *============================================================================*/

/**
 * @brief 计算接收 FIFO 中未消费的数据长度
 * @param dev UART 虚拟外设句柄
 * @return uint16_t
 * @retval 0 至 FIFO 容量 未消费字节数
 */
static uint16_t Dev_UART_RxUsed(const dev_uart_t *dev)
{
    return (uint16_t)(dev->rt->rx_head - dev->rt->rx_tail);
}

/**
 * @brief 计算发送 FIFO 中尚未完成的数据长度
 * @param dev UART 虚拟外设句柄
 * @return uint16_t
 * @retval 0 至 FIFO 容量 尚未完成字节数
 */
static uint16_t Dev_UART_TxUsed(const dev_uart_t *dev)
{
    return (uint16_t)(dev->rt->tx_head - dev->rt->tx_tail);
}

/**
 * @brief 向环形缓冲写入一段连续输入数据
 * @param buf 环形缓冲地址
 * @param size 缓冲区容量，必须是 2 的幂
 * @param head 当前写入序号
 * @param data 输入数据地址
 * @param len 输入数据长度
 * @return uint16_t
 * @retval 新的写入序号
 */
static uint16_t Dev_UART_RingWrite(uint8_t *buf, uint16_t size, uint16_t head,
                                   const uint8_t *data, uint16_t len)
{
    uint16_t offset = (uint16_t)(head & (uint16_t)(size - 1U));
    uint16_t first = (uint16_t)(size - offset);

    if (first > len)
    {
        first = len;
    }
    (void)memcpy(&buf[offset], data, first);
    if (len > first)
    {
        (void)memcpy(buf, &data[first], (size_t)(len - first));
    }
    return (uint16_t)(head + len);
}

/**
 * @brief 将 BSP 中断事件转换为服务任务通知
 * @param ctx 回调上下文
 * @param events BSP 事件位
 * @retval 无
 *
 * @note 此回调运行于中断上下文，只发出 ISR 安全通知，不搬运数据。
 */
static void Dev_UART_OnEvent(void *ctx, uint32_t events)
{
    const dev_uart_t *dev = (const dev_uart_t *)ctx;

    (void)events;
    if ((dev != NULL) && (dev->rt != NULL) &&
        (dev_uart_port_ops.signal_isr != NULL))
    {
        dev_uart_port_ops.signal_isr();
    }
}

/**
 * @brief 获取发送 FIFO 队首连续区间
 * @param ctx 回调上下文
 * @param data 输出数据地址；错误或队列为空时写入 NULL
 * @return uint16_t
 * @retval 连续可发送长度；参数无效或队列为空时为 0
 */
static uint16_t Dev_UART_TxAcquire(void *ctx, const uint8_t **data)
{
    const dev_uart_t *dev = (const dev_uart_t *)ctx;
    uint16_t used;
    uint16_t offset;
    uint16_t len;
    uint32_t state;

    if ((data == NULL) || (dev == NULL) || (dev->rt == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return 0U;
    }
    *data = NULL;
    state = dev_uart_port_ops.critical_enter();
    used = Dev_UART_TxUsed(dev);
    offset = (uint16_t)(dev->rt->tx_tail & (uint16_t)(dev->tx_size - 1U));
    dev_uart_port_ops.critical_exit(state);
    if (used == 0U)
    {
        return 0U;
    }


    len = (uint16_t)(dev->tx_size - offset);
    if (len > used)
    {
        len = used;
    }
    *data = &dev->tx_buf[offset];
    return len;
}

/**
 * @brief 释放已经完成发送的 FIFO 队首区间
 * @param ctx 回调上下文
 * @param len 已发送字节数
 * @retval 无
 */
static void Dev_UART_TxRelease(void *ctx, uint16_t len)
{
    const dev_uart_t *dev = (const dev_uart_t *)ctx;
    uint32_t state;

    if ((dev == NULL) || (dev->rt == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return;
    }
    state = dev_uart_port_ops.critical_enter();
    dev->rt->tx_tail = (uint16_t)(dev->rt->tx_tail + len);
    dev_uart_port_ops.critical_exit(state);
}

/**
 * @brief 将 BSP 返回码映射为设备层状态码
 * @param status BSP 返回码
 * @return dev_uart_status_t
 * @retval DEV_UART_OK BSP 操作成功
 * @retval DEV_UART_ERR_PARAM BSP 参数非法
 * @retval DEV_UART_ERR_STATE BSP 状态不允许
 * @retval DEV_UART_ERR_TIMEOUT BSP 阻塞操作超时
 * @retval DEV_UART_ERR_HAL BSP HAL 操作失败
 */
static dev_uart_status_t Dev_UART_Map(bsp_uart_status_t status)
{
    if (status == BSP_UART_OK)
    {
        return DEV_UART_OK;
    }
    if (status == BSP_UART_ERR_PARAM)
    {
        return DEV_UART_ERR_PARAM;
    }
    if (status == BSP_UART_ERR_STATE)
    {
        return DEV_UART_ERR_STATE;
    }
    if (status == BSP_UART_ERR_TIMEOUT)
    {
        return DEV_UART_ERR_TIMEOUT;
    }
    return DEV_UART_ERR_HAL;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/

/**
 * @brief 初始化 UART1 虚拟外设并启动 BSP 接收
 * @return dev_uart_status_t
 * @retval DEV_UART_OK 初始化成功或已完成
 * @retval DEV_UART_ERR_PARAM 端口绑定或配置非法
 * @retval DEV_UART_ERR_HAL BSP 启动失败
 */
dev_uart_status_t Dev_UART_Init(void)
{
    bsp_uart_port_t port;
    bsp_uart_status_t status;
    const dev_uart_t *dev = &dev_uart1;

    if ((dev->bsp == NULL) || (dev->rt == NULL) || (dev->dma_buf == NULL) ||
        (dev->rx_buf == NULL) || (dev->tx_buf == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }
    if ((dev->rx_size == 0U) || ((dev->rx_size & (dev->rx_size - 1U)) != 0U) ||
        (dev->tx_size == 0U) || ((dev->tx_size & (dev->tx_size - 1U)) != 0U) ||
        (dev->dma_node < 16U) || ((dev->dma_node & (dev->dma_node - 1U)) != 0U))
    {
        return DEV_UART_ERR_PARAM;
    }
    if (dev->rt->started != 0U)
    {
        return (dev->rt->ready != 0U) ? DEV_UART_OK : DEV_UART_ERR_STATE;
    }
    if ((dev->lock_ref == NULL) || (*dev->lock_ref == NULL) ||
        (dev_uart_port_ops.lock == NULL) || (dev_uart_port_ops.unlock == NULL) ||
        (dev_uart_port_ops.signal_isr == NULL) || (dev_uart_port_ops.valid == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL) ||
        (dev_uart_port_ops.valid(*dev->lock_ref) == 0U))
    {
        return DEV_UART_ERR_PARAM;
    }

    (void)memset(dev->rt, 0, sizeof(*dev->rt));
    port.dma_buf = dev->dma_buf;
    port.dma_node = dev->dma_node;
    port.on_event = Dev_UART_OnEvent;
    port.tx_acquire = Dev_UART_TxAcquire;
    port.tx_release = Dev_UART_TxRelease;
    port.ctx = (void *)dev;

    status = BSP_UART_Start(dev->bsp, &port);
    if (status != BSP_UART_OK)
    {
        return Dev_UART_Map(status);
    }
    dev->rt->rx_done = BSP_UART_RxTotal(dev->bsp);
    dev->rt->started = 1U;
    dev->rt->ready = 1U;
    return DEV_UART_OK;
}

/**
 * @brief 搬运已完成接收节点并推进发送队列
 * @param dev UART 虚拟外设句柄
 * @return dev_uart_status_t
 * @retval DEV_UART_OK 服务完成
 * @retval DEV_UART_ERR_PARAM 句柄或接收服务配置非法
 * @retval DEV_UART_ERR_STATE UART 未就绪或接收数据丢失
 * @retval DEV_UART_ERR_TIMEOUT 阻塞接收服务超时
 * @retval DEV_UART_ERR_HAL 接收服务或发送队列启动失败
 *
 * @note 必须由单一任务或主循环调用。DMA 中断每完成一个节点即覆盖前一节点，
 *       因此处理落后超过一个节点时只复制最新完整节点并累计丢失量。
 */
dev_uart_status_t Dev_UART_Service(const dev_uart_t *dev)
{
    uint32_t total;
    uint32_t delta;
    uint32_t ring_size;
    uint32_t start;
    uint16_t head;
    uint16_t tail;
    uint16_t used;
    uint16_t space;
    uint16_t copy_len;
    uint16_t offset;
    uint16_t first;
    dev_uart_status_t result = DEV_UART_OK;

    if ((dev == NULL) || (dev->rt == NULL) || (dev->bsp == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }
    if (dev->rt->ready == 0U)
    {
        return DEV_UART_ERR_STATE;
    }

    {
        bsp_uart_status_t rx_status = BSP_UART_RxService(dev->bsp);
        if (rx_status != BSP_UART_OK)
        {
            return Dev_UART_Map(rx_status);
        }
    }

    total = BSP_UART_RxTotal(dev->bsp);
    delta = total - dev->rt->rx_done;
    ring_size = (uint32_t)dev->dma_node * 2U;
    start = dev->rt->rx_done;
    if (delta > dev->dma_node)
    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        dev->rt->rx_dma_lost += delta - dev->dma_node;
        dev_uart_port_ops.critical_exit(state);
        start = total - dev->dma_node;
        delta = dev->dma_node;
        result = DEV_UART_ERR_STATE;
    }

    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        head = dev->rt->rx_head;
        tail = dev->rt->rx_tail;
        dev_uart_port_ops.critical_exit(state);
    }
    used = (uint16_t)(head - tail);
    space = (used < dev->rx_size) ? (uint16_t)(dev->rx_size - used) : 0U;
    copy_len = (delta > space) ? space : (uint16_t)delta;
    if (copy_len < delta)
    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        dev->rt->rx_overflow += delta - copy_len;
        dev_uart_port_ops.critical_exit(state);
        result = DEV_UART_ERR_STATE;
    }

    if (copy_len != 0U)
    {
        offset = (uint16_t)(start & (ring_size - 1U));
        first = (uint16_t)(ring_size - offset);
        if (first > copy_len)
        {
            first = copy_len;
        }
        head = Dev_UART_RingWrite(dev->rx_buf, dev->rx_size, head,
                                  &dev->dma_buf[offset], first);
        if (copy_len > first)
        {
            head = Dev_UART_RingWrite(dev->rx_buf, dev->rx_size, head,
                                      dev->dma_buf, (uint16_t)(copy_len - first));
        }
        dev->rt->rx_head = head;
    }
    dev->rt->rx_done = total;
    {
        bsp_uart_status_t tx_status = BSP_UART_TxKick(dev->bsp);
        if ((tx_status != BSP_UART_OK) && (result == DEV_UART_OK))
        {
            result = Dev_UART_Map(tx_status);
        }
    }
    return result;
}

/**
 * @brief 等待 UART 服务任务收到传输事件
 * @param dev UART 虚拟外设句柄
 * @param timeout_ms 最大等待时间，单位毫秒
 * @return dev_uart_status_t
 * @retval DEV_UART_OK 收到通知
 * @retval DEV_UART_ERR_PARAM 句柄或等待端口非法
 * @retval DEV_UART_ERR_TIMEOUT 等待超时
 *
 * @note 仅由注册为该 UART 服务任务的线程调用。
 */
dev_uart_status_t Dev_UART_Wait(const dev_uart_t *dev, uint32_t timeout_ms)
{
    if ((dev == NULL) || (dev->lock_ref == NULL) || (*dev->lock_ref == NULL) ||
        (dev->rt == NULL) || (dev_uart_port_ops.wait == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }
    return (dev_uart_port_ops.wait(timeout_ms) != 0U) ? DEV_UART_OK :
           DEV_UART_ERR_TIMEOUT;
}

/**
 * @brief 查询 UART 是否就绪
 * @param dev UART 虚拟外设句柄
 * @return uint8_t
 * @retval 1 UART 已就绪
 * @retval 0 句柄无效、BSP 未就绪或设备未就绪
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev)
{
    return ((dev != NULL) && (dev->rt != NULL) && (dev->rt->ready != 0U) &&
            (BSP_UART_IsReady(dev->bsp) != 0U)) ? 1U : 0U;
}

/**
 * @brief 查询接收 FIFO 中未消费的字节数
 * @param dev UART 虚拟外设句柄
 * @return uint16_t
 * @retval 0 至 FIFO 容量 未消费字节数；句柄无效时为 0
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev)
{
    uint16_t used;

    if ((dev == NULL) || (dev->rt == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return 0U;
    }
    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        used = Dev_UART_RxUsed(dev);
        dev_uart_port_ops.critical_exit(state);
    }

    return used;
}

/**
 * @brief 查看接收 FIFO 队首的连续数据而不消费
 * @param dev UART 虚拟外设句柄
 * @param data 输出队首地址；无连续数据时写入空指针
 * @return uint16_t
 * @retval 0 至 FIFO 容量 连续可读字节数
 *
 * @note 指针仅在对应数据被读取前有效，调用者不得修改缓冲内容。
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, const uint8_t **data)
{
    uint16_t used;
    uint16_t offset;
    uint16_t len;

    if (data == NULL)
    {
        return 0U;
    }
    *data = NULL;
    if ((dev == NULL) || (dev->rt == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return 0U;
    }
    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        used = Dev_UART_RxUsed(dev);
        offset = (uint16_t)(dev->rt->rx_tail & (uint16_t)(dev->rx_size - 1U));
        dev_uart_port_ops.critical_exit(state);
    }
    if (used == 0U)
    {
        return 0U;
    }
    len = (uint16_t)(dev->rx_size - offset);
    if (len > used)
    {
        len = used;
    }
    *data = &dev->rx_buf[offset];
    return len;
}

/**
 * @brief 消费接收 FIFO 队首指定数量的字节
 * @param dev UART 虚拟外设句柄
 * @param len 请求消费数量
 * @return uint16_t
 * @retval 0 至请求长度 实际消费字节数
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint16_t len)
{
    if ((dev == NULL) || (dev->rt == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return 0U;
    }
    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        uint16_t current = Dev_UART_RxUsed(dev);
        if (len > current)
        {
            len = current;
        }
        dev->rt->rx_tail = (uint16_t)(dev->rt->rx_tail + len);
        dev_uart_port_ops.critical_exit(state);
    }
    return len;
}

/**
 * @brief 将完整数据帧串行化写入发送 FIFO
 * @param dev UART 虚拟外设句柄
 * @param data 帧数据地址
 * @param len 帧数据长度
 * @param timeout_ms 等待空间和锁的超时，单位毫秒
 * @return dev_uart_status_t
 * @retval DEV_UART_OK 全帧已入队
 * @retval DEV_UART_ERR_PARAM 参数非法
 * @retval DEV_UART_ERR_STATE UART 未就绪
 * @retval DEV_UART_ERR_TIMEOUT 未能在期限内完整入队
 * @retval DEV_UART_ERR_HAL BSP 启动发送失败；完整帧仍保留在队列中
 *
 * @note 一个调用对应一个不可交错的帧；FIFO 空间不足时不入队任何字节。
 */
dev_uart_status_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *data,
                                 uint16_t len, uint32_t timeout_ms)
{
    uint32_t start;
    uint16_t head;
    uint16_t tail;
    uint16_t used;
    uint16_t space;

    if ((dev == NULL) || (dev->rt == NULL) || (data == NULL) || (len == 0U))
    {
        return DEV_UART_ERR_PARAM;
    }
    if ((dev->rt->ready == 0U) || (dev->lock_ref == NULL) ||
        (*dev->lock_ref == NULL))
    {
        return DEV_UART_ERR_STATE;
    }
    if ((dev_uart_port_ops.lock == NULL) || (dev_uart_port_ops.unlock == NULL) ||
        (dev_uart_port_ops.ticks == NULL) || (dev_uart_port_ops.delay == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return DEV_UART_ERR_STATE;
    }
    if (dev_uart_port_ops.lock(*dev->lock_ref, timeout_ms) == 0U)
    {
        return DEV_UART_ERR_TIMEOUT;
    }

    start = dev_uart_port_ops.ticks();
    for (;;)
    {
        {
            uint32_t state = dev_uart_port_ops.critical_enter();
            head = dev->rt->tx_head;
            tail = dev->rt->tx_tail;
            dev_uart_port_ops.critical_exit(state);
        }
        used = (uint16_t)(head - tail);
        space = (used < dev->tx_size) ? (uint16_t)(dev->tx_size - used) : 0U;
        if (space >= len)
        {
            head = Dev_UART_RingWrite(dev->tx_buf, dev->tx_size, head, data, len);
            {
                uint32_t state = dev_uart_port_ops.critical_enter();
                dev->rt->tx_head = head;
                dev_uart_port_ops.critical_exit(state);
            }
            {
                bsp_uart_status_t tx_status = BSP_UART_TxKick(dev->bsp);
                dev_uart_port_ops.unlock(*dev->lock_ref);
                return Dev_UART_Map(tx_status);
            }
        }
        if ((timeout_ms == 0U) ||
            ((dev_uart_port_ops.ticks() - start) >= timeout_ms))
        {
            break;
        }
        dev_uart_port_ops.unlock(*dev->lock_ref);
        dev_uart_port_ops.delay(1U);
        {
            uint32_t elapsed = dev_uart_port_ops.ticks() - start;
            if (elapsed >= timeout_ms)
            {
                return DEV_UART_ERR_TIMEOUT;
            }
            if (dev_uart_port_ops.lock(*dev->lock_ref, timeout_ms - elapsed) == 0U)
            {
                return DEV_UART_ERR_TIMEOUT;
            }
        }
    }
    dev_uart_port_ops.unlock(*dev->lock_ref);
    return DEV_UART_ERR_TIMEOUT;
}

/**
 * @brief 等待发送 FIFO 与硬件发送器全部空闲
 * @param dev UART 虚拟外设句柄
 * @param timeout_ms 最大等待时间，单位毫秒
 * @return dev_uart_status_t
 * @retval DEV_UART_OK 全部数据发送完成
 * @retval DEV_UART_ERR_PARAM 参数非法
 * @retval DEV_UART_ERR_STATE UART 未就绪或端口操作未绑定
 * @retval DEV_UART_ERR_TIMEOUT 等待发送互斥量、发送完成或期限超时
 * @retval DEV_UART_ERR_HAL BSP 启动发送失败；队列数据保留以便后续重试
 *
 * @note 调用期间持有发送互斥量，避免后续帧并入本次冲刷范围。
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms)
{
    uint32_t start;

    if ((dev == NULL) || (dev->rt == NULL) || (dev->bsp == NULL) ||
        (dev->lock_ref == NULL) || (*dev->lock_ref == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }
    if (dev->rt->ready == 0U)
    {
        return DEV_UART_ERR_STATE;
    }
    if ((dev_uart_port_ops.lock == NULL) || (dev_uart_port_ops.unlock == NULL) ||
        (dev_uart_port_ops.ticks == NULL) || (dev_uart_port_ops.delay == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return DEV_UART_ERR_STATE;
    }
    if (dev_uart_port_ops.lock(*dev->lock_ref, timeout_ms) == 0U)
    {
        return DEV_UART_ERR_TIMEOUT;
    }

    start = dev_uart_port_ops.ticks();
    for (;;)
    {
        bsp_uart_status_t tx_status = BSP_UART_TxKick(dev->bsp);
        if (tx_status != BSP_UART_OK)
        {
            dev_uart_port_ops.unlock(*dev->lock_ref);
            return Dev_UART_Map(tx_status);
        }
        if ((Dev_UART_TxFree(dev) == dev->tx_size) &&
            (BSP_UART_TxIdle(dev->bsp) != 0U))
        {
            dev_uart_port_ops.unlock(*dev->lock_ref);
            return DEV_UART_OK;
        }
        if ((timeout_ms == 0U) || ((dev_uart_port_ops.ticks() - start) >= timeout_ms))
        {
            break;
        }
        dev_uart_port_ops.delay(1U);
    }
    dev_uart_port_ops.unlock(*dev->lock_ref);
    return DEV_UART_ERR_TIMEOUT;
}

/**
 * @brief 查询发送 FIFO 的剩余容量
 * @param dev UART 虚拟外设句柄
 * @return uint16_t
 * @retval 0 至 FIFO 容量 可写字节数；句柄无效时为 0
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev)
{
    uint16_t used;

    if ((dev == NULL) || (dev->rt == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return 0U;
    }
    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        used = Dev_UART_TxUsed(dev);
        dev_uart_port_ops.critical_exit(state);
    }
    return (used < dev->tx_size) ? (uint16_t)(dev->tx_size - used) : 0U;
}

/**
 * @brief 查询接收 DMA 覆盖与 FIFO 丢弃的累计字节数
 * @param dev UART 虚拟外设句柄
 * @return uint32_t
 * @retval 0 至 UINT32_MAX 累计丢失字节数；句柄无效时为 0
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev)
{
    uint32_t state;
    uint32_t total;

    if ((dev == NULL) || (dev->rt == NULL) ||
        (dev_uart_port_ops.critical_enter == NULL) ||
        (dev_uart_port_ops.critical_exit == NULL))
    {
        return 0U;
    }
    state = dev_uart_port_ops.critical_enter();
    total = dev->rt->rx_overflow + dev->rt->rx_dma_lost;
    dev_uart_port_ops.critical_exit(state);
    return total;
}

/**
 * @brief 清零接收 DMA 覆盖与 FIFO 丢弃计数
 * @param dev UART 虚拟外设句柄
 * @retval 无
 */
void Dev_UART_RxClearOverflow(const dev_uart_t *dev)
{
    if ((dev != NULL) && (dev->rt != NULL) &&
        (dev_uart_port_ops.critical_enter != NULL) &&
        (dev_uart_port_ops.critical_exit != NULL))
    {
        uint32_t state = dev_uart_port_ops.critical_enter();
        dev->rt->rx_overflow = 0U;
        dev->rt->rx_dma_lost = 0U;
        dev_uart_port_ops.critical_exit(state);
    }
}
