/**
 * @file bsp_uart.c
 * @brief UART 硬件驱动层：DMA Circular 乒乓接收 + 环形 FIFO，发送队列逐段 DMA
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 接收链路（三级，每级只做一件事）：
 *         USARTx RX ──DMA Circular──> s_rx_dma[256]（半区 A / 半区 B）
 *                                        │
 *                     HT / TC / IDLE 中断 │ 只做"把稳定段拷进 FIFO"
 *                                        ▼
 *                                   s_rx_fifo[512]（环形，单生产单消费）
 *                                        │
 *                             任务上下文 │ BSP_UART_Read()
 *                                        ▼
 *                                    上层协议解析
 *
 *       为什么不是"零拷贝直接读 DMA 缓冲"：DMA 会在半区边界回卷覆写，上层
 *       读的速度一旦慢于到达速度就会读到撕裂数据，且无法察觉。多一次 memcpy
 *       （每次只搬半个半区，微秒级）换来确定性。
 *
 *       为什么发布位置用"绝对位置 + 回卷判断"而不是回调给的 Size 参数：
 *       HAL 的 HT 回调传 Size/2、TC 传 Size、IDLE 传 Size - NDTR，三者含义
 *       不一致；统一换算成"当前写指针位置"后再发布，只有一套边界逻辑。
 *
 *       OS 对象归属：每口一把互斥量，由 .ioc 配置、freertos.c 生成，
 *       本文件只 extern。BSP_UART_Init() 必须放 USER CODE BEGIN RTOS_MUTEX，
 *       那里句柄才建好；HAL 回调是强符号，靠这次调用把本目标拉进镜像。
 *
 *       并发契约：
 *         接收  单生产者（DMA/ISR 写 head）单消费者（一个任务写 tail），无锁；
 *         发送  多生产者（任意任务写 head，本口自己的 LOCK 排队）单消费者
 *               （ISR 写 tail），"检查 busy + 启动 DMA"用 PRIMASK 临界区保护。
 *       RX DMA 流与 UART 中断必须同优先级：IDLE 走 USARTx_IRQn，
 *       HT/TC 走 DMA 流中断，同级才不会互相打断发布位置的计算。
 *
 *       所有权：各口 HAL 句柄与两条 DMA 句柄由本模块独占，上层不要再直接调
 *       HAL_UART_Transmit_DMA() 之类的接口，否则 tx_busy 与实际状态脱节，
 *       后续 Write 会一直判定"正在发送"。
 *
 *       实例：每路一套缓冲、一把锁，互不排队。上层不取数据时接收 FIFO
 *       只进不出，超容量后按设计丢弃并累加 rx_overflow——这是"接口先留好、
 *       数据源后接"的正常状态，不是故障。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "bsp_uart.h"
// 功能包含
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

/*==============================================================================
 * 外部变量声明
 *============================================================================*/
/** @brief 每口一把互斥量，用于发送队列的生产者排队
 *  @note 由 .ioc 生成、freertos.c 定义；一个口的发送排队不会挡住另一个口。 */
extern osMutexId_t UART1_LOCKHandle;
extern osMutexId_t UART2_LOCKHandle;

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 等锁的默认超时（ms）：拿不到锁说明有任务正卡在发送路径上 */
#define BSP_UART_LOCK_TIMEOUT_MS    100U

/** @brief Flush 的默认超时（ms），调用者传 0 时使用 */
#define BSP_UART_FLUSH_TIMEOUT_MS   100U

/** @brief USART1 接收 DMA 乒乓缓冲：整块被 DMA 循环写，两个半区各半
 *  @note 半区时间 = 半区字节数 × 10 / 波特率；数值必须是 2 的倍数、不小于 16。 */
#define BSP_UART1_RX_DMA_SIZE       256U

/** @brief USART1 接收 FIFO 容量：必须是 2 的幂，可用字节数为容量减一
 *  @note 可吸收的突发时长 = 可用字节数 × 10 / 波特率。 */
#define BSP_UART1_RX_FIFO_SIZE      512U

/** @brief USART1 发送 FIFO 容量：必须是 2 的幂，可用字节数为容量减一 */
#define BSP_UART1_TX_FIFO_SIZE      512U

/** @brief USART2 接收 DMA 乒乓缓冲，规格与 USART1 相同 */
#define BSP_UART2_RX_DMA_SIZE       256U

