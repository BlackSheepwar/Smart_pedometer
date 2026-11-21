/**
 * @file    dev_uart.c
 * @brief   串口设备层实现：队列、溢出记账、加锁与超时。
 * @author  Ricardo
 * @date    2026-11-21
 *
 * 数据路径：
 *  接收 DMA 直接写本层的 rx_buf，芯片层只报"总共收了多少字节"，
 *  本层用绝对写指针与读指针的差算可读量，整条路径没有第二次拷贝。
 *
 *  发送队列的物理位置就在本层，芯片层通过 acquire 拿到队首连续区间的
 *  指针直接外发，发完只归还长度，同样没有拷贝。
 *
 * 并发：
 *  发送队列的写指针只在持锁的任务上下文里推进，读指针由芯片层在完成
 *  中断里推进；接收读指针只有单消费者约束。
 */

#include "dev_uart.h"

#include "bsp_uart.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#include <stddef.h>
#include <string.h>

/*==============================================================================
 * 外部声明
 *============================================================================*/

/**
 * @brief USART1 的发送互斥量，由 CubeMX 在 RTOS_MUTEX 段里创建
 */
extern osMutexId_t UART1_LOCKHandle;

/*==============================================================================
 * 配置
 *============================================================================*/

/** @brief 发送等待队列空位时的单次让步时间 */
#define DEV_UART_RETRY_MS       1U

/** @brief 写接口在 timeout_ms 为 0 时不等待，这里只用来兜底除零 */
#define DEV_UART_TIMEOUT_MS_DEF 50U

/*==============================================================================
 * 类型定义
 *============================================================================*/

/**
 * @brief 串口设备运行块：每个端口一份
 */
typedef struct
{
    const bsp_uart_t *bsp;          /**< 芯片层句柄 */
    osMutexId_t      *lock;         /**< 发送队列的互斥量 */
    uint8_t          *rx_buf;       /**< 接收队列，同时是 DMA 目标 */
    uint8_t          *tx_buf;       /**< 发送队列 */
    uint16_t          rx_size;      /**< 接收队列字节数，2 的幂 */
    uint16_t          tx_size;      /**< 发送队列字节数，2 的幂 */
    uint32_t          rx_done;      /**< 已读到的绝对位置 */
    uint32_t          rx_overflow;  /**< 因读得不够快而丢掉的字节数 */
    uint16_t          tx_head;      /**< 写指针（相对队列） */
    uint16_t          tx_tail;      /**< 读指针（相对队列） */
    uint8_t           initialized;  /**< 1 = 已接管芯片层 */
} dev_uart_rt_t;

/**
 * @brief 串口设备句柄：配置与运行块指针
 */
