/**
 * @file dev_uart.c
 * @brief UART 虚拟外设：接收/发送队列、溢出记账、写入串行化、超时等待
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 设计模型：
 *       1. 每个物理口一个设备对象（bsp 句柄 + 互斥量 + 运行块），对象本身
 *          不导出，上层只拿到设备句柄；
 *       2. 队列在本层：接收队列由中断侧写入、任务侧读取，发送队列由任务侧
 *          写入、中断侧取出；两个队列都是单生产者单消费者，索引只用无符号
 *          差值，不需要关中断也不用加锁；
 *       3. 本层把"取数据"和"推数据"两个钩子注入 bsp_uart，芯片层只负责把
 *          字节从寄存器搬到内存、再从内存搬回寄存器，不知道队列存在；
 *       4. 写：抢互斥量 → 往发送队列里塞 → 通知芯片层启动搬运 → 队列满就
 *          让出 CPU 等一拍，直到塞完或超时 → 放锁。一次 Write 的字节在队列
 *          里连续，不会被另一个任务插进去；
 *       5. 读：先等够 len 字节（或超时），再一次性取走；阻塞搬运档位下顺带
 *          轮询芯片层，让没有中断的裸机移植也能收到数据。
 *
 *       本文件是唯一含 OS 调用的地方（互斥量、延时、节拍）；要移植到裸机，
 *       只改本文件的等待部分，bsp_uart 与上层接口都不用动。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "dev_uart.h"
// 功能包含
#include "bsp_uart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

/*==============================================================================
 * 外部变量声明
 *============================================================================*/
/* ---- CubeMX 生成的互斥量：名字与 .ioc 里 FREERTOS.Mutexes01 一致 ---- */
extern osMutexId_t UART1_LOCKHandle;

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 写/读的默认超时（ms），调用者传 0 表示不等待（写）或取当前值（读） */
#define DEV_UART_DEFAULT_TIMEOUT_MS  50U

/** @brief 等待发完的默认超时（ms），调用者传 0 时使用 */
#define DEV_UART_FLUSH_TIMEOUT_MS    100U

/** @brief USART1 接收队列容量，必须是 2 的幂（索引用掩码回卷）
 *  @note 队列满时新字节被丢弃并累加溢出计数，不覆盖旧数据。 */
#define DEV_UART1_RX_BUF_SIZE        512U

/** @brief USART1 发送队列容量，必须是 2 的幂
 *  @note 容量决定"一帧一次 Write"能有多长；满了 Write 会等或截断。 */
#define DEV_UART1_TX_BUF_SIZE        512U

/** @brief 编译器屏障：保证数据写入先于索引发布，防止中断侧读到半成品 */
#define DEV_UART_BARRIER()          __asm volatile ("" : : : "memory")

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 设备运行块
 * @note 队列、索引、计数器都在这里：每个设备实例一份，不共享。
 */
typedef struct {
    uint8_t          *rx_buf;        /* 接收队列缓冲 */
    uint8_t          *tx_buf;        /* 发送队列缓冲 */
    uint16_t          rx_size;       /* 接收队列容量（2 的幂） */
    uint16_t          tx_size;       /* 发送队列容量（2 的幂） */
    volatile uint16_t rx_head;       /* 中断侧写入位置 */
    volatile uint16_t rx_tail;       /* 任务侧读取位置 */
    volatile uint16_t tx_head;       /* 任务侧写入位置 */
    volatile uint16_t tx_tail;       /* 中断侧取出位置 */
    volatile uint32_t rx_overflow;   /* 因队列满而丢弃的字节数 */
    volatile uint8_t  initialized;   /* 1: 已完成初始化 */
} dev_uart_rt_t;

/**
 * @brief UART 设备对象（不出头文件）
 */