/** @brief USART2 接收 FIFO 容量 */
#define BSP_UART2_RX_FIFO_SIZE      512U

/** @brief USART2 发送 FIFO 容量 */
#define BSP_UART2_TX_FIFO_SIZE      512U

/** @brief 实例表长度，由 s_handles[] 的行数决定 */
#define BSP_UART_COUNT              (sizeof(s_handles) / sizeof(s_handles[0]))

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief UART 句柄（布局只在 .c 里出现）
 * @note 只放配置，不放运行时状态：句柄是 const，进 .rodata 不占 RAM；
 *       索引与计数放 s_rt[]，两者按下标一一对应。
 *       hal 用 void *，避免头文件引入 HAL；lock 存句柄变量的地址，
 *       句柄值要等 freertos.c 运行时才产生。
 */
struct bsp_uart {
    void             *hal;          /* HAL UART 句柄（UART_HandleTypeDef *） */
    osMutexId_t      *lock;         /* 指向 freertos.c 生成的互斥量句柄 */
    uint8_t          *rx_dma;       /* DMA 循环写缓冲，长度 rx_dma_size */
    uint8_t          *rx_fifo;      /* 接收环形 FIFO，长度 rx_fifo_size */
    uint8_t          *tx_fifo;      /* 发送环形 FIFO，长度 tx_fifo_size */
    uint16_t          rx_dma_size;  /* 必须是 2 的倍数 */
    uint16_t          rx_fifo_size; /* 必须是 2 的幂 */
    uint16_t          tx_fifo_size; /* 必须是 2 的幂 */
};

/**
 * @brief 一路 UART 的运行时状态
 * @note 句柄是 const，可写字段只能落在本结构里；下标与 s_handles[] 对齐。
 */
typedef struct {
    const bsp_uart_t *cfg;        /* 对应的配置句柄，Init 时绑定 */
    volatile uint16_t rx_pub;     /* 已发布进 FIFO 的 DMA 位置，0..rx_dma_size */
    volatile uint16_t rx_head;    /* 接收 FIFO 写索引，仅 ISR 写 */
    volatile uint16_t rx_tail;    /* 接收 FIFO 读索引，仅消费任务写 */
    volatile uint16_t tx_head;    /* 发送 FIFO 写索引，仅生产者写（持锁） */
    volatile uint16_t tx_tail;    /* 发送 FIFO 读索引，仅 ISR 写 */
    volatile uint16_t tx_len;     /* 当前这段 DMA 的长度，ISR 用它推进 tail */
    volatile uint32_t rx_overflow;/* FIFO 满丢弃的字节数 */
    volatile uint8_t  tx_busy;    /* 1: 发送 DMA 正在跑 */
    volatile uint8_t  ready;      /* 1: 初始化完成且接收在跑 */
} bsp_uart_rt_t;

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief USART1 接收 DMA 乒乓缓冲，4 字节对齐便于 DMA 突发访问 */
static uint8_t s_uart1_rx_dma[BSP_UART1_RX_DMA_SIZE] __attribute__((aligned(4)));

/** @brief USART1 接收环形 FIFO，容量 512，可用 511 字节 */
static uint8_t s_uart1_rx_fifo[BSP_UART1_RX_FIFO_SIZE];

/** @brief USART1 发送环形 FIFO，容量 512，可用 511 字节 */
static uint8_t s_uart1_tx_fifo[BSP_UART1_TX_FIFO_SIZE];

/** @brief USART2 接收 DMA 乒乓缓冲 */
static uint8_t s_uart2_rx_dma[BSP_UART2_RX_DMA_SIZE] __attribute__((aligned(4)));

/** @brief USART2 接收环形 FIFO */
static uint8_t s_uart2_rx_fifo[BSP_UART2_RX_FIFO_SIZE];

/** @brief USART2 发送环形 FIFO */
static uint8_t s_uart2_tx_fifo[BSP_UART2_TX_FIFO_SIZE];

/** @brief USART1 配置句柄；hal / lock 都指向 CubeMX 生成的句柄 */
const bsp_uart_t bsp_uart1 = {
    .hal          = &huart1,
    .lock         = &UART1_LOCKHandle,
    .rx_dma       = s_uart1_rx_dma,
    .rx_fifo      = s_uart1_rx_fifo,
    .tx_fifo      = s_uart1_tx_fifo,
    .rx_dma_size  = BSP_UART1_RX_DMA_SIZE,
    .rx_fifo_size = BSP_UART1_RX_FIFO_SIZE,
    .tx_fifo_size = BSP_UART1_TX_FIFO_SIZE,
};

