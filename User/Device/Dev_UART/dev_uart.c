/**
 * @file dev_uart.c
 * @brief UART 虚拟外设：DMA 窗口搬运、双向 FIFO、中断续链发送
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 设计模型：
 *       1. 接收：芯片层回调带的是本实例句柄，本层原样把它投进事件队列；
 *          服务任务按"绝对接收量 - 已搬走量"求增量，把字节从 DMA 缓冲搬
 *          进接收 FIFO。
 *       2. 发送：写者持互斥量把整帧写进发送 FIFO，若当前没有在途段就
 *          顺手启动第一段；之后每段发完由 TC 中断归还并续链下一段，
 *          整条链不需要任务参与。
 *       3. 发送不加临界区：写者之间由互斥量串行化，写者与中断不并发
 *          （中断只打断任务）；中断先归还已发段、再判断有无后续数据，
 *          写者只在"无在途段"时才启动，两者不会撞车。
 *       4. 零拷贝：FIFO 队首连续段直接交给 DMA，TC 后才 consume。
 *       5. 新增一路 UART 只需登记实例与缓冲；回调和任务都不用改，也不需要
 *          在任何一张表里再补一行。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "dev_uart.h"
// 功能包含
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "bsp_uart.h"
#include "ringbuffer.h"

/*==============================================================================
 * 外部变量声明
 *============================================================================*/
/** @brief CubeMX 生成的 UART 事件队列，元素是待服务的实例句柄 */
extern osMessageQueueId_t UART_EVT_QHandle;
/** @brief CubeMX 生成的 UART1 发送互斥量 */
extern osMutexId_t UART1_LOCKHandle;

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief UART 虚拟外设运行块
 * @note 回调带的是 const 句柄，句柄里 bsp、缓冲、rt 都在，中断里一次取用即可，
 *       运行块不保存回指指针。
 */
typedef struct {
    ringbuffer_t rx_fifo;       /* 接收 FIFO：服务任务生产、应用消费 */
    ringbuffer_t tx_fifo;       /* 发送 FIFO：写者生产、TC 中断消费 */
    uint32_t     rx_base;       /* 最近一次武装成功时的绝对接收基准 */
    uint32_t     rx_cons;       /* 已从 DMA 缓冲搬走的绝对字节数 */
    uint32_t     rx_lost;       /* DMA 覆盖与接收 FIFO 丢弃累计 */
    uint32_t     tx_errors;     /* 发送失败段累计 */
    volatile uint16_t tx_chunk; /* 在途段字节数，TC 后归还 */
    volatile uint8_t  tx_busy;  /* 1 = 有一段 DMA 发送在途 */
} dev_uart_rt_t;