struct dev_uart {
    const bsp_uart_t *bsp;  /* 芯片侧 UART 句柄 */
    osMutexId_t      *lock; /* 指向 CubeMX 生成的互斥量句柄 */
    dev_uart_rt_t    *rt;   /* 运行块 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief USART1 接收队列缓冲 */
static uint8_t s_dev_uart1_rx_buf[DEV_UART1_RX_BUF_SIZE];

/** @brief USART1 发送队列缓冲 */
static uint8_t s_dev_uart1_tx_buf[DEV_UART1_TX_BUF_SIZE];

/** @brief USART1 的运行块 */
static dev_uart_rt_t dev_uart1_rt = {
    .rx_buf = s_dev_uart1_rx_buf,
    .tx_buf = s_dev_uart1_tx_buf,
    .rx_size = DEV_UART1_RX_BUF_SIZE,
    .tx_size = DEV_UART1_TX_BUF_SIZE,
};

/** @brief USART1 设备对象；不导出，只经设备句柄间接使用 */
const dev_uart_t dev_uart1 = {
    .bsp  = &bsp_uart1,
    .lock = &UART1_LOCKHandle,
    .rt   = &dev_uart1_rt,
};

/** @brief 设备表：初始化按它轮询 */
static const dev_uart_t *const s_devs[] = {
    &dev_uart1,
};

/** @brief 设备表长度：由表的行数算出 */
static const size_t s_dev_count = sizeof(s_devs) / sizeof(s_devs[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 判断从 start 起是否已经用满 ticks 个节拍
 * @param start 起始节拍
 * @param ticks 预算节拍数
 * @retval 1: 已到或已超
 * @retval 0: 还没到
 * @note 用无符号差值比较，节拍计数回绕时仍然正确。
 */
static uint8_t Dev_UART_Expired(TickType_t start, TickType_t ticks)
{
    return (((TickType_t)(xTaskGetTickCount() - start) >= ticks) ? 1U : 0U);
}

/**
 * @brief 队列中已排队的字节数
 * @param head 写位置
 * @param tail 读位置
 * @param size 容量（2 的幂）
 * @retval 已用容量
 * @note 无符号减法天然处理回卷。
 */
static uint16_t Dev_UART_Used(uint16_t head, uint16_t tail, uint16_t size)
{
    return (uint16_t)((uint16_t)(head - tail) & (uint16_t)(size - 1U));
}

/**
 * @brief 接收钩子：中断侧把芯片层交来的字节压进接收队列
 * @param ctx 运行块（注册钩子时传的是 rt，不是设备句柄）
 * @param data 字节起始地址
 * @param len 字节数
 * @retval 无
 * @note 中断上下文调用。队列满时丢弃剩余字节并累加溢出计数，不覆盖旧数据；
 *       rx_head 只在最后发布一次，所以任务侧要么看到整段、要么看不到。
 */
static void Dev_UART_OnRxPush(void *ctx, const uint8_t *data, uint16_t len)
{
    dev_uart_rt_t *rt = (dev_uart_rt_t *)ctx;
    uint16_t head = rt->rx_head;
    uint16_t tail = rt->rx_tail;
    uint16_t size = rt->rx_size;
    uint16_t space = (uint16_t)((uint16_t)(size - 1U) - Dev_UART_Used(head, tail, size));
    uint16_t n = len;
    uint16_t i;

    if (n > space) {
        n = space;
    }

    for (i = 0U; i < n; i++) {
        rt->rx_buf[head] = data[i];
        head = (uint16_t)((uint16_t)(head + 1U) & (uint16_t)(size - 1U));
    }

    DEV_UART_BARRIER();
    rt->rx_head = head;

    if (n < len) {
        rt->rx_overflow += (uint32_t)(len - n);
    }
}

/**
 * @brief 发送钩子：中断侧从发送队列里取一段交给芯片层
 * @param ctx 运行块
 * @param buf 芯片层的暂存区
 * @param max 最多取多少字节
 * @retval 实际取出的字节数，0 表示队列空
 * @note 中断上下文与任务上下文都可能调用（阻塞档由任务直接取），所以索引
 *       更新只做一次保存；tail 只有消费侧写，不存在两个消费者。
 */
static uint16_t Dev_UART_OnTxFetch(void *ctx, uint8_t *buf, uint16_t max)
{
    dev_uart_rt_t *rt = (dev_uart_rt_t *)ctx;
    uint16_t head = rt->tx_head;
    uint16_t tail = rt->tx_tail;
    uint16_t size = rt->tx_size;
    uint16_t n = Dev_UART_Used(head, tail, size);
    uint16_t i;

    if (n > max) {
        n = max;
    }

    for (i = 0U; i < n; i++) {
        buf[i] = rt->tx_buf[tail];
        tail = (uint16_t)((uint16_t)(tail + 1U) & (uint16_t)(size - 1U));
    }

    DEV_UART_BARRIER();
    rt->tx_tail = tail;
    return n;
}

/**
 * @brief 从发送队列取走已入队的前 n 字节（不回读数据）
 * @param rt 运行块
 * @param n 已排入的字节数
 * @retval 无
 * @note 只在 Write 里调用：数据已拷进队列，这里只推进写位置并发布。
 */
static void Dev_UART_TxCommit(dev_uart_rt_t *rt, uint16_t n)
{
    uint16_t head = rt->tx_head;

    head = (uint16_t)((uint16_t)(head + n) & (uint16_t)(rt->tx_size - 1U));
    DEV_UART_BARRIER();
    rt->tx_head = head;
}

/**
 * @brief 把用户缓冲拷进发送队列的空位
 * @param rt 运行块
 * @param data 数据起始地址
 * @param len 数据长度
 * @retval 实际排入的字节数
 * @note 不推进写位置，由 Dev_UART_TxCommit() 统一发布，保证一次 Write 的
 *       字节对消费者是一整段。
 */
static uint16_t Dev_UART_TxCopy(dev_uart_rt_t *rt, const uint8_t *data,
                                uint16_t len)
{
    uint16_t head = rt->tx_head;
    uint16_t size = rt->tx_size;
    uint16_t i;

    for (i = 0U; i < len; i++) {
        rt->tx_buf[head] = data[i];
        head = (uint16_t)((uint16_t)(head + 1U) & (uint16_t)(size - 1U));
    }
    return len;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化本层：检查互斥量、注册钩子、启动芯片侧接收
 * @retval DEV_UART_OK: 全部实例就绪
 * @retval DEV_UART_ERR_STATE: 缺 bsp 句柄、互斥量尚未创建，或芯片层拒绝启动
 * @retval DEV_UART_ERR_PARAM: 实例配置非法（队列容量不是 2 的幂或缓冲为空）
 * @note 必须在 BSP_UART_Init() 之后调用；本函数把收/发钩子注入芯片层，
 *       所以之后芯片层的搬运不再经过队列以外的中间层。
 */
dev_uart_status_t Dev_UART_Init(void)
{
    size_t i;

    for (i = 0U; i < s_dev_count; i++) {
        const dev_uart_t *dev = s_devs[i];
        dev_uart_rt_t *rt = dev->rt;
        bsp_uart_hooks_t hooks;
        bsp_uart_status_t st;

        if (rt->initialized != 0U) {
            continue;
        }
        if ((dev->bsp == NULL) || (dev->lock == NULL) || (*dev->lock == NULL)) {
            return DEV_UART_ERR_STATE;
        }
        if ((rt->rx_buf == NULL) || (rt->tx_buf == NULL) ||
            (rt->rx_size < 2U) || (rt->tx_size < 2U) ||
            ((rt->rx_size & (uint16_t)(rt->rx_size - 1U)) != 0U) ||
            ((rt->tx_size & (uint16_t)(rt->tx_size - 1U)) != 0U)) {
            return DEV_UART_ERR_PARAM;
        }

        hooks.rx_push = Dev_UART_OnRxPush;
        hooks.tx_fetch = Dev_UART_OnTxFetch;
        hooks.ctx = rt;

        st = BSP_UART_Start(dev->bsp, &hooks);
        if (st != BSP_UART_OK) {
            return DEV_UART_ERR_STATE;
        }

        rt->rx_head = 0U;
        rt->rx_tail = 0U;
        rt->tx_head = 0U;
        rt->tx_tail = 0U;
        rt->rx_overflow = 0U;
        rt->initialized = 1U;
    }
    return DEV_UART_OK;
}

/**
 * @brief 查询设备是否可用
 * @param dev 设备句柄，可为 NULL
 * @retval 1: 已初始化且芯片层就绪
 * @retval 0: 否
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt->initialized == 0U)) {
        return 0U;
    }
    return BSP_UART_IsReady(dev->bsp);
}

/**
 * @brief 查询接收队列中已收未读的字节数
 * @param dev 设备句柄，可为 NULL
 * @retval 可读字节数，0 表示没有数据或句柄非法
 * @note 单消费者语义：同一口只允许一个任务调用。
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return 0U;
    }
    return Dev_UART_Used(dev->rt->rx_head, dev->rt->rx_tail, dev->rt->rx_size);
}

/**
 * @brief 窥视接收数据，不取走
 * @param dev 设备句柄，可为 NULL
 * @param buf 接收缓冲区，可为 NULL（此时只返回可窥视长度）
 * @param len 最多看多少字节，不能为 0
 * @retval 实际看到的字节数
 * @note 只读不推进读位置；与 Read 一样是单消费者语义。
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, uint8_t *buf, uint16_t len)
{
    dev_uart_rt_t *rt;
    uint16_t tail;
    uint16_t avail;
    uint16_t n;
    uint16_t i;

    if ((dev == NULL) || (len == 0U)) {
        return 0U;
    }

    rt = dev->rt;
    avail = Dev_UART_Used(rt->rx_head, rt->rx_tail, rt->rx_size);
    n = (avail < len) ? avail : len;

    if (buf != NULL) {
        tail = rt->rx_tail;
        for (i = 0U; i < n; i++) {
            buf[i] = rt->rx_buf[tail];
            tail = (uint16_t)((uint16_t)(tail + 1U) & (uint16_t)(rt->rx_size - 1U));
        }
    }
    return n;
}

/**
 * @brief 读取数据：等"够 len 字节"或超时，再取走当前可读的部分
 * @param dev        设备句柄，可为 NULL
 * @param buf        接收缓冲区，不可为 NULL
 * @param len        期望字节数，不能为 0
 * @param timeout_ms 最多等多少毫秒；0 表示不等，取当前可读的部分
 * @retval 实际取到的字节数（≤ len），0 表示超时且没有数据
 * @note 等待期间每 1 ms 让出一次 CPU，并顺带轮询芯片层——芯片层退到阻塞
 *       档位时没有中断可用，只能靠这次轮询把字节取上来。
 *       返回时可能不足 len：超时、或对端只发来这么多。
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint8_t *buf, uint16_t len,
                       uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    TickType_t ticks = 0U;
    TickType_t start = 0U;
    uint16_t avail;
    uint16_t tail;
    uint16_t n;
    uint16_t i;

    if ((dev == NULL) || (buf == NULL) || (len == 0U) ||
        (dev->rt->initialized == 0U)) {
        return 0U;
    }

    rt = dev->rt;

    if ((timeout_ms != 0U) &&
        (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
        ticks = pdMS_TO_TICKS(timeout_ms);
        start = xTaskGetTickCount();
    }

    for (;;) {
        avail = Dev_UART_Used(rt->rx_head, rt->rx_tail, rt->rx_size);
        if (avail >= len) {
            break;
        }
        if (ticks == 0U) {
            break;                       /* 不等待：取当前值 */
        }
        if (Dev_UART_Expired(start, ticks) != 0U) {
            break;
        }
        (void)BSP_UART_Poll(dev->bsp);   /* 阻塞档位靠它取字节 */
        (void)osDelay(1U);
    }

    avail = Dev_UART_Used(rt->rx_head, rt->rx_tail, rt->rx_size);
    if ((avail == 0U) || (buf == NULL)) {
        return 0U;
    }
    n = (avail < len) ? avail : len;

    tail = rt->rx_tail;
    for (i = 0U; i < n; i++) {
        buf[i] = rt->rx_buf[tail];
        tail = (uint16_t)((uint16_t)(tail + 1U) & (uint16_t)(rt->rx_size - 1U));
    }
    DEV_UART_BARRIER();
    rt->rx_tail = tail;
    return n;
}

/**
 * @brief 写入数据：等发送队列腾出空间，最多等到超时
 * @param dev        设备句柄，可为 NULL
 * @param buf        待发送数据，不可为 NULL
 * @param len        请求发送的字节数，不能为 0
 * @param timeout_ms 最多等多少毫秒；0 表示用默认 50 ms
 * @retval 实际排入队列的字节数（≤ len）
 * @note 互斥量在整笔 Write 期间一直持有：既保证字节连续，也避免两个任务
 *       交替等待空间时互相插队。排入后立刻通知芯片层启动搬运。
 *       不得在中断中调用。
 */
uint16_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *buf, uint16_t len,
                        uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    TickType_t ticks;
    TickType_t start;
    uint16_t done = 0U;