/** @brief USART2 配置句柄；与 USART1 各持一把锁，两口互不排队 */
const bsp_uart_t bsp_uart2 = {
    .hal          = &huart2,
    .lock         = &UART2_LOCKHandle,
    .rx_dma       = s_uart2_rx_dma,
    .rx_fifo      = s_uart2_rx_fifo,
    .tx_fifo      = s_uart2_tx_fifo,
    .rx_dma_size  = BSP_UART2_RX_DMA_SIZE,
    .rx_fifo_size = BSP_UART2_RX_FIFO_SIZE,
    .tx_fifo_size = BSP_UART2_TX_FIFO_SIZE,
};

/** @brief 驱动管理的句柄表：Init 与中断反查都按它轮询 */
static const bsp_uart_t *const s_handles[] = {
    &bsp_uart1,
    &bsp_uart2,
};

/** @brief 运行时状态表，下标与 s_handles[] 严格一一对应 */
static bsp_uart_rt_t s_rt[BSP_UART_COUNT];

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 按公共句柄找到对应的运行时状态
 * @param uart UART 句柄，可为 NULL
 * @retval 对应状态块；句柄不在表中返回 NULL
 * @note 只做指针比对，不判断是否已初始化。
 */
static bsp_uart_rt_t *BSP_UART_Find(const bsp_uart_t *uart)
{
    uint32_t i;

    if (uart == NULL) {
        return NULL;
    }
    for (i = 0U; i < BSP_UART_COUNT; i++) {
        if (s_handles[i] == uart) {
            return &s_rt[i];
        }
    }
    return NULL;
}

/**
 * @brief 按 HAL 句柄找到对应的运行时状态（中断入口用）
 * @param hal HAL UART 句柄
 * @retval 对应状态块；未在表中或尚未 Init 返回 NULL
 * @note 中断里调用，只做指针比对，不做任何 HAL 调用。
 */
static bsp_uart_rt_t *BSP_UART_Find_By_Hal(const void *hal)
{
    uint32_t i;

    for (i = 0U; i < BSP_UART_COUNT; i++) {
        if ((s_rt[i].cfg != NULL) && (s_rt[i].cfg->hal == hal)) {
            return &s_rt[i];
        }
    }
    return NULL;
}

/**
 * @brief 把一段字节压进接收 FIFO，空间不足时丢弃尾部并记账
 * @param rt  运行时状态，不可为 NULL
 * @param src 源数据，不可为 NULL
 * @param len 字节数，不能为 0
 * @retval 无
 * @note 仅由中断调用（单生产者）。刻意留一个字节空间用于区分空/满：
 *       head == tail 为空，head + 1 == tail 为满，所以可用容量是 size-1。
 */
static void BSP_UART_Rx_Push(bsp_uart_rt_t *rt, const uint8_t *src, uint16_t len)
{
    const bsp_uart_t *cfg = rt->cfg;
    uint16_t mask = (uint16_t)(cfg->rx_fifo_size - 1U);
    uint16_t head = rt->rx_head;
    uint16_t used = (uint16_t)((head - rt->rx_tail) & mask);
    uint16_t space = (uint16_t)(mask - used);
    uint16_t first;

    if (len > space) {
        rt->rx_overflow += (uint32_t)(len - space);
        len = space;
    }
    if (len == 0U) {
        return;
    }

    /* 跨过 FIFO 末尾就分两段：写索引起到末尾，再从头写剩下 */
    first = (uint16_t)(cfg->rx_fifo_size - head);
    if (first > len) {
        first = len;
    }
    (void)memcpy(&cfg->rx_fifo[head], src, (size_t)first);
    if (len > first) {
        (void)memcpy(&cfg->rx_fifo[0], &src[first], (size_t)(len - first));
    }

    rt->rx_head = (uint16_t)((head + len) & mask);
}

/**
 * @brief 把 DMA 缓冲中"已稳定"的一段发布进 FIFO
 * @param rt  运行时状态，不可为 NULL
 * @param pos 当前 DMA 写指针位置：0..rx_dma_size，等于 rx_dma_size 表示
 *            整圈刚回卷（TC 事件）
 * @retval 无
 * @note 发布区间永远是从 rx_pub 到 pos 的正向距离，跨回卷时拆成两段。
 *       DMA 在半区边界必然触发 HT 或 TC，所以未发布量不会超过半圈；
 *       真出现超过半圈说明丢了中断，按半圈截断并记溢出，不让脏区间扩散。
 */
