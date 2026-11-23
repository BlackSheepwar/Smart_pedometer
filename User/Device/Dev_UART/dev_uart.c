/**
 * @file dev_uart.c
 * @brief UART 虚拟外设缓冲、任务侧搬运与线程同步
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 设计模型：
 *       1. 中断只发布：循环 DMA 的 HT / TC / IDLE 事件在 BSP 里换算成绝对
 *          接收计数，本层回调只置发布位置并用 ISR 安全接口通知服务任务。
 *       2. 搬运在任务：服务任务按"绝对发布量 - 已搬走量"求增量，从 DEV
 *          持有的 DMA 缓冲分两段拷进接收 FIFO；任务延迟超过一个环形窗口
 *          时只保留窗口内最新数据并计入丢失。
 *       3. 两个 FIFO：接收 FIFO 是服务任务与一个应用消费者之间的 SPSC；
 *          发送 FIFO 是多写者（互斥量串行化）与服务任务推进。所有跨上下文
 *          访问都用端口临界区串行化，弥补第三方库未声明内存序的缺口。
 *       4. 发送完成以 USART TC 为准：BSP 的完成回调对应最后一位离开发送器，
 *          不会把"数据已交给移位寄存器"误当成"线路已发完"。
 *       5. 单帧必须完整：写者等够整帧空间才入队，帧不会半截进入发送 FIFO。
 *       6. OS 依赖暂时内联：当前直接用 FreeRTOS / CMSIS-OS v2 提供节拍、
 *          互斥量、任务通知与短临界区。OSAL 解耦层重建后，把 OS 绑定一节
 *          抽回端口文件即可，收发核心逻辑不动。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dev_uart.h"
// 功能包含
#include "FreeRTOS.h"
#include "bsp_uart.h"
#include "cmsis_os.h"
#include "ringbuffer.h"
#include "stm32f4xx.h"

/*==============================================================================
 * 外部变量声明
 *============================================================================*/
/** @brief CubeMX 生成的 UART1 发送互斥量 */
extern osMutexId_t UART1_LOCKHandle;

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单个接收 DMA 节点字节数；缓冲总长为其 2 倍 */
#define DEV_UART_DMA_NODE       128U
/** @brief 双节点接收 DMA 缓冲总字节数 */
#define DEV_UART_DMA_BYTES      (DEV_UART_DMA_NODE * 2U)
/** @brief 接收 FIFO 容量，第三方库上限 32767 */
#define DEV_UART_RX_FIFO_BYTES  512U
/** @brief 发送 FIFO 容量，第三方库上限 32767 */
#define DEV_UART_TX_FIFO_BYTES  512U
/** @brief 单次临界区最多搬运的字节数，限制关中断时长 */
#define DEV_UART_COPY_CHUNK     32U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/** @brief UART 虚拟外设的运行块 */
typedef struct {
    ringbuffer_t rx_fifo;        /* 接收 FIFO：服务任务生产、应用消费 */
    ringbuffer_t tx_fifo;        /* 发送 FIFO：写者生产、服务任务推进 */
    uint32_t     rx_base;        /* 最近一次武装成功时的绝对接收基准 */
    uint32_t     rx_cons;        /* 已从 DMA 缓冲搬走的绝对字节数 */
    uint32_t     rx_lost;        /* DMA 覆盖与接收 FIFO 丢弃累计 */
    uint32_t     tx_errors;      /* 发送失败段累计 */
    volatile uint8_t  tx_busy;   /* 1 = 有一段 DMA 发送在途 */
    volatile uint16_t tx_chunk;  /* 在途段字节数，完成回调后从 FIFO 消费 */
    volatile uint8_t  tx_done;   /* 1 = 完成回调已到，待服务任务归还 */
    volatile uint8_t  tx_ok;     /* 最近完成回调结局：1 = TC，0 = 错误中止 */
    uint8_t      ready;          /* 1 = 设备层服务可用 */
    uint8_t      started;        /* 1 = 初始化已完成 */
} dev_uart_rt_t;