    if ((dev == NULL) || (buf == NULL) || (len == 0U) ||
        (dev->rt->initialized == 0U)) {
        return 0U;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return 0U;
    }

    rt = dev->rt;
    ticks = pdMS_TO_TICKS((timeout_ms != 0U) ?
                          timeout_ms : DEV_UART_DEFAULT_TIMEOUT_MS);

    if (osMutexAcquire(*dev->lock, ticks) != osOK) {
        return 0U;
    }

    start = xTaskGetTickCount();

    while (done < len) {
        uint16_t used = Dev_UART_Used(rt->tx_head, rt->tx_tail, rt->tx_size);
        uint16_t space = (uint16_t)((uint16_t)(rt->tx_size - 1U) - used);
        uint16_t chunk = (uint16_t)(len - done);

        if (chunk > space) {
            chunk = space;
        }
        if (chunk > 0U) {
            done = (uint16_t)(done + Dev_UART_TxCopy(rt, &buf[done], chunk));
            Dev_UART_TxCommit(rt, chunk);
            (void)BSP_UART_TxKick(dev->bsp);
        }
        if (done >= len) {
            break;
        }
        if (Dev_UART_Expired(start, ticks) != 0U) {
            break;
        }
        (void)osDelay(1U);
    }

    (void)osMutexRelease(*dev->lock);
    return done;
}