static void BSP_UART_Rx_Publish(bsp_uart_rt_t *rt, uint16_t pos)
{
    const bsp_uart_t *cfg = rt->cfg;
    uint16_t size = cfg->rx_dma_size;
    uint16_t pub = rt->rx_pub;

    /* ---------- 1. TC：整圈写完，pos 归一到 0，发布 pub..size ---------- */
    if (pos >= size) {
        /* pub 已经是 0 说明这一圈早就发布过（TC 与 IDLE 撞在同一圈末尾），
           再发一次会把旧数据重复压进 FIFO，比丢数据更难查 */
        if ((pub != 0U) && (size > pub)) {
            BSP_UART_Rx_Push(rt, &cfg->rx_dma[pub], (uint16_t)(size - pub));
        }
        rt->rx_pub = 0U;
        return;
    }

    /* ---------- 2. 普通事件：算正向距离，回卷拆两段 ---------- */
    if (pos >= pub) {
        if (pos > pub) {
            BSP_UART_Rx_Push(rt, &cfg->rx_dma[pub], (uint16_t)(pos - pub));
        }
    } else {
        BSP_UART_Rx_Push(rt, &cfg->rx_dma[pub], (uint16_t)(size - pub));
        if (pos > 0U) {
            BSP_UART_Rx_Push(rt, &cfg->rx_dma[0], pos);
        }
    }
    rt->rx_pub = pos;
}

/**
 * @brief 启动（或重启）接收：复位发布位置并武装 DMA + IDLE 中断
 * @param rt 运行时状态，不可为 NULL
 * @retval BSP_UART_OK: 已启动
 * @retval BSP_UART_ERR_HAL: HAL 拒绝启动（外设忙或 DMA 通道异常）
 * @note 只有在 RxState 为 READY 时才会成功；错误回调里 HAL 已把状态复位，
 *       可以直接调用。重启会把 rx_pub 归零，因为 DMA 从缓冲起点重新写。
 */
static bsp_uart_status_t BSP_UART_Rx_Start(bsp_uart_rt_t *rt)
{
    const bsp_uart_t *cfg = rt->cfg;

    rt->rx_pub = 0U;
    if (HAL_UARTEx_ReceiveToIdle_DMA((UART_HandleTypeDef *)cfg->hal,
                                     cfg->rx_dma,
                                     cfg->rx_dma_size) != HAL_OK) {
        return BSP_UART_ERR_HAL;
    }
    return BSP_UART_OK;
}

/**
 * @brief 若发送 DMA 空闲且队列有数据，启动下一段传输
 * @param rt 运行时状态，不可为 NULL
 * @retval 无
 * @note 任务与中断都会调用，用 PRIMASK 保存/恢复包住"判断 + 置位 + 启动"，
 *       避免任务刚判定 busy 为 0、中断就插进来又启动一次的竞态。
 *       临界区内只做寄存器配置，不等待硬件；关中断时长约 2 µs。
 *       环形 FIFO 回卷处不连续，所以一段最多发到缓冲末尾，剩下的由 TC
 *       回调接续。
 */
static void BSP_UART_Tx_Kick(bsp_uart_rt_t *rt)
{
    const bsp_uart_t *cfg = rt->cfg;
    uint16_t tail;
    uint16_t head;
    uint16_t len;
    uint32_t primask;

    primask = __get_PRIMASK();
    __disable_irq();

    if ((rt->tx_busy != 0U) || (rt->tx_head == rt->tx_tail)) {
        __set_PRIMASK(primask);
        return;
    }

    tail = rt->tx_tail;
    head = rt->tx_head;
    len = (head > tail) ? (uint16_t)(head - tail)
                        : (uint16_t)(cfg->tx_fifo_size - tail);

    rt->tx_busy = 1U;
    rt->tx_len = len;
    if (HAL_UART_Transmit_DMA((UART_HandleTypeDef *)cfg->hal,
                              &cfg->tx_fifo[tail], len) != HAL_OK) {
        /* 启动失败只回滚标志，数据仍在队列里，下次 Write/Kick 会重试 */
        rt->tx_busy = 0U;
    }

    __set_PRIMASK(primask);
}