/** @brief UART 虚拟外设句柄布局 */
struct dev_uart {
    const bsp_uart_t *bsp;          /* 芯片层总线句柄 */
    uint8_t  *dma_buf;              /* 双节点接收 DMA 缓冲 */
    uint8_t  *rx_pool;              /* 接收 FIFO 存储池 */
    uint8_t  *tx_pool;              /* 发送 FIFO 存储池 */
    uint16_t  dma_node;             /* 单个 DMA 节点字节数 */
    uint16_t  rx_size;              /* 接收 FIFO 容量 */
    uint16_t  tx_size;              /* 发送 FIFO 容量 */
    dev_uart_rt_t *rt;              /* 本实例的运行块 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief UART 服务任务句柄，由服务任务启动时注册 */
static TaskHandle_t s_uart_service_task;

/** @brief UART1 的双节点接收 DMA 缓冲 */
static uint8_t s_uart1_dma[DEV_UART_DMA_BYTES] __attribute__((aligned(4)));
/** @brief UART1 的接收 FIFO 存储池 */
static uint8_t s_uart1_rx_pool[DEV_UART_RX_FIFO_BYTES] __attribute__((aligned(4)));
/** @brief UART1 的发送 FIFO 存储池 */
static uint8_t s_uart1_tx_pool[DEV_UART_TX_FIFO_BYTES];
/** @brief UART1 的运行块，与句柄实例成对定义 */
static dev_uart_rt_t dev_uart1_rt;

/** @brief UART1 虚拟外设配置句柄 */
const dev_uart_t dev_uart1 = {
    .bsp      = &bsp_uart1,
    .dma_buf  = s_uart1_dma,
    .rx_pool  = s_uart1_rx_pool,
    .tx_pool  = s_uart1_tx_pool,
    .dma_node = DEV_UART_DMA_NODE,
    .rx_size  = DEV_UART_RX_FIFO_BYTES,
    .tx_size  = DEV_UART_TX_FIFO_BYTES,
    .rt       = &dev_uart1_rt
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
 * @retval DEV_UART_ERR_TIMEOUT: 超时
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
        case BSP_UART_ERR_TIMEOUT:
            return DEV_UART_ERR_TIMEOUT;
        case BSP_UART_ERR_HAL:
        default:
            return DEV_UART_ERR_HAL;
    }
}

/**
 * @brief 校验句柄、缓冲与互斥量是否可用于收发
 * @param dev UART 虚拟外设句柄
 * @retval 1: 可用
 * @retval 0: 句柄、运行块、缓冲或互斥量非法
 */
static uint8_t Dev_UART_ConfigValid(const dev_uart_t *dev)
{
    uint32_t node;

    if ((dev == NULL) || (dev->rt == NULL) || (dev->bsp == NULL) ||
        (dev->dma_buf == NULL) || (dev->rx_pool == NULL) ||
        (dev->tx_pool == NULL) || (UART1_LOCKHandle == NULL))
    {
        return 0U;
    }

    node = (uint32_t)dev->dma_node;
    if ((node < 16U) || (node > 16384U) || ((node & (node - 1U)) != 0U))
    {
        return 0U;
    }

    if ((dev->rx_size == 0U) || (dev->rx_size > 32767U) ||
        (dev->tx_size == 0U) || (dev->tx_size > 32767U))
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief 读取 FreeRTOS 系统节拍
 * @retval 当前系统 tick
 */
static uint32_t Dev_UART_Ticks(void)
{
    return osKernelGetTickCount();
}

/**
 * @brief 进入短临界区
 * @retval 进入前的中断屏蔽状态
 */
static uint32_t Dev_UART_CriticalEnter(void)
{
    uint32_t state = __get_PRIMASK();

    __disable_irq();
    return state;
}

/**
 * @brief 按进入前状态退出短临界区
 * @param state 进入前的中断屏蔽状态
 * @retval 无
 */
static void Dev_UART_CriticalExit(uint32_t state)
{
    if (state == 0U)
    {
        __enable_irq();
    }
}

/**
 * @brief 在限定时间内获取 UART 发送互斥量
 * @param timeout_ms 等待上限，单位毫秒
 * @retval 1: 成功获取
 * @retval 0: 互斥量尚未创建或等待超时
 */
static uint8_t Dev_UART_Lock(uint32_t timeout_ms)
{
    if (UART1_LOCKHandle == NULL)
    {
        return 0U;
    }

    return (osMutexAcquire(UART1_LOCKHandle, pdMS_TO_TICKS(timeout_ms)) == osOK) ?
           1U : 0U;
}

/**
 * @brief 释放 UART 发送互斥量
 * @retval 无
 */
static void Dev_UART_Unlock(void)
{
    if (UART1_LOCKHandle != NULL)
    {
        (void)osMutexRelease(UART1_LOCKHandle);
    }
}

/**
 * @brief 等待服务任务的直接通知
 * @param timeout_ms 等待上限，单位毫秒
 * @retval 1: 收到通知
 * @retval 0: 等待超时
 */
static uint8_t Dev_UART_NotifyWait(uint32_t timeout_ms)
{
    return (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) != 0U) ? 1U : 0U;
}