/** @brief UART 虚拟外设句柄布局：容量属于实例，每路可以各不相同 */
struct dev_uart {
    const bsp_uart_t *bsp;      /* 芯片层总线句柄 */
    uint8_t          *dma;      /* 双节点接收 DMA 缓冲，长度 2*node */
    uint8_t          *rx_pool;  /* 接收 FIFO 存储池 */
    uint8_t          *tx_pool;  /* 发送 FIFO 存储池 */
    uint16_t          node;     /* 单个 DMA 节点字节数：16 至 16384 的 2 次幂 */
    uint16_t          rx_size;  /* 接收 FIFO 容量 */
    uint16_t          tx_size;  /* 发送 FIFO 容量，也是单帧上限 */
    dev_uart_rt_t    *rt;       /* 本实例运行块 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/**
 * @brief 实例容量：只用来定静态缓冲的大小
 * @note 运行期一律读句柄里的 node / rx_size / tx_size，代码不假设任何一路的
 *       容量；新增一路串口就复制这一组宏与下面的缓冲定义。
 */
#define DEV_UART1_NODE      128U
#define DEV_UART1_RX_BYTES  512U
#define DEV_UART1_TX_BYTES  512U

/** @brief UART1 的接收 DMA 缓冲 */
static uint8_t s_uart1_dma[DEV_UART1_NODE * 2U] __attribute__((aligned(4)));
/** @brief UART1 的接收 FIFO 存储池 */
static uint8_t s_uart1_rx_pool[DEV_UART1_RX_BYTES] __attribute__((aligned(4)));
/** @brief UART1 的发送 FIFO 存储池 */
static uint8_t s_uart1_tx_pool[DEV_UART1_TX_BYTES];
/** @brief UART1 的运行块 */
static dev_uart_rt_t s_uart1_rt;

/** @brief UART1 虚拟外设句柄 */
const dev_uart_t dev_uart1 = {
    .bsp     = &bsp_uart1,
    .dma     = s_uart1_dma,
    .rx_pool = s_uart1_rx_pool,
    .tx_pool = s_uart1_tx_pool,
    .node    = DEV_UART1_NODE,
    .rx_size = DEV_UART1_RX_BYTES,
    .tx_size = DEV_UART1_TX_BYTES,
    .rt      = &s_uart1_rt
};

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把芯片层状态码映射为设备层状态码
 * @param st 芯片层状态
 * @retval DEV_UART_OK: 成功
 * @retval DEV_UART_ERR_PARAM: 参数非法
 * @retval DEV_UART_ERR_STATE: 状态不允许
 * @retval DEV_UART_ERR_HAL: 芯片层失败
 */
static dev_uart_status_t Dev_UART_MapBsp(bsp_uart_status_t st)
{
    switch (st)
    {
        case BSP_UART_OK:
            return DEV_UART_OK;
        case BSP_UART_ERR_PARAM:
            return DEV_UART_ERR_PARAM;
        case BSP_UART_ERR_STATE:
            return DEV_UART_ERR_STATE;
        default:
            return DEV_UART_ERR_HAL;
    }
}

/**
 * @brief 接收回调：把本实例句柄投进事件队列
 * @param ctx 端口绑定里登记的上下文，就是本实例句柄
 * @retval 无
 *
 * @note 中断上下文调用；搬运与重装留给服务任务。句柄里 bsp、缓冲、rt 都在，
 *       取一次就够，不需要按任何下层句柄反查。
 */
static void Dev_UART_OnRx(void *ctx)
{
    const dev_uart_t *dev = (const dev_uart_t *)ctx;

    (void)osMessageQueuePut(UART_EVT_QHandle, &dev, 0U, 0U);
}

/**
 * @brief 发送完成回调：归还已发段，有后续数据就原地续链
 * @param ctx 端口绑定里登记的上下文，就是本实例句柄
 * @param ok  1 = 线路 TC 完成；0 = 错误中止
 * @retval 无
 *
 * @note 中断上下文调用。先 consume 再判断下一段：这样写者只要看到
 *       tx_busy 为 0，队首一定是未被发出的数据，不会重复武装。续链
 *       期间 tx_busy 保持 1，写者不会插进来；启动失败才清 0，留给
 *       下一次写入重新拉起。
 */
static void Dev_UART_OnTxDone(void *ctx, uint8_t ok)
{
    const dev_uart_t *dev = (const dev_uart_t *)ctx;
    dev_uart_rt_t *rt = dev->rt;
    uint8_t *ptr = NULL;
    uint16_t len;

    if (ok == 0U)
    {
        rt->tx_errors++;
    }

    (void)ringbuffer_consume(&rt->tx_fifo, (size_t)rt->tx_chunk);

    len = (uint16_t)ringbuffer_peek(&rt->tx_fifo, &ptr);
    if (len == 0U)
    {
        rt->tx_busy = 0U;
        return;
    }

    rt->tx_chunk = len;
    if (BSP_UART_TxStart(dev->bsp, ptr, len) != BSP_UART_OK)
    {
        rt->tx_busy = 0U;
    }
}

/**
 * @brief 接收搬运：把 DMA 缓冲里的新增字节搬进接收 FIFO
 * @param dev UART 句柄
 * @retval 无
 *
 * @note 只允许服务任务调用。增量超过一个环形窗口时，窗口外的旧字节已被
 *       DMA 覆盖，只保留最新数据并计入丢失；接收 FIFO 满时丢最新字节。
 */
static void Dev_UART_RxPump(const dev_uart_t *dev)
{
    dev_uart_rt_t *rt = dev->rt;
    uint32_t ring = (uint32_t)dev->node * 2U;
    uint32_t pub  = BSP_UART_RxTotal(dev->bsp);
    uint32_t len  = pub - rt->rx_cons;
    uint32_t cons = rt->rx_cons;
    uint32_t space;
    uint32_t chunk;
    uint32_t off;
    uint16_t first;
    size_t copied;

    if (len == 0U)
    {
        return;
    }

    /* 超过一个窗口：窗口外的旧字节已被 DMA 覆盖，只保留最新数据 */
    if (len > ring)
    {
        rt->rx_lost += len - ring;
        cons = pub - ring;
        len  = ring;
    }

    space = (uint32_t)ringbuffer_space_len(&rt->rx_fifo);
    chunk = (len < space) ? len : space;

    /* FIFO 装不下的部分直接丢弃；搬走的和丢掉的都算处理过，一次推进到位 */
    rt->rx_lost += len - chunk;
    rt->rx_cons  = pub;

    if (chunk == 0U)
    {
        return;
    }

    off   = (cons - rt->rx_base) & (ring - 1U);
    first = (uint16_t)(ring - off);
    if ((uint32_t)first > chunk)
    {
        first = (uint16_t)chunk;
    }

    copied = ringbuffer_put(&rt->rx_fifo, &dev->dma[off], first);
    if (chunk > (uint32_t)first)
    {
        copied += ringbuffer_put(&rt->rx_fifo, dev->dma,
                                 (uint16_t)(chunk - (uint32_t)first));
    }

    if ((uint32_t)copied < chunk)
    {
        rt->rx_lost += chunk - (uint32_t)copied;
    }
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化一路 UART 虚拟外设
 * @param dev 虚拟外设句柄
 * @retval DEV_UART_OK: 成功
 * @retval DEV_UART_ERR_STATE: 接收 DMA 未配置成 circular
 * @retval DEV_UART_ERR_HAL: 芯片层启动接收失败
 *
 * @note 任务上下文调用，每路一次。事件队列由 CubeMX 建立（UART_EVT_Q），
 *       调用前必须已存在，否则中断无处投递。
 */
dev_uart_status_t Dev_UART_Init(const dev_uart_t *dev)
{
    dev_uart_rt_t *rt = dev->rt;
    bsp_uart_port_t port;
    bsp_uart_status_t bst;
    uint32_t total;

    ringbuffer_init(&rt->rx_fifo, dev->rx_pool, dev->rx_size);
    ringbuffer_init(&rt->tx_fifo, dev->tx_pool, dev->tx_size);

    port.buf        = dev->dma;
    port.node       = dev->node;
    port.ctx        = (void *)dev;
    port.on_rx      = Dev_UART_OnRx;
    port.on_tx_done = Dev_UART_OnTxDone;

    bst = BSP_UART_Start(dev->bsp, &port);
    if (bst != BSP_UART_OK)
    {
        return Dev_UART_MapBsp(bst);
    }

    total = BSP_UART_RxTotal(dev->bsp);
    rt->rx_base = total;
    rt->rx_cons = total;

    return DEV_UART_OK;
}

/**
 * @brief 阻塞等待一路待服务的串口
 * @param dev 输出：待服务的实例
 * @retval 无
 *
 * @note 没有超时：串口闲着就睡着，来事件才醒。队列给谁就服务谁。
 */
void Dev_UART_Wait(const dev_uart_t **dev)
{
    (void)osMessageQueueGet(UART_EVT_QHandle, dev, NULL, osWaitForever);
}

/**
 * @brief 服务一路串口：搬运接收、按需重装
 * @param dev UART 句柄
 * @retval DEV_UART_OK: 本轮完成
 * @retval DEV_UART_ERR_STATE: 芯片层重装接收被拒
 * @retval DEV_UART_ERR_HAL: 芯片层重装接收失败
 *
 * @note 发送不经过这里：整条发送链由写者和 TC 中断推进。
 */
dev_uart_status_t Dev_UART_Service(const dev_uart_t *dev)
{
    bsp_uart_status_t bst;
    uint32_t total;

    Dev_UART_RxPump(dev);

    if (BSP_UART_IsReady(dev->bsp) == 0U)
    {
        total = BSP_UART_RxTotal(dev->bsp);
        bst = BSP_UART_RxRearm(dev->bsp);
        if (bst != BSP_UART_OK)
        {
            return Dev_UART_MapBsp(bst);
        }
        dev->rt->rx_base = total;
        dev->rt->rx_cons = total;
    }

    return DEV_UART_OK;
}

/**
 * @brief 查询接收 FIFO 可读字节数
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 未消费字节数
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev)
{
    return (uint16_t)ringbuffer_data_len(&dev->rt->rx_fifo);
}

/**
 * @brief 查看接收 FIFO 队首连续数据，不消费
 * @param dev  UART 句柄
 * @param data 输出队首地址
 * @retval 0 至 FIFO 容量 连续可读字节数
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, const uint8_t **data)
{
    uint8_t *ptr = NULL;
    size_t len = ringbuffer_peek(&dev->rt->rx_fifo, &ptr);

    *data = ptr;

    return (uint16_t)len;
}

/**
 * @brief 消费接收 FIFO 队首字节
 * @param dev UART 句柄
 * @param len 请求消费数量
 * @retval 0 至请求长度 实际消费数量
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint16_t len)
{
    return (uint16_t)ringbuffer_consume(&dev->rt->rx_fifo, (size_t)len);
}

/**
 * @brief 将完整数据帧写入发送 FIFO，必要时启动第一段
 * @param dev        UART 句柄
 * @param data       数据起点
 * @param len        数据长度，1 至 FIFO 容量
 * @param timeout_ms 等互斥量的上限，单位毫秒
 * @retval DEV_UART_OK: 已入队；没有在途段时发送已经启动
 * @retval DEV_UART_ERR_PARAM: 长度非法
 * @retval DEV_UART_ERR_STATE: 空间不足（未写入）或互斥量等待超时
 * @retval DEV_UART_ERR_HAL: 芯片层启动首段失败；整帧仍在队列里
 *
 * @note 持锁期间完成"拷入 FIFO + 决定是否启动"，锁外不碰发送状态。
 *       空间不足立即返回，设备层不睡眠。
 */
dev_uart_status_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *data,
                                 uint16_t len, uint32_t timeout_ms)
{
    dev_uart_rt_t *rt = dev->rt;
    dev_uart_status_t st = DEV_UART_OK;
    bsp_uart_status_t bst;
    uint8_t *ptr = NULL;
    uint16_t chunk;

    if ((len == 0U) || (len > dev->tx_size))
    {
        return DEV_UART_ERR_PARAM;
    }

    if (osMutexAcquire(UART1_LOCKHandle, pdMS_TO_TICKS(timeout_ms)) != osOK)
    {
        return DEV_UART_ERR_STATE;
    }

    if (ringbuffer_space_len(&rt->tx_fifo) < (size_t)len)
    {
        (void)osMutexRelease(UART1_LOCKHandle);
        return DEV_UART_ERR_STATE;
    }

    (void)ringbuffer_put(&rt->tx_fifo, data, len);

    if (rt->tx_busy == 0U)
    {
        chunk = (uint16_t)ringbuffer_peek(&rt->tx_fifo, &ptr);
        if (chunk > 0U)
        {
            rt->tx_chunk = chunk;
            rt->tx_busy  = 1U;

            bst = BSP_UART_TxStart(dev->bsp, ptr, chunk);
            if (bst != BSP_UART_OK)
            {
                rt->tx_busy = 0U;
                st = Dev_UART_MapBsp(bst);
            }
        }
    }

    (void)osMutexRelease(UART1_LOCKHandle);

    return st;
}

/**
 * @brief 查询发送 FIFO 剩余空间
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 可写字节数
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev)
{
    return (uint16_t)ringbuffer_space_len(&dev->rt->tx_fifo);
}

/**
 * @brief 查询接收丢弃的累计字节数
 * @param dev UART 句柄
 * @retval 0 至 UINT32_MAX 累计丢失字节数
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev)
{
    return dev->rt->rx_lost;
}

/**
 * @brief 查询发送失败的累计段数
 * @param dev UART 句柄
 * @retval 0 至 UINT32_MAX 因错误中止的发送段数
 *
 * @note 失败段可能只发出一部分，帧完整性由协议层自行处理。
 */
uint32_t Dev_UART_TxErrors(const dev_uart_t *dev)
{
    return dev->rt->tx_errors;
}