/**
 * @brief 从接收 FIFO 头部拷出数据，可选是否推进读索引
 * @param rt      运行时状态，不可为 NULL
 * @param buf     目的缓冲区，不可为 NULL
 * @param len     最多拷多少字节，不能为 0
 * @param consume 1: 真读走（推进 tail）；0: 只窥视（tail 不动）
 * @retval 实际拷出的字节数，0 表示 FIFO 为空
 * @note 单消费者语义。consume 为 0 时数据仍留在 FIFO 里：环形缓冲只有在
 *       tail 推进后才释放空间，所以"窥视看到的字节"在下次 Read 前一定还在，
 *       前提是同一个任务连续调用、不违反单消费者约定。
 */
static uint16_t BSP_UART_Copy_Out(bsp_uart_rt_t *rt, uint8_t *buf,
                                  uint16_t len, uint8_t consume)
{
    const bsp_uart_t *cfg = rt->cfg;
    uint16_t mask = (uint16_t)(cfg->rx_fifo_size - 1U);
    uint16_t tail = rt->rx_tail;
    uint16_t used = (uint16_t)((rt->rx_head - tail) & mask);
    uint16_t first;

    if (len > used) {
        len = used;
    }
    if (len == 0U) {
        return 0U;
    }

    /* ---------- 跨过 FIFO 末尾就分两段拷 ---------- */
    first = (uint16_t)(cfg->rx_fifo_size - tail);
    if (first > len) {
        first = len;
    }
    (void)memcpy(buf, &cfg->rx_fifo[tail], (size_t)first);
    if (len > first) {
        (void)memcpy(&buf[first], &cfg->rx_fifo[0], (size_t)(len - first));
    }

    if (consume != 0U) {
        rt->rx_tail = (uint16_t)((tail + len) & mask);
    }
    return len;
}

/*==============================================================================
 * 接口实现：初始化与查询
 *============================================================================*/
/**
 * @brief 初始化 UART 驱动并启动接收 DMA
 * @retval BSP_UART_OK: 全部实例就绪（已就绪的实例会被跳过）
 * @retval BSP_UART_ERR_STATE: 缺 HAL 句柄、互斥量尚未创建、UART 不在 READY，
 *         或收发 DMA 通道未配置
 * @retval BSP_UART_ERR_HAL: 接收 DMA 启动失败
 * @note 必须在 USER CODE BEGIN RTOS_MUTEX 内调用（CubeMX 刚建完各口互斥量），
 *       且 MX_USARTx_UART_Init() 已执行过。不能在中断里调用。
 *       若 .ioc 把接收 DMA 配成 Normal，这里会改成 Circular 并重新
 *       HAL_DMA_Init()——乒乓模型依赖硬件循环写，Normal 模式只收一圈就停。
 */
bsp_uart_status_t BSP_UART_Init(void)
{
    uint32_t i;

    for (i = 0U; i < BSP_UART_COUNT; i++) {
        const bsp_uart_t *cfg = s_handles[i];
        bsp_uart_rt_t *rt = &s_rt[i];
        UART_HandleTypeDef *hal;

        if (rt->ready != 0U) {
            continue;
        }
        if ((cfg == NULL) || (cfg->hal == NULL) || (cfg->lock == NULL)) {
            return BSP_UART_ERR_STATE;
        }
        if (*cfg->lock == NULL) {
            return BSP_UART_ERR_STATE;   /* 本口的 LOCK 还没被 osMutexNew 建出来 */
        }

        hal = (UART_HandleTypeDef *)cfg->hal;
        if ((hal->hdmarx == NULL) || (hal->hdmatx == NULL)) {
            return BSP_UART_ERR_STATE;   /* 收发都要求 DMA，缺一路就整体拒收 */
        }
        if (hal->RxState != HAL_UART_STATE_READY) {
            return BSP_UART_ERR_STATE;
        }

        /* ---------- 1. 接收 DMA 必须是循环模式，否则乒乓只有一圈 ---------- */
        if (hal->hdmarx->Init.Mode != DMA_CIRCULAR) {
            hal->hdmarx->Init.Mode = DMA_CIRCULAR;
            if (HAL_DMA_Init(hal->hdmarx) != HAL_OK) {
                return BSP_UART_ERR_HAL;
            }
        }

        /* ---------- 2. 清运行态并把句柄绑上，回调靠它反查 ---------- */
        rt->cfg         = cfg;
        rt->rx_pub      = 0U;
        rt->rx_head     = 0U;
        rt->rx_tail     = 0U;
        rt->tx_head     = 0U;
        rt->tx_tail     = 0U;
        rt->tx_len      = 0U;
        rt->rx_overflow = 0U;
        rt->tx_busy     = 0U;

        /* ---------- 3. 武装接收，之后数据全靠中断推进 ---------- */
        if (BSP_UART_Rx_Start(rt) != BSP_UART_OK) {
            rt->cfg = NULL;
            return BSP_UART_ERR_HAL;
        }
        rt->ready = 1U;
    }

    return BSP_UART_OK;
}