/**
 * @brief 延时当前调用任务
 * @param timeout_ms 延时时间，单位毫秒
 * @retval 无
 */
static void Dev_UART_Delay(uint32_t timeout_ms)
{
    (void)osDelay(timeout_ms);
}

/**
 * @brief 从中断上下文安全地通知服务任务
 * @retval 无
 *
 * @note 服务任务尚未注册时不通知；其有限等待超时可恢复处理。
 */
static void Dev_UART_SignalIsr(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t task = s_uart_service_task;

    if (task != NULL)
    {
        vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/**
 * @brief 按超时判定是否已经用尽等待窗口
 * @param start      开始计时的节拍
 * @param timeout_ms 允许等待的毫秒数
 * @retval 1: 已到或超过期限
 * @retval 0: 仍在期限内
 *
 * @note 用无符号减法比较，节拍回绕也成立。
 */
static uint8_t Dev_UART_Expired(uint32_t start, uint32_t timeout_ms)
{
    return ((uint32_t)(Dev_UART_Ticks() - start) >= timeout_ms) ? 1U : 0U;
}

/**
 * @brief 接收搬运：把 DMA 缓冲里的新增字节搬进接收 FIFO
 * @param dev UART 虚拟外设句柄
 * @retval 无
 *
 * @note 只允许服务任务调用。增量超过一个环形窗口时，窗口外的旧字节已经
 *       被 DMA 覆盖，只保留窗口内最新数据并计入丢失；接收 FIFO 满时丢
 *       最新字节（保留已缓存的旧数据）。
 */
static void Dev_UART_RxPump(const dev_uart_t *dev)
{
    dev_uart_rt_t *rt = dev->rt;
    uint32_t ring = (uint32_t)dev->dma_node * 2U;
    uint32_t pub = BSP_UART_RxTotal(dev->bsp);
    uint32_t len;
    uint32_t space;
    uint32_t chunk;
    uint32_t off;
    uint32_t state;
    uint16_t first;
    size_t copied;

    if ((uint32_t)(pub - rt->rx_cons) > ring)
    {
        rt->rx_lost += (uint32_t)(pub - rt->rx_cons) - ring;
        rt->rx_cons = pub - ring;
    }

    len = pub - rt->rx_cons;
    while (len > 0U)
    {
        state = Dev_UART_CriticalEnter();
        space = (uint32_t)ringbuffer_space_len(&rt->rx_fifo);
        Dev_UART_CriticalExit(state);

        if (space == 0U)
        {
            rt->rx_lost += len;
            rt->rx_cons += len;
            break;
        }

        chunk = len;
        if (chunk > space)
        {
            chunk = space;
        }
        if (chunk > DEV_UART_COPY_CHUNK)
        {
            chunk = DEV_UART_COPY_CHUNK;
        }

        off = (rt->rx_cons - rt->rx_base) & (ring - 1U);
        first = (uint16_t)(ring - off);
        if ((uint32_t)first > chunk)
        {
            first = (uint16_t)chunk;
        }

        state = Dev_UART_CriticalEnter();
        copied = ringbuffer_put(&rt->rx_fifo, &dev->dma_buf[off], first);
        if (chunk > (uint32_t)first)
        {
            copied += ringbuffer_put(&rt->rx_fifo, dev->dma_buf,
                                     (uint16_t)(chunk - (uint32_t)first));
        }
        Dev_UART_CriticalExit(state);

        if (copied == 0U)
        {
            break;
        }

        if ((uint32_t)copied < chunk)
        {
            rt->rx_lost += chunk - (uint32_t)copied;
        }
        rt->rx_cons += chunk;
        len -= chunk;
    }
}

/**
 * @brief 发送推进：归还完成段、给队首连续段启动 DMA
 * @param dev UART 虚拟外设句柄
 * @retval DEV_UART_OK: 没有待办或已成功启动一段
 * @retval DEV_UART_ERR_PARAM: 参数非法
 * @retval DEV_UART_ERR_STATE: 芯片层状态不允许
 * @retval DEV_UART_ERR_TIMEOUT: 芯片层暂时忙
 * @retval DEV_UART_ERR_HAL: 芯片层启动失败
 *
 * @note 可由服务任务与写者调用；在途标志与完成标志用临界区串行化，
 *       保证同一时刻只有一段 DMA 在途。
 */
static dev_uart_status_t Dev_UART_TxPump(const dev_uart_t *dev)
{
    dev_uart_rt_t *rt = dev->rt;
    bsp_uart_status_t bst;
    uint32_t state;
    uint16_t chunk = 0U;
    uint16_t len;
    uint8_t  ok = 1U;
    uint8_t  done = 0U;
    uint8_t  busy;
    uint8_t *ptr = NULL;

    state = Dev_UART_CriticalEnter();
    if (rt->tx_done != 0U)
    {
        done = 1U;
        chunk = rt->tx_chunk;
        ok = rt->tx_ok;
        rt->tx_done = 0U;
    }
    Dev_UART_CriticalExit(state);

    if (done != 0U)
    {
        state = Dev_UART_CriticalEnter();
        if (ok == 0U)
        {
            rt->tx_errors++;
        }
        (void)ringbuffer_consume(&rt->tx_fifo, (size_t)chunk);
        Dev_UART_CriticalExit(state);
    }

    state = Dev_UART_CriticalEnter();
    busy = rt->tx_busy;
    len  = (uint16_t)ringbuffer_peek(&rt->tx_fifo, &ptr);
    if ((busy == 0U) && (len > 0U))
    {
        rt->tx_busy  = 1U;
        rt->tx_chunk = len;
    }
    else
    {
        len = 0U;
    }
    Dev_UART_CriticalExit(state);

    if (len == 0U)
    {
        return DEV_UART_OK;
    }

    bst = BSP_UART_TxStart(dev->bsp, ptr, len);
    if (bst != BSP_UART_OK)
    {
        state = Dev_UART_CriticalEnter();
        rt->tx_busy  = 0U;
        rt->tx_chunk = 0U;
        Dev_UART_CriticalExit(state);
        return Dev_UART_MapBsp(bst);
    }

    return DEV_UART_OK;
}

/**
 * @brief 接收事件回调：只发 ISR 安全通知，不搬运数据
 * @param ctx    回调上下文（设备句柄）
 * @param events 芯片层事件位
 * @param total  自武装以来累计接收字节数
 * @retval 无
 *
 * @note 运行于中断上下文；搬运与重装留给服务任务，避免中断时长乘以
 *       数据量。
 */
static void Dev_UART_OnRx(void *ctx, uint32_t events, uint32_t total)
{
    (void)events;
    (void)total;

    if (ctx != NULL)
    {
        Dev_UART_SignalIsr();
    }
}

/**
 * @brief 发送完成回调：记录结局并唤醒服务任务归还队列
 * @param ctx 回调上下文（设备句柄）
 * @param ok  1 = 线路 TC 完成；0 = 错误中止
 * @retval 无
 *
 * @note 运行于中断上下文；只置标志与通知，出队由服务任务完成。
 */
static void Dev_UART_OnTxDone(void *ctx, uint8_t ok)
{
    const dev_uart_t *dev = (const dev_uart_t *)ctx;
    dev_uart_rt_t *rt;

    if ((dev == NULL) || (dev->rt == NULL))
    {
        return;
    }

    rt = dev->rt;
    rt->tx_busy = 0U;
    rt->tx_ok   = (ok != 0U) ? 1U : 0U;
    rt->tx_done = 1U;

    Dev_UART_SignalIsr();
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 注册当前任务为 UART 服务任务
 * @retval 无
 *
 * @note 必须在服务任务里调用一次，且之后由该任务调用 Dev_UART_Wait() /
 *       Dev_UART_Service()。中断只向这个任务发直接通知，未注册时通知被
 *       丢弃，服务退化为 Dev_UART_Wait() 的有限超时轮询。
 */
void Dev_UART_RegisterTask(void)
{
    s_uart_service_task = xTaskGetCurrentTaskHandle();
}

/**
 * @brief 初始化 UART1 虚拟外设
 * @retval DEV_UART_OK: 成功或已就绪
 * @retval DEV_UART_ERR_PARAM: 句柄、缓冲或互斥句柄非法
 * @retval DEV_UART_ERR_STATE: 芯片层接收启动被拒绝
 * @retval DEV_UART_ERR_TIMEOUT: 芯片层接收启动超时
 * @retval DEV_UART_ERR_HAL: 芯片层接收启动失败
 */
dev_uart_status_t Dev_UART_Init(void)
{
    const dev_uart_t *dev = &dev_uart1;
    dev_uart_rt_t *rt;
    bsp_uart_port_t port;
    bsp_uart_status_t bst;
    uint32_t total;

    if (Dev_UART_ConfigValid(dev) == 0U)
    {
        return DEV_UART_ERR_PARAM;
    }

    rt = dev->rt;
    if (rt->started != 0U)
    {
        return DEV_UART_OK;
    }

    (void)memset(rt, 0, sizeof(*rt));
    ringbuffer_init(&rt->rx_fifo, dev->rx_pool, dev->rx_size);
    ringbuffer_init(&rt->tx_fifo, dev->tx_pool, dev->tx_size);

    port.dma_buf    = dev->dma_buf;
    port.dma_node   = dev->dma_node;
    port.on_rx_event = Dev_UART_OnRx;
    port.on_tx_done  = Dev_UART_OnTxDone;
    port.ctx         = (void *)dev;

    bst = BSP_UART_Start(dev->bsp, &port);
    if (bst != BSP_UART_OK)
    {
        return Dev_UART_MapBsp(bst);
    }

    total = BSP_UART_RxTotal(dev->bsp);
    rt->rx_base = total;
    rt->rx_cons = total;
    rt->ready   = 1U;
    rt->started = 1U;

    return DEV_UART_OK;
}

/**
 * @brief 任务侧服务：搬运新增接收字节并推进发送
 * @param dev UART 句柄
 * @retval DEV_UART_OK: 本轮服务完成
 * @retval DEV_UART_ERR_PARAM: 句柄非法
 * @retval DEV_UART_ERR_STATE: UART 尚未初始化
 * @retval DEV_UART_ERR_TIMEOUT: 芯片层重新武装暂时失败
 * @retval DEV_UART_ERR_HAL: 芯片层重新武装失败
 */
dev_uart_status_t Dev_UART_Service(const dev_uart_t *dev)
{
    dev_uart_rt_t *rt;
    bsp_uart_status_t bst;
    dev_uart_status_t st;

    if ((dev == NULL) || (dev->rt == NULL) || (dev->bsp == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }

    rt = dev->rt;
    if (rt->started == 0U)
    {
        return DEV_UART_ERR_STATE;
    }

    Dev_UART_RxPump(dev);

    if (BSP_UART_IsReady(dev->bsp) == 0U)
    {
        uint32_t total = BSP_UART_RxTotal(dev->bsp);

        bst = BSP_UART_RxRearm(dev->bsp);
        if (bst == BSP_UART_OK)
        {
            rt->rx_base = total;
            rt->rx_cons = total;
            rt->ready   = 1U;
        }
        else if (bst == BSP_UART_ERR_STATE)
        {
            rt->ready = 0U;
        }
        else if (bst == BSP_UART_ERR_TIMEOUT)
        {
            rt->ready = 0U;
        }
        else
        {
            rt->ready = 0U;
            return Dev_UART_MapBsp(bst);
        }
    }

    st = Dev_UART_TxPump(dev);
    if (st != DEV_UART_OK)
    {
        return st;
    }

    return DEV_UART_OK;
}

/**
 * @brief 等待 UART 服务任务收到事件
 * @param dev        UART 句柄
 * @param timeout_ms 最大等待时间，单位毫秒
 * @retval DEV_UART_OK: 收到通知
 * @retval DEV_UART_ERR_PARAM: 句柄非法
 * @retval DEV_UART_ERR_STATE: UART 尚未初始化
 * @retval DEV_UART_ERR_TIMEOUT: 等待超时
 */
dev_uart_status_t Dev_UART_Wait(const dev_uart_t *dev, uint32_t timeout_ms)
{
    if ((dev == NULL) || (dev->rt == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }

    if (dev->rt->started == 0U)
    {
        return DEV_UART_ERR_STATE;
    }

    return (Dev_UART_NotifyWait(timeout_ms) != 0U) ?
           DEV_UART_OK : DEV_UART_ERR_TIMEOUT;
}

/**
 * @brief 查询 UART 是否就绪
 * @param dev UART 句柄
 * @retval 1: 设备层与芯片层接收都已就绪
 * @retval 0: 句柄无效、尚未初始化或接收已被错误停止
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt == NULL))
    {
        return 0U;
    }

    if ((dev->rt->started == 0U) || (dev->rt->ready == 0U))
    {
        return 0U;
    }

    return BSP_UART_IsReady(dev->bsp);
}

/**
 * @brief 查询接收 FIFO 可读字节数，不触发硬件轮询
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 未消费字节数；句柄无效时为 0
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev)
{
    uint32_t state;
    size_t len;

    if ((dev == NULL) || (dev->rt == NULL))
    {
        return 0U;
    }

    state = Dev_UART_CriticalEnter();
    len = ringbuffer_data_len(&dev->rt->rx_fifo);
    Dev_UART_CriticalExit(state);

    return (uint16_t)len;
}

/**
 * @brief 查看接收 FIFO 队首连续数据，不消费数据
 * @param dev  UART 句柄
 * @param data 输出队首地址；无数据或参数非法时写入 NULL
 * @retval 0 至 FIFO 容量 连续可读字节数；参数非法时为 0
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, const uint8_t **data)
{
    uint32_t state;
    size_t len;
    uint8_t *ptr = NULL;

    if (data != NULL)
    {
        *data = NULL;
    }

    if ((dev == NULL) || (dev->rt == NULL))
    {
        return 0U;
    }

    state = Dev_UART_CriticalEnter();
    len = ringbuffer_peek(&dev->rt->rx_fifo, &ptr);
    Dev_UART_CriticalExit(state);

    if (data != NULL)
    {
        *data = ptr;
    }

    return (uint16_t)len;
}

/**
 * @brief 消费接收 FIFO 队首字节
 * @param dev UART 句柄
 * @param len 请求消费数量
 * @retval 0 至请求长度 实际消费数量；句柄无效时为 0
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint16_t len)
{
    uint32_t state;
    size_t used;

    if ((dev == NULL) || (dev->rt == NULL) || (len == 0U))
    {
        return 0U;
    }

    state = Dev_UART_CriticalEnter();
    used = ringbuffer_consume(&dev->rt->rx_fifo, (size_t)len);
    Dev_UART_CriticalExit(state);

    return (uint16_t)used;
}

/**
 * @brief 将完整数据帧串行化写入发送 FIFO
 * @param dev        UART 句柄
 * @param data       数据起点
 * @param len        数据长度；必须不超过发送 FIFO 容量
 * @param timeout_ms FIFO 空间等待上限，单位毫秒；0 表示只尝试一次
 * @retval DEV_UART_OK: 全部写入，发送已推进
 * @retval DEV_UART_ERR_PARAM: 参数非法或单帧超过 FIFO 容量
 * @retval DEV_UART_ERR_STATE: UART 未就绪
 * @retval DEV_UART_ERR_TIMEOUT: 空间、互斥量或期限等待超时
 * @retval DEV_UART_ERR_HAL: 芯片层启动发送失败；完整帧仍保留在队列中
 */
dev_uart_status_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *data,
                                 uint16_t len, uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    dev_uart_status_t st;
    uint32_t state;
    uint32_t start;
    uint32_t space;

    if ((dev == NULL) || (dev->rt == NULL) ||
        (data == NULL) || (len == 0U))
    {
        return DEV_UART_ERR_PARAM;
    }

    rt = dev->rt;
    if (rt->started == 0U)
    {
        return DEV_UART_ERR_STATE;
    }

    if (len > dev->tx_size)
    {
        return DEV_UART_ERR_PARAM;
    }

    if (Dev_UART_Lock(timeout_ms) == 0U)
    {
        return DEV_UART_ERR_TIMEOUT;
    }

    start = Dev_UART_Ticks();
    for (;;)
    {
        state = Dev_UART_CriticalEnter();
        space = (uint32_t)ringbuffer_space_len(&rt->tx_fifo);
        Dev_UART_CriticalExit(state);

        if (space >= (uint32_t)len)
        {
            break;
        }

        if (Dev_UART_Expired(start, timeout_ms) != 0U)
        {
            Dev_UART_Unlock();
            return DEV_UART_ERR_TIMEOUT;
        }

        Dev_UART_Delay(1U);
    }

    state = Dev_UART_CriticalEnter();
    (void)ringbuffer_put(&rt->tx_fifo, data, len);
    Dev_UART_CriticalExit(state);

    st = Dev_UART_TxPump(dev);
    Dev_UART_Unlock();

    return st;
}

/**
 * @brief 等待发送 FIFO 和硬件发送状态全部空闲
 * @param dev        UART 句柄
 * @param timeout_ms 最大等待时间，单位毫秒；0 表示只查询一次
 * @retval DEV_UART_OK: 当前数据全部发送完成
 * @retval DEV_UART_ERR_PARAM: 句柄非法或互斥量尚未创建
 * @retval DEV_UART_ERR_STATE: UART 未就绪
 * @retval DEV_UART_ERR_TIMEOUT: 等待互斥量、发送完成或期限超时
 * @retval DEV_UART_ERR_HAL: 芯片层发送启动失败；队列数据保留以便后续重试
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms)
{
    dev_uart_rt_t *rt;
    dev_uart_status_t st;
    uint32_t state;
    uint32_t start;
    uint8_t pending;

    if ((dev == NULL) || (dev->rt == NULL))
    {
        return DEV_UART_ERR_PARAM;
    }

    rt = dev->rt;
    if (rt->started == 0U)
    {
        return DEV_UART_ERR_STATE;
    }

    if (Dev_UART_Lock(timeout_ms) == 0U)
    {
        return DEV_UART_ERR_TIMEOUT;
    }

    start = Dev_UART_Ticks();
    for (;;)
    {
        st = Dev_UART_TxPump(dev);
        if (st != DEV_UART_OK)
        {
            Dev_UART_Unlock();
            return st;
        }

        state = Dev_UART_CriticalEnter();
        pending = (ringbuffer_data_len(&rt->tx_fifo) != 0U) ? 1U : 0U;
        Dev_UART_CriticalExit(state);

        if ((pending == 0U) && (BSP_UART_TxIdle(dev->bsp) != 0U))
        {
            break;
        }

        if (Dev_UART_Expired(start, timeout_ms) != 0U)
        {
            Dev_UART_Unlock();
            return DEV_UART_ERR_TIMEOUT;
        }

        Dev_UART_Delay(1U);
    }

    Dev_UART_Unlock();
    return DEV_UART_OK;
}

/**
 * @brief 查询发送 FIFO 剩余空间
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 可写字节数；句柄无效时为 0
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev)
{
    uint32_t state;
    size_t space;

    if ((dev == NULL) || (dev->rt == NULL))
    {
        return 0U;
    }

    state = Dev_UART_CriticalEnter();
    space = ringbuffer_space_len(&dev->rt->tx_fifo);
    Dev_UART_CriticalExit(state);

    return (uint16_t)space;
}

/**
 * @brief 查询 DMA 覆盖与接收 FIFO 丢弃的累计字节数
 * @param dev UART 句柄
 * @retval 0 至 UINT32_MAX 累计丢失字节数；句柄无效时为 0
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev)
{
    if ((dev == NULL) || (dev->rt == NULL))
    {
        return 0U;
    }

    return dev->rt->rx_lost;
}

/**
 * @brief 清零 DMA 覆盖与接收 FIFO 丢弃计数
 * @param dev UART 句柄
 * @retval 无
 */
void Dev_UART_RxClearOverflow(const dev_uart_t *dev)
{
    uint32_t state;

    if ((dev == NULL) || (dev->rt == NULL))
    {
        return;
    }

    state = Dev_UART_CriticalEnter();
    dev->rt->rx_lost = 0U;
    Dev_UART_CriticalExit(state);
}