struct dev_uart
{
    const bsp_uart_t *bsp;      /**< 芯片层句柄 */
    osMutexId_t      *lock;     /**< 发送队列互斥量 */
    dev_uart_rt_t    *rt;       /**< 本实例的运行块 */
    uint8_t          *rx_buf;   /**< 接收队列存储 */
    uint8_t          *tx_buf;   /**< 发送队列存储 */
    uint16_t          rx_size;  /**< 接收队列字节数 */
    uint16_t          tx_size;  /**< 发送队列字节数 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/

/** @brief USART1 的接收队列：DMA 直接写入，长度必须是 2 的幂 */
static uint8_t s_uart1_rx_buf[512] __attribute__((aligned(4)));

/** @brief USART1 的发送队列：芯片层直接从这里取数外发 */
static uint8_t s_uart1_tx_buf[512];

/** @brief USART1 的运行块，与句柄实例成对定义 */
static dev_uart_rt_t dev_uart1_rt;

/**
 * @brief USART1 的设备句柄
 *
 * @note 队列存储与容量在 dev_uart.c 里；句柄只登记"用哪个芯片层句柄、
 *       用哪把锁、队列在哪"。
 */
const dev_uart_t dev_uart1 =
{
    .bsp     = &bsp_uart1,
    .lock    = &UART1_LOCKHandle,
    .rt      = &dev_uart1_rt,
    .rx_buf  = s_uart1_rx_buf,
    .tx_buf  = s_uart1_tx_buf,
    .rx_size = sizeof(s_uart1_rx_buf),
    .tx_size = sizeof(s_uart1_tx_buf)
};

/** @brief 已注册的端口表 */
static const dev_uart_t *const s_devs[] =
{
    &dev_uart1,
};

/** @brief 端口表长度 */
static const size_t s_dev_count = sizeof(s_devs) / sizeof(s_devs[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/

/**
 * @brief 计算时间差是否已经超过预算
 * @param start     起始 tick
 * @param ticks     预算 tick
 * @return uint8_t
 * @retval 1        已到期
 * @retval 0        还没到
 */
static uint8_t Dev_UART_Expired(uint32_t start, uint32_t ticks)
{
    return ((osKernelGetTickCount() - start) >= ticks) ? 1U : 0U;
}

/**
 * @brief 把累计接收量与读指针对齐，顺带记溢出
 * @param rt        运行块
 * @return uint16_t 当前可读字节数
 *
 * @note 一旦累计量比读指针超出一整圈，说明最旧的数据已经被 DMA 覆盖，
 *       把读指针推到最旧的有效字节并把差额记进溢出计数。
 */
static uint16_t Dev_UART_RxSync(dev_uart_rt_t *rt)
{
    uint32_t total = BSP_UART_RxCount(rt->bsp);
    uint32_t pending = total - rt->rx_done;

    if (pending > rt->rx_size)
    {
        rt->rx_overflow += pending - rt->rx_size;
        rt->rx_done = total - rt->rx_size;
        pending = rt->rx_size;
    }

    return (uint16_t)pending;
}

/**
 * @brief 发送队列已用字节数
 * @param rt        运行块
 * @return uint16_t 已用字节数
 */
static uint16_t Dev_UART_TxUsed(const dev_uart_rt_t *rt)
{
    return (uint16_t)((rt->tx_head - rt->tx_tail) & (uint16_t)(rt->tx_size - 1U));
}

/**
 * @brief 发送队列剩余空间
 * @param rt        运行块
 * @return uint16_t 可写字节数
 *
 * @note 留一个空位用来区分"空"与"满"。
 */
static uint16_t Dev_UART_TxRoom(const dev_uart_rt_t *rt)
{
    return (uint16_t)((rt->tx_size - 1U) - Dev_UART_TxUsed(rt));
}

/**
 * @brief 把一段数据写进发送队列
 * @param rt        运行块
 * @param src       源数据
 * @param len       字节数；调用方保证不超过剩余空间
 */
static void Dev_UART_TxCopy(dev_uart_rt_t *rt, const uint8_t *src, uint16_t len)
{
    uint16_t mask = (uint16_t)(rt->tx_size - 1U);
    uint16_t pos = (uint16_t)(rt->tx_head & mask);
    uint16_t first = (uint16_t)(rt->tx_size - pos);

    if (first > len)
    {
        first = len;
    }

    (void)memcpy(&rt->tx_buf[pos], src, first);

    if (len > first)
    {
        (void)memcpy(&rt->tx_buf[0], &src[first], (size_t)(len - first));
    }

    rt->tx_head = (uint16_t)((rt->tx_head + len) & mask);
}

/**
 * @brief 芯片层取数回调：交出队首一段连续数据
 * @param ctx       运行块
 * @param data      输出：这段数据的起点
 * @return uint16_t 这段的字节数；0 表示没有待发数据
 *
 * @note 只在索引上做运算，不做拷贝，可以被中断上下文调用。
 */
static uint16_t Dev_UART_OnTxAcquire(void *ctx, const uint8_t **data)
{
    dev_uart_rt_t *rt = (dev_uart_rt_t *)ctx;
    uint16_t used;
    uint16_t mask;
    uint16_t first;

    if ((rt == NULL) || (data == NULL))
    {
        return 0U;
    }

    *data = NULL;
    mask = (uint16_t)(rt->tx_size - 1U);
    used = Dev_UART_TxUsed(rt);

    if (used == 0U)
    {
        return 0U;
    }

    first = (uint16_t)(rt->tx_size - (rt->tx_tail & mask));
    if (first > used)
    {
        first = used;
    }

    *data = &rt->tx_buf[rt->tx_tail & mask];

    return first;
}

/**
 * @brief 芯片层归还回调：推进读指针
 * @param ctx       运行块
 * @param len       已搬完的字节数
 */
static void Dev_UART_OnTxRelease(void *ctx, uint16_t len)
{
    dev_uart_rt_t *rt = (dev_uart_rt_t *)ctx;
    uint16_t mask;

    if ((rt == NULL) || (len == 0U))
    {
        return;
    }

    mask = (uint16_t)(rt->tx_size - 1U);
    rt->tx_tail = (uint16_t)((rt->tx_tail + len) & mask);
}

/*==============================================================================
 * 对外接口
 *============================================================================*/

/**
 * @brief 初始化设备层：登记队列、接管芯片层的收发缓冲
 * @return dev_uart_status_t
 * @retval DEV_UART_OK        就绪
 * @retval DEV_UART_ERR_STATE 芯片层未初始化，或已经接管过
 * @retval DEV_UART_ERR_HAL   芯片层启动失败
 */
dev_uart_status_t Dev_UART_Init(void)
{
    size_t i;
    const dev_uart_t *dev;
    dev_uart_rt_t *rt;
    bsp_uart_tx_ops_t ops;
    bsp_uart_status_t st;

    for (i = 0U; i < s_dev_count; i++)
    {
        dev = s_devs[i];
        rt = (dev == NULL) ? NULL : (dev_uart_rt_t *)dev->rt;

        if ((dev == NULL) || (rt == NULL) || (dev->rx_buf == NULL) || (dev->tx_buf == NULL))
        {
            return DEV_UART_ERR_STATE;
        }

        if (rt->initialized != 0U)
        {
            continue;
        }

        (void)memset(rt, 0, sizeof(*rt));
        rt->bsp     = dev->bsp;
        rt->lock    = dev->lock;
        rt->rx_buf  = dev->rx_buf;
        rt->tx_buf  = dev->tx_buf;
        rt->rx_size = dev->rx_size;
        rt->tx_size = dev->tx_size;

        ops.acquire = Dev_UART_OnTxAcquire;
        ops.release = Dev_UART_OnTxRelease;
        ops.ctx     = rt;

        st = BSP_UART_Start(dev->bsp, dev->rx_buf, dev->rx_size, &ops);
        if (st != BSP_UART_OK)
        {
            return DEV_UART_ERR_HAL;
        }

        rt->initialized = 1U;
    }

    return DEV_UART_OK;
}

/**
 * @brief 查询端口是否可用
 * @param dev       设备句柄
 * @return uint8_t
 * @retval 1        已初始化且芯片层接收已武装
 * @retval 0        未初始化或芯片层已停
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt == NULL) || (dev->rt->initialized == 0U))
    {
        return 0U;
    }

    return BSP_UART_IsReady(dev->rt->bsp);
}

/**
 * @brief 查询接收队列里可读的字节数
 * @param dev       设备句柄
 * @return uint16_t 可读字节数
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt == NULL) || (dev->rt->initialized == 0U))
    {
        return 0U;
    }

    return Dev_UART_RxSync(dev->rt);
}

/**
 * @brief 看一眼队首字节，不取走
 * @param dev       设备句柄
 * @param byte      输出：队首字节
 * @return uint8_t
 * @retval 1        取到了
 * @retval 0        队列为空或参数非法
 */
uint8_t Dev_UART_Peek(const dev_uart_t *dev, uint8_t *byte)
{
    dev_uart_rt_t *rt;
    uint16_t avail;

    if ((dev == NULL) || (dev->rt == NULL) || (byte == NULL) || (dev->rt->initialized == 0U))
    {
        return 0U;
    }

    rt = dev->rt;
    avail = Dev_UART_RxSync(rt);

    if (avail == 0U)
    {
        return 0U;
    }

    *byte = rt->rx_buf[rt->rx_done & (uint32_t)(rt->rx_size - 1U)];

    return 1U;
}

/**
 * @brief 读接收队列
 * @param dev        设备句柄
 * @param buf        目的缓冲
 * @param len        期望字节数
 * @param timeout_ms 最长等待时间
 * @return uint16_t  实际读出的字节数
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    uint32_t start;
    uint32_t ticks;
    uint16_t avail;
    uint16_t n;
    uint16_t mask;
    uint16_t pos;
    uint16_t first;

    if ((dev == NULL) || (dev->rt == NULL) || (buf == NULL) || (len == 0U))
    {
        return 0U;
    }

    rt = dev->rt;

    if (rt->initialized == 0U)
    {
        return 0U;
    }

    start = osKernelGetTickCount();
    ticks = pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        avail = Dev_UART_RxSync(rt);

        if (avail >= len)
        {
            break;
        }

        if (BSP_UART_IsReady(rt->bsp) == 0U)
        {
            break;
        }

        (void)BSP_UART_RxPoll(rt->bsp);

        if ((timeout_ms == 0U) || (Dev_UART_Expired(start, ticks) != 0U))
        {
            break;
        }

        osDelay(DEV_UART_RETRY_MS);
    }

    avail = Dev_UART_RxSync(rt);
    n = (avail < len) ? avail : len;

    if (n == 0U)
    {
        return 0U;
    }

    mask = (uint16_t)(rt->rx_size - 1U);
    pos = (uint16_t)(rt->rx_done & mask);
    first = (uint16_t)(rt->rx_size - pos);

    if (first > n)
    {
        first = n;
    }

    (void)memcpy(buf, &rt->rx_buf[pos], first);

    if (n > first)
    {
        (void)memcpy(&buf[first], &rt->rx_buf[0], (size_t)(n - first));
    }

    rt->rx_done += n;

    return n;
}

/**
 * @brief 写入发送队列并请求外发
 * @param dev        设备句柄
 * @param buf        源数据
 * @param len        字节数
 * @param timeout_ms 队列满时最长等待时间
 * @return uint16_t  实际排入队列的字节数
 */
uint16_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    uint32_t start;
    uint32_t ticks;
    uint16_t done = 0U;
    uint16_t room;
    uint16_t chunk;

    if ((dev == NULL) || (dev->rt == NULL) || (buf == NULL) || (len == 0U))
    {
        return 0U;
    }

    rt = dev->rt;

    if ((rt->initialized == 0U) || (rt->lock == NULL))
    {
        return 0U;
    }

    (void)osMutexAcquire(*rt->lock, osWaitForever);

    start = osKernelGetTickCount();
    ticks = pdMS_TO_TICKS(timeout_ms);

    while (done < len)
    {
        room = Dev_UART_TxRoom(rt);

        if (room == 0U)
        {
            (void)BSP_UART_TxKick(rt->bsp);

            if ((timeout_ms == 0U) || (Dev_UART_Expired(start, ticks) != 0U))
            {
                break;
            }

            osDelay(DEV_UART_RETRY_MS);
            continue;
        }

        chunk = (uint16_t)(len - done);
        if (chunk > room)
        {
            chunk = room;
        }

        Dev_UART_TxCopy(rt, &buf[done], chunk);
        done += chunk;
        (void)BSP_UART_TxKick(rt->bsp);
    }

    (void)osMutexRelease(*rt->lock);

    return done;
}

/**
 * @brief 等待发送彻底停下
 * @param dev        设备句柄
 * @param timeout_ms 最长等待时间
 * @return dev_uart_status_t
 * @retval DEV_UART_OK          已发完
 * @retval DEV_UART_ERR_PARAM   dev 为空
 * @retval DEV_UART_ERR_STATE   未初始化
 * @retval DEV_UART_ERR_TIMEOUT 超时仍有数据
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    uint32_t start;
    uint32_t ticks;
    dev_uart_status_t ret = DEV_UART_ERR_TIMEOUT;

    if ((dev == NULL) || (dev->rt == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }

    rt = dev->rt;

    if ((rt->initialized == 0U) || (rt->lock == NULL))
    {
        return DEV_UART_ERR_STATE;
    }

    (void)osMutexAcquire(*rt->lock, osWaitForever);

    start = osKernelGetTickCount();
    ticks = pdMS_TO_TICKS((timeout_ms == 0U) ? DEV_UART_TIMEOUT_MS_DEF : timeout_ms);

    for (;;)
    {
        (void)BSP_UART_TxKick(rt->bsp);

        if ((Dev_UART_TxUsed(rt) == 0U) && (BSP_UART_TxIdle(rt->bsp) != 0U))
        {
            ret = DEV_UART_OK;
            break;
        }

        if (Dev_UART_Expired(start, ticks) != 0U)
        {
            break;
        }

        osDelay(DEV_UART_RETRY_MS);
    }

    (void)osMutexRelease(*rt->lock);

    return ret;
}

/**
 * @brief 查询发送队列剩余空间
 * @param dev       设备句柄
 * @return uint16_t 可写字节数
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt == NULL) || (dev->rt->initialized == 0U))
    {
        return 0U;
    }

    return Dev_UART_TxRoom(dev->rt);
}

/**
 * @brief 读取接收溢出计数
 * @param dev       设备句柄
 * @return uint32_t 累计丢掉的字节数
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt == NULL) || (dev->rt->initialized == 0U))
    {
        return 0U;
    }

    (void)Dev_UART_RxSync(dev->rt);

    return dev->rt->rx_overflow;
}

/**
 * @brief 把接收溢出计数清零
 * @param dev       设备句柄
 * @return dev_uart_status_t
 * @retval DEV_UART_OK        已清零
 * @retval DEV_UART_ERR_PARAM dev 为空
 */
dev_uart_status_t Dev_UART_RxClearOverflow(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }

    dev->rt->rx_overflow = 0U;

    return DEV_UART_OK;
}