/**
 * @brief 查询驱动是否已就绪
 * @param uart UART 句柄，不可为 NULL
 * @retval 1: 已初始化且接收 DMA 在跑
 * @retval 0: 未初始化、接收已停或句柄非法
 */
uint8_t BSP_UART_Is_Ready(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);

    return ((rt != NULL) && (rt->ready != 0U)) ? 1U : 0U;
}

/**
 * @brief 查询接收 FIFO 中已收未读的字节数
 * @param uart UART 句柄，不可为 NULL
 * @retval 可读字节数，最大为 FIFO 容量减一
 * @note 单消费者语义：只应由一个任务调用 Read/Available，多任务同时读会撕裂。
 *       接收因错误停掉后仍可读出 FIFO 里的残留数据，此时 Is_Ready() 返回 0。
 */
uint16_t BSP_UART_Available(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    uint16_t head;
    uint16_t tail;

    if ((rt == NULL) || (rt->cfg == NULL)) {
        return 0U;
    }
    head = rt->rx_head;   /* 先取 head 再取 tail：并发时只会少算，不会多算 */
    tail = rt->rx_tail;
    return (uint16_t)((head - tail) & (uint16_t)(rt->cfg->rx_fifo_size - 1U));
}

/**
 * @brief 窥视接收 FIFO 头部的数据，不取走
 * @param uart UART 句柄，不可为 NULL
 * @param buf  接收缓冲区，不可为 NULL
 * @param len  最多看多少字节，不能为 0
 * @retval 实际看到的字节数，0 表示 FIFO 为空
 * @note 只读不消费：tail 不动，数据仍留在 FIFO 里，后续 Read() 还能取到。
 *       典型用法是"先看头部拿到长度字段，整包到齐了再一次性 Read 走"，
 *       这样半包不会因为一次试探性读取而丢失。
 *       单消费者语义见 BSP_UART_Available()。
 */
uint16_t BSP_UART_Peek(const bsp_uart_t *uart, uint8_t *buf, uint16_t len)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);

    if ((rt == NULL) || (rt->cfg == NULL) || (buf == NULL) || (len == 0U)) {
        return 0U;
    }
    return BSP_UART_Copy_Out(rt, buf, len, 0U);
}

/**
 * @brief 从接收 FIFO 取数据，非阻塞
 * @param uart UART 句柄，不可为 NULL
 * @param buf  接收缓冲区，不可为 NULL
 * @param len  最多取多少字节，不能为 0
 * @retval 实际取到的字节数，0 表示当前没有数据
 * @note 取不满不会等待；要"凑够 len 字节再返回"由上层循环加超时自己做。
 *       会推进读索引：取走的字节 BSP 不再保留，所以"先探性读一段再判断
 *       包是否完整"必须改用 BSP_UART_Peek()，否则半包会被消耗掉。
 *       单消费者语义见 BSP_UART_Available()。
 */
uint16_t BSP_UART_Read(const bsp_uart_t *uart, uint8_t *buf, uint16_t len)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);

    if ((rt == NULL) || (rt->cfg == NULL) || (buf == NULL) || (len == 0U)) {
        return 0U;
    }
    return BSP_UART_Copy_Out(rt, buf, len, 1U);
}

/**
 * @brief 读取接收溢出计数
 * @param uart UART 句柄，不可为 NULL
 * @retval 上电以来因 FIFO 满而丢弃的字节数
 * @note 累加值，不回绕判断；上层可周期性比对增量来判断"是不是读得太慢"。
 */
uint32_t BSP_UART_Rx_Overflow(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);

    return (rt != NULL) ? rt->rx_overflow : 0U;
}

