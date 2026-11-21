/**
 * @file dev_uart.c
 * @brief UART 虚拟外设：写入串行化、超时等待、发完判定
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 设计模型：
 *       1. 每个物理口一个设备对象（bsp 句柄 + 互斥量 + 运行期状态），
 *          对象本身不导出，上层只拿到设备句柄；
 *       2. 写：抢互斥量 → 循环把字节排进 bsp_uart 的发送 FIFO → 满了就让出
 *          CPU 等一拍，直到排完或超时 → 放锁。一次 Write 的字节在队列里连续；
 *       3. 读：先等够 len 字节（或超时），再一次性拷走已到达的部分。单消费者；
 *       4. 发完判定：轮询 bsp_uart 的"发送路径空闲"，每 1 ms 让出一次 CPU。
 *
 *       本层含 OS 调用（互斥量、延时、节拍），bsp_uart 不含任何 OS 调用。
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

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 设备运行时状态
 */
typedef struct {
    volatile uint8_t initialized; /* 1: 已完成初始化 */
} dev_uart_rt_t;

/**
 * @brief UART 设备对象（不出头文件）
 */
struct dev_uart {
    const bsp_uart_t *bsp;  /* 芯片侧 UART 句柄 */
    osMutexId_t      *lock; /* 指向 CubeMX 生成的互斥量句柄 */
    dev_uart_rt_t    *rt;   /* 运行期状态 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief USART1 的运行时状态 */
static dev_uart_rt_t dev_uart1_rt;

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

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化本层：检查互斥量并探测 bsp_uart 是否就绪
 * @retval DEV_UART_OK: 全部实例就绪
 * @retval DEV_UART_ERR_STATE: 缺 bsp 句柄、互斥量尚未创建，或 bsp_uart 未就绪
 * @note 探测走 BSP_UART_Is_Ready()，不读 BSP 句柄内部字段。
 */
dev_uart_status_t Dev_UART_Init(void)
{
    size_t i;

    for (i = 0U; i < s_dev_count; i++) {
        const dev_uart_t *dev = s_devs[i];

        if (dev->rt->initialized != 0U) {
            continue;
        }
        if (dev->bsp == NULL || dev->lock == NULL || *dev->lock == NULL) {
            return DEV_UART_ERR_STATE;
        }
        if (BSP_UART_Is_Ready(dev->bsp) == 0U) {
            return DEV_UART_ERR_STATE;
        }
        dev->rt->initialized = 1U;
    }
    return DEV_UART_OK;
}

/**
 * @brief 查询设备是否可用
 * @param dev 设备句柄，可为 NULL
 * @retval 1: 已初始化且 bsp_uart 就绪
 * @retval 0: 否
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev)
{
    if (dev == NULL || dev->rt->initialized == 0U) {
        return 0U;
    }
    return BSP_UART_Is_Ready(dev->bsp);
}

/**
 * @brief 查询接收 FIFO 中已收未读的字节数
 * @param dev 设备句柄，可为 NULL
 * @retval 可读字节数，0 表示没有数据或句柄非法
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return 0U;
    }
    return BSP_UART_Available(dev->bsp);
}

/**
 * @brief 窥视接收数据，不取走
 * @param dev 设备句柄，可为 NULL
 * @param buf 接收缓冲区，可为 NULL
 * @param len 最多看多少字节，不能为 0
 * @retval 实际看到的字节数
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, uint8_t *buf, uint16_t len)
{
    if (dev == NULL) {
        return 0U;
    }
    return BSP_UART_Peek(dev->bsp, buf, len);
}

/**
 * @brief 读取数据：等"够 len 字节"或超时，再取走当前全部可读字节
 * @param dev        设备句柄，可为 NULL
 * @param buf        接收缓冲区，不可为 NULL
 * @param len        期望字节数，不能为 0
 * @param timeout_ms 最多等多少毫秒；0 表示不等
 * @retval 实际取到的字节数（≤ len），0 表示超时且没有数据
 * @note 单消费者语义：同一口只允许一个任务调用。
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint8_t *buf, uint16_t len,
                       uint32_t timeout_ms)
{
    TickType_t ticks;
    TickType_t start;

    if (dev == NULL || buf == NULL || len == 0U ||
        dev->rt->initialized == 0U) {
        return 0U;
    }
    if (timeout_ms == 0U) {
        return BSP_UART_Read(dev->bsp, buf, len);
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return BSP_UART_Read(dev->bsp, buf, len);
    }

    ticks = pdMS_TO_TICKS(timeout_ms);
    start = xTaskGetTickCount();

    while (BSP_UART_Available(dev->bsp) < len) {
        if (Dev_UART_Expired(start, ticks) != 0U) {
            break;
        }
        (void)osDelay(1U);
    }

    return BSP_UART_Read(dev->bsp, buf, len);
}

/**
 * @brief 写入数据：等发送队列腾出空间，最多等到超时
 * @param dev        设备句柄，可为 NULL
 * @param buf        待发送数据，不可为 NULL
 * @param len        请求发送的字节数，不能为 0
 * @param timeout_ms 最多等多少毫秒；0 表示立刻返回
 * @retval 实际排入队列的字节数（≤ len）
 * @note 互斥量在整笔 Write 期间一直持有：既保证字节连续，也避免两个任务
 *       交替等待空间时互相插队。不得在中断中调用。
 */
uint16_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *buf, uint16_t len,
                        uint32_t timeout_ms)
{
    TickType_t ticks;
    TickType_t start;
    uint16_t done = 0U;

    if (dev == NULL || buf == NULL || len == 0U ||
        dev->rt->initialized == 0U) {
        return 0U;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return 0U;
    }

    ticks = pdMS_TO_TICKS((timeout_ms != 0U) ?
                          timeout_ms : DEV_UART_DEFAULT_TIMEOUT_MS);

    if (osMutexAcquire(*dev->lock, ticks) != osOK) {
        return 0U;
    }

    start = xTaskGetTickCount();

    while (done < len) {
        uint16_t n = BSP_UART_Write(dev->bsp, &buf[done],
                                    (uint16_t)(len - done));

        done = (uint16_t)(done + n);
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
 * @retval DEV_UART_ERR_STATE: 未初始化
 * @retval DEV_UART_ERR_TIMEOUT: 等锁或等发完超时
 * @note 等锁与等发完共用一份预算；持有互斥量期间其他写者会被挡住，
 *       这正是"把这条链路刷干净"的语义。
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms)
{
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

    ticks = pdMS_TO_TICKS((timeout_ms != 0U) ?
                          timeout_ms : DEV_UART_FLUSH_TIMEOUT_MS);

    if (osMutexAcquire(*dev->lock, ticks) != osOK) {
        return DEV_UART_ERR_TIMEOUT;
    }

    start = xTaskGetTickCount();

    while (BSP_UART_Tx_Idle(dev->bsp) == 0U) {
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
 * @brief 查询发送 FIFO 剩余可写空间
 * @param dev 设备句柄，可为 NULL
 * @retval 还能排入多少字节
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return 0U;
    }
    return BSP_UART_Tx_Free(dev->bsp);
}

/**
 * @brief 读取接收溢出计数
 * @param dev 设备句柄，可为 NULL
 * @retval 上电以来因 FIFO 满而丢弃的字节数
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return 0U;
    }
    return BSP_UART_Rx_Overflow(dev->bsp);
}

/**
 * @brief 清零接收溢出计数
 * @param dev 设备句柄，可为 NULL
 * @retval 无
 */
void Dev_UART_RxClearOverflow(const dev_uart_t *dev)
{
    if (dev == NULL) {
        return;
    }
    BSP_UART_Rx_Clear_Overflow(dev->bsp);
}