/**
 * @brief 等待发送队列排空且最后一个字节移出移位寄存器
 * @param dev        设备句柄，可为 NULL
 * @param timeout_ms 超时预算（毫秒），0 表示用默认 100 ms
 * @retval DEV_UART_OK: 已发完
 * @retval DEV_UART_ERR_PARAM: 句柄非法
 * @retval DEV_UART_ERR_STATE: 未初始化，或当前不在任务上下文
 * @retval DEV_UART_ERR_TIMEOUT: 等锁或等发完超时
 * @note 等锁与等发完共用一份预算；持有互斥量期间其他写者会被挡住，
 *       这正是"把这条链路刷干净"的语义。
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    TickType_t ticks;
    TickType_t start;

    if (dev == NULL) {
        return DEV_UART_ERR_PARAM;
    }
    if (dev->rt->initialized == 0U) {
        return DEV_UART_ERR_STATE;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return DEV_UART_ERR_STATE;
    }

    rt = dev->rt;
    ticks = pdMS_TO_TICKS((timeout_ms != 0U) ?
                          timeout_ms : DEV_UART_FLUSH_TIMEOUT_MS);

    if (osMutexAcquire(*dev->lock, ticks) != osOK) {
        return DEV_UART_ERR_TIMEOUT;
    }

    start = xTaskGetTickCount();

    for (;;) {
        uint16_t used = Dev_UART_Used(rt->tx_head, rt->tx_tail, rt->tx_size);

        if ((used == 0U) && (BSP_UART_TxIdle(dev->bsp) != 0U)) {
            break;
        }
        if (Dev_UART_Expired(start, ticks) != 0U) {
            (void)osMutexRelease(*dev->lock);
            return DEV_UART_ERR_TIMEOUT;
        }
        (void)osDelay(1U);
    }

    (void)osMutexRelease(*dev->lock);
    return DEV_UART_OK;
}

/**
 * @brief 查询发送队列剩余可写空间
 * @param dev 设备句柄，可为 NULL
 * @retval 还能排入多少字节
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return 0U;
    }
    return (uint16_t)((uint16_t)(dev->rt->tx_size - 1U) -
                      Dev_UART_Used(dev->rt->tx_head, dev->rt->tx_tail,
                                    dev->rt->tx_size));
}

/**
 * @brief 读取接收溢出计数
 * @param dev 设备句柄，可为 NULL
 * @retval 上电以来因队列满而丢弃的字节数
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return 0U;
    }
    return dev->rt->rx_overflow;
}

/**
 * @brief 清零接收溢出计数
 * @param dev 设备句柄，可为 NULL
 * @retval 无
 * @note 与中断侧的累加存在极小的竞争窗口：清零与"刚好又溢出"重叠时可能
 *       丢掉一次计入，只影响统计，不影响数据。
 */
void Dev_UART_RxClearOverflow(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return;
    }
    dev->rt->rx_overflow = 0U;
}