/**
 * @brief 清零接收溢出计数
 * @param uart UART 句柄，不可为 NULL
 * @retval 无
 */
void BSP_UART_Rx_Clear_Overflow(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);

    if (rt != NULL) {
        rt->rx_overflow = 0U;
    }
}

/*==============================================================================
 * 接口实现：发送
 *============================================================================*/
/**
 * @brief 查询发送 FIFO 剩余可写空间
 * @param uart UART 句柄，不可为 NULL
 * @retval 还能排入多少字节
 * @note 并发下数值立刻会变，只适合判断"大概够不够"；要精确就用 Write 的返回值。
 */
uint16_t BSP_UART_Tx_Free(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    uint16_t mask;
    uint16_t head;
    uint16_t tail;

    if ((rt == NULL) || (rt->ready == 0U)) {
        return 0U;
    }
    mask = (uint16_t)(rt->cfg->tx_fifo_size - 1U);
    head = rt->tx_head;
    tail = rt->tx_tail;
    return (uint16_t)(mask - ((head - tail) & mask));
}

/**
 * @brief 把数据排入发送队列，立即返回
 * @param uart UART 句柄，不可为 NULL
 * @param buf  待发送数据，不可为 NULL
 * @param len  请求发送的字节数，不能为 0
 * @retval 实际排入队列的字节数，小于 len 说明队列满了（剩余部分被丢弃）
 * @note 多任务可并发调用，内部用本口自己的 LOCK 排队，一次调用排入的 len 字节在
 *       队列里连续，不会与其他任务的数据交错——所以"一帧一次 Write"就是
 *       帧级原子的。会短暂阻塞等锁，不得在中断中调用。
 */
uint16_t BSP_UART_Write(const bsp_uart_t *uart, const uint8_t *buf, uint16_t len)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    const bsp_uart_t *cfg;
    uint16_t mask;
    uint16_t head;
    uint16_t used;
    uint16_t space;
    uint16_t first;

    if ((rt == NULL) || (rt->ready == 0U) || (buf == NULL) || (len == 0U)) {
        return 0U;
    }

    /* ---------- 1. 抢发送锁：生产者之间排队，防止 head 被并发改写 ---------- */
    if (osMutexAcquire(*uart->lock, pdMS_TO_TICKS(BSP_UART_LOCK_TIMEOUT_MS)) != osOK) {
        return 0U;
    }

    cfg = rt->cfg;
    mask = (uint16_t)(cfg->tx_fifo_size - 1U);
    head = rt->tx_head;
    used = (uint16_t)((head - rt->tx_tail) & mask);
    space = (uint16_t)(mask - used);
    if (len > space) {
        len = space;   /* 放不下的部分调用者自己重发，BSP 不做阻塞等待 */
    }

    /* ---------- 2. 入队：跨过 FIFO 末尾就分两段写 ---------- */
    if (len > 0U) {
        first = (uint16_t)(cfg->tx_fifo_size - head);
        if (first > len) {
            first = len;
        }
        (void)memcpy(&cfg->tx_fifo[head], buf, (size_t)first);
        if (len > first) {
            (void)memcpy(&cfg->tx_fifo[0], &buf[first], (size_t)(len - first));
        }
        rt->tx_head = (uint16_t)((head + len) & mask);
    }

    /* ---------- 3. DMA 若空闲就立刻发，否则等 TC 回调接续 ---------- */
    BSP_UART_Tx_Kick(rt);

    (void)osMutexRelease(*uart->lock);
    return len;
}

/**
 * @brief 等待发送队列排空且最后一个字节移出移位寄存器
 * @param uart       UART 句柄，不可为 NULL
 * @param timeout_ms 超时预算（毫秒），0 表示用默认 100 ms
 * @retval BSP_UART_OK: 已发完
 * @retval BSP_UART_ERR_PARAM: 句柄非法或驱动未就绪
 * @retval BSP_UART_ERR_TIMEOUT: 超时仍有数据未发完
 * @note 每 1 ms 让出一次 CPU，不要在中断里调用。发送前若刚 Write 完就
 *       Flush，返回时对端已收到最后一个字节的停止位。
 */
bsp_uart_status_t BSP_UART_Flush(const bsp_uart_t *uart, uint32_t timeout_ms)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    UART_HandleTypeDef *hal;
    uint32_t tickstart;

    if ((rt == NULL) || (rt->ready == 0U)) {
        return BSP_UART_ERR_PARAM;
    }
    if (timeout_ms == 0U) {
        timeout_ms = BSP_UART_FLUSH_TIMEOUT_MS;
    }

    hal = (UART_HandleTypeDef *)rt->cfg->hal;
    tickstart = HAL_GetTick();

    /* ---------- 队列空 + DMA 停 + 发送状态机回到 READY 才算真发完 ---------- */
    while ((rt->tx_head != rt->tx_tail) ||
           (rt->tx_busy != 0U) ||
           (hal->gState != HAL_UART_STATE_READY)) {
        if ((HAL_GetTick() - tickstart) > timeout_ms) {
            return BSP_UART_ERR_TIMEOUT;
        }
        (void)osDelay(1U);
    }
    return BSP_UART_OK;
}

/*==============================================================================
 * HAL 回调覆盖
 *
 * 接收用 HAL_UARTEx_ReceiveToIdle_DMA() 启动，HAL 把 HT / TC / IDLE 三种事件
 * 统一送到 HAL_UARTEx_RxEventCallback()，事件类型放在 huart->RxEventType。
 * 这里覆盖的是 HAL 的 __weak 回调，不是 USART1/2_IRQHandler 或 DMA 流中断
 * 本身；中断向量仍由 Src/stm32f4xx_it.c 分发。
 *============================================================================*/
/**
 * @brief 接收事件回调：把 HT / TC / IDLE 换算成同一个"写指针位置"再发布
 * @param huart HAL UART 句柄
 * @param Size  HAL 给出的事件位置，本文件不用（三种事件含义不一致），
 *              统一改用 RxEventType + DMA 计数器推导
 * @retval 无
 * @note 中断上下文，只做 memcpy 与索引更新，不调用任何 RTOS API。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    bsp_uart_rt_t *rt = BSP_UART_Find_By_Hal(huart);
    uint16_t pos;
    uint16_t dma_size;

    (void)Size;

    if ((rt == NULL) || (rt->ready == 0U) || (huart->hdmarx == NULL)) {
        return;
    }

    dma_size = rt->cfg->rx_dma_size;

    switch (huart->RxEventType) {
    case HAL_UART_RXEVENT_HT:
        pos = (uint16_t)(dma_size / 2U);       /* 前半区已稳定 */
        break;
    case HAL_UART_RXEVENT_TC:
        pos = dma_size;                        /* 整圈完成，发布后半区 */
        break;
    case HAL_UART_RXEVENT_IDLE:
    default:
        /* 半区内已收到的尾部：计数器剩余量反推当前写指针 */
        pos = (uint16_t)(dma_size - __HAL_DMA_GET_COUNTER(huart->hdmarx));
        break;
    }

    BSP_UART_Rx_Publish(rt, pos);
}

/**
 * @brief 发送完成回调：推进 tail，接着发环形缓冲里的下一段
 * @param huart HAL UART 句柄
 * @retval 无
 * @note 中断上下文。HAL 在调用本回调之前已把 gState 与 DMA 的 State 复位成
 *       READY，所以这里可以安全地再次 HAL_UART_Transmit_DMA()。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find_By_Hal(huart);

    if ((rt == NULL) || (rt->ready == 0U)) {
        return;
    }

    rt->tx_tail = (uint16_t)((rt->tx_tail + rt->tx_len) &
                             (uint16_t)(rt->cfg->tx_fifo_size - 1U));
    rt->tx_busy = 0U;
    BSP_UART_Tx_Kick(rt);
}

/**
 * @brief 错误回调：ORE / FE / NE 会终止 DMA 接收，这里原地重启
 * @param huart HAL UART 句柄
 * @retval 无
 * @note 上电时对端已在发、或线路干扰导致 ORE 是常态，HAL 的阻塞式错误处理
 *       会关掉 DMAR 并退出接收，不重启就永久收不到数据。
 *       重启前 rx_pub 归零（DMA 从缓冲起点重新写），已进 FIFO 的数据不受影响。
 *       重启失败时不重试——继续重试只会反复失败，上层可从溢出计数与
 *       BSP_UART_Is_Ready() 观察到异常。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find_By_Hal(huart);

    if ((rt == NULL) || (rt->ready == 0U)) {
        return;
    }

    if (BSP_UART_Rx_Start(rt) != BSP_UART_OK) {
        rt->ready = 0U;   /* 接收彻底停了，让查询接口能反映出来 */
    }
}
