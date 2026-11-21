/**
 * @file bsp_uart.c
 * @brief UART 硬件驱动层实现：两路能力探测、三档搬运、字节注入
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 数据流（本层不缓存，缓存全在设备层）：
 *
 *         USARTx RX ──DMA Circular──> rx_buf（本层暂存）
 *                          │
 *              HT / TC / IDLE│只算"新增了哪一段"，立刻交给 rx_push
 *                          ▼
 *                    设备层的接收队列
 *
 *         设备层发送队列 ──tx_fetch──> tx_stage ──DMA/中断──> USARTx TX
 *
 *       为什么本层不排队：队列容量、满时丢谁、溢出怎么记，都是"产品策略"，
 *       与芯片无关；放在本层会让裸机与 RTOS 两套实现各写一遍。本层只做
 *       "把字节从寄存器搬到内存、再从内存搬回寄存器"。
 *
 *       位置换算：HAL 的 HT 回调传 Size/2、TC 传 Size、IDLE 传 Size-NDTR，
 *       三者含义不一致；统一换算成"DMA 当前写指针位置"后再算增量，只有一套
 *       边界逻辑（见 BSP_UART_RxPublish）。
 *
 *       退档：启动失败时收/发各自向下退一档（DMA→中断→阻塞），只退不进；
 *       退档后本层不再尝试高档。设备层通过表现（吞吐、延迟）感知，不需要
 *       额外的状态查询接口。
 *
 *       并发：接收单生产者（中断写 rx_pub）单消费者（无，本层不存）；
 *       发送"检查 + 置位 + 启动"用 PRIMASK 临界区保护，临界区内不等待硬件。
 *       RX DMA 流与 UART 中断必须同优先级：IDLE 走 USARTx_IRQn，HT/TC 走
 *       DMA 流中断，同级才不会互相打断位置计算。
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

/** @brief USART1 接收 DMA 目标缓冲：整块被 DMA 循环写
 *  @note 这是硬件写入口，不是给上层用的队列；数值必须是 2 的倍数、不小于 16。
 *        半区时间 = 半区字节数 × 10 / 波特率。 */
#define BSP_UART1_RX_BUF_SIZE       256U

/** @brief USART1 发送暂存：一拍最多塞给外设这么多字节
 *  @note DMA 源必须是一块连续内存，所以先取到这块再启动传输。 */
#define BSP_UART1_TX_STAGE_SIZE     256U

/** @brief 实例表长度，由 s_handles[] 的行数决定 */
#define BSP_UART_COUNT              (sizeof(s_handles) / sizeof(s_handles[0]))

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 一路 UART 的运行块
 * @note 句柄是 const，可写字段只能落在本结构里；每个句柄实例各带一份。
 */
typedef struct {
    const bsp_uart_t *cfg;          /* 对应的配置句柄，Init 时绑定 */
    bsp_uart_hooks_t  hooks;        /* 设备层注册进来的收发钩子 */
    volatile uint16_t rx_pub;       /* 已交给上层的 DMA 位置，0..rx_buf_size */
    volatile uint16_t tx_len;       /* 已取进暂存区、还没发完的字节数 */
    volatile uint8_t  tx_busy;      /* 1: 发送已经交给外设 */
    volatile uint8_t  rx_byte;      /* 中断档逐字节接收的落点 */
    volatile uint8_t  ready;        /* 1: 已注册钩子且接收在跑 */
    volatile uint8_t  initialized;  /* 1: 已初始化并探测过能力 */
    uint8_t           tx_mode;      /* 发送生效档位 */
    uint8_t           rx_mode;      /* 接收生效档位 */
} bsp_uart_rt_t;

/**
 * @brief UART 句柄（布局只在 .c 里出现）
 * @note 只放配置与一个指向运行块的指针：可写数据全在运行块里，句柄是 const，
 *       进 .rodata 不占 RAM；每路 UART 各带一份运行块，由 rt 指向它。
 *       hal 用 void *，避免头文件引入 HAL。
 */
struct bsp_uart {
    void           *hal;           /* HAL UART 句柄（UART_HandleTypeDef *） */
    uint8_t        *rx_buf;        /* DMA 循环写缓冲，长度 rx_buf_size */
    uint8_t        *tx_stage;      /* 发送暂存，长度 tx_stage_size */
    uint16_t        rx_buf_size;   /* 必须是 2 的倍数 */
    uint16_t        tx_stage_size; /* 一拍最多多少字节 */
    bsp_uart_rt_t  *rt;            /* 本实例的运行块：每路 UART 各一份 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief USART1 接收 DMA 目标缓冲，4 字节对齐便于 DMA 突发访问 */
static uint8_t s_uart1_rx_buf[BSP_UART1_RX_BUF_SIZE] __attribute__((aligned(4)));

/** @brief USART1 发送暂存，一拍一段 */
static uint8_t s_uart1_tx_stage[BSP_UART1_TX_STAGE_SIZE];

/** @brief USART1 的运行块，与句柄实例成对定义 */
static bsp_uart_rt_t bsp_uart1_rt;

/** @brief USART1 配置句柄；hal 指向 CubeMX 生成的 HAL 句柄 */
const bsp_uart_t bsp_uart1 = {
    .hal           = &huart1,
    .rx_buf        = s_uart1_rx_buf,
    .tx_stage      = s_uart1_tx_stage,
    .rx_buf_size   = BSP_UART1_RX_BUF_SIZE,
    .tx_stage_size = BSP_UART1_TX_STAGE_SIZE,
    .rt            = &bsp_uart1_rt,
};

/** @brief 驱动管理的句柄表：Init 与中断反查都按它轮询 */
static const bsp_uart_t *const s_handles[] = {
    &bsp_uart1,
};

/** @brief 表里的 UART 路数，供循环比较 */
static const uint32_t s_uart_count = (uint32_t)BSP_UART_COUNT;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 按公共句柄找到对应的运行块
 * @param uart UART 句柄，可为 NULL
 * @retval 对应运行块；句柄未绑定运行块返回 NULL
 * @note 只做指针比对，不判断是否已初始化。
 */
static bsp_uart_rt_t *BSP_UART_Find(const bsp_uart_t *uart)
{
    if (uart == NULL) {
        return NULL;
    }
    return uart->rt;
}

/**
 * @brief 按 HAL 句柄找到对应的运行块（中断入口用）
 * @param hal HAL UART 句柄
 * @retval 对应运行块；未在表中或尚未 Init 返回 NULL
 * @note 中断里调用，只做指针比对，不做任何 HAL 调用。
 */
static bsp_uart_rt_t *BSP_UART_FindByHal(const void *hal)
{
    uint32_t i;

    for (i = 0U; i < s_uart_count; i++) {
        if ((s_handles[i]->rt != NULL) && (s_handles[i]->rt->cfg != NULL) &&
            (s_handles[i]->rt->cfg->hal == hal)) {
            return s_handles[i]->rt;
        }
    }
    return NULL;
}

/**
 * @brief 从 HAL 句柄探测发送能力
 * @param hal HAL UART 句柄，不可为 NULL
 * @retval BSP_UART_CAP_DMA: 发送 DMA 流已挂上
 * @retval BSP_UART_CAP_IT: 没有 DMA 流，退到中断
 * @note 阻塞档不需要额外硬件能力，退档时由搬运流程继续往下走。
 */
static uint8_t BSP_UART_ActualTx(const UART_HandleTypeDef *hal)
{
    return (hal->hdmatx != NULL) ? (uint8_t)BSP_UART_CAP_DMA
                                 : (uint8_t)BSP_UART_CAP_IT;
}

/**
 * @brief 从 HAL 句柄探测接收能力
 * @param hal HAL UART 句柄，不可为 NULL
 * @retval BSP_UART_CAP_DMA: 接收 DMA 流已挂上
 * @retval BSP_UART_CAP_IT: 没有 DMA 流，退到中断
 * @note 阻塞接收（轮询）不需要硬件中断能力，退档时继续往下走。
 */
static uint8_t BSP_UART_ActualRx(const UART_HandleTypeDef *hal)
{
    return (hal->hdmarx != NULL) ? (uint8_t)BSP_UART_CAP_DMA
                                 : (uint8_t)BSP_UART_CAP_IT;
}

/**
 * @brief 把一段字节交给设备层
 * @param rt   运行块，不可为 NULL
 * @param data 字节起始地址，不可为 NULL
 * @param len  字节数，0 表示没有新数据（直接返回）
 * @retval 无
 * @note 中断上下文调用；钩子未注册时什么也不做（正常流程里注册过才会 ready）。
 */
static void BSP_UART_RxGive(bsp_uart_rt_t *rt, const uint8_t *data, uint16_t len)
{
    if ((len == 0U) || (rt->hooks.rx_push == NULL)) {
        return;
    }
    rt->hooks.rx_push(rt->hooks.ctx, data, len);
}

/**
 * @brief 把 DMA 缓冲中"已稳定"的一段交给上层
 * @param rt  运行块，不可为 NULL
 * @param pos 当前 DMA 写指针位置：0..rx_buf_size，等于 rx_buf_size 表示
 *            整圈刚回卷（TC 事件）
 * @retval 无
 * @note 交付区间永远是从 rx_pub 到 pos 的正向距离，跨回卷时拆成两段。
 *       DMA 在半区边界必然触发 HT 或 TC，所以未交付量不会超过半圈；
 *       真出现超过半圈说明丢了中断，按半圈截断，不让脏区间扩散。
 */
static void BSP_UART_RxPublish(bsp_uart_rt_t *rt, uint16_t pos)
{
    const bsp_uart_t *cfg = rt->cfg;
    uint16_t size = cfg->rx_buf_size;
    uint16_t pub = rt->rx_pub;

    /* ---------- 1. TC：整圈写完，pos 归一到 0，交付 pub..size ---------- */
    if (pos >= size) {
        /* pub 已经是 0 说明这一圈早就交过（TC 与 IDLE 撞在同一圈末尾），
           再交一次会把旧数据重复送上去 */
        if ((pub != 0U) && (size > pub)) {
            BSP_UART_RxGive(rt, &cfg->rx_buf[pub], (uint16_t)(size - pub));
        }
        rt->rx_pub = 0U;
        return;
    }

    /* ---------- 2. 普通事件：算正向距离，回卷拆两段 ---------- */
    if (pos >= pub) {
        if (pos > pub) {
            BSP_UART_RxGive(rt, &cfg->rx_buf[pub], (uint16_t)(pos - pub));
        }
    } else {
        BSP_UART_RxGive(rt, &cfg->rx_buf[pub], (uint16_t)(size - pub));
        if (pos > 0U) {
            BSP_UART_RxGive(rt, &cfg->rx_buf[0], pos);
        }
    }
    rt->rx_pub = pos;
}

/**
 * @brief 按当前接收档位武装接收
 * @param rt 运行块，不可为 NULL
 * @retval BSP_UART_OK: 已武装
 * @retval BSP_UART_ERR_HAL: HAL 拒绝启动
 * @note DMA 档从缓冲起点重新开始，所以 rx_pub 归零；中断档逐字节进 rt->rx_byte；
 *       阻塞档不武装任何中断，由 BSP_UART_Poll() 主动取。
 */
static bsp_uart_status_t BSP_UART_RxArm(bsp_uart_rt_t *rt)
{
    const bsp_uart_t *cfg = rt->cfg;
    UART_HandleTypeDef *hal = (UART_HandleTypeDef *)cfg->hal;

    rt->rx_pub = 0U;

    if (rt->rx_mode == BSP_UART_CAP_DMA) {
        if (HAL_UARTEx_ReceiveToIdle_DMA(hal, cfg->rx_buf,
                                        cfg->rx_buf_size) != HAL_OK) {
            return BSP_UART_ERR_HAL;
        }
        return BSP_UART_OK;
    }
    if (rt->rx_mode == BSP_UART_CAP_IT) {
        if (HAL_UART_Receive_IT(hal, (uint8_t *)&rt->rx_byte, 1U) != HAL_OK) {
            return BSP_UART_ERR_HAL;
        }
        return BSP_UART_OK;
    }
    return BSP_UART_OK;   /* 阻塞档：没有人打断，靠轮询 */
}

/**
 * @brief 从设备层取一段待发字节到暂存区
 * @param rt 运行块，不可为 NULL
 * @retval 取到的字节数，0 表示设备层暂时没有数据
 * @note 在临界区或中断里调用，依赖设备层的 tx_fetch 是 ISR 安全的。
 */
static uint16_t BSP_UART_TxFetch(bsp_uart_rt_t *rt)
{
    uint16_t n;

    if (rt->hooks.tx_fetch == NULL) {
        return 0U;
    }
    n = rt->hooks.tx_fetch(rt->hooks.ctx, rt->cfg->tx_stage,
                           rt->cfg->tx_stage_size);
    if (n > rt->cfg->tx_stage_size) {
        n = rt->cfg->tx_stage_size;   /* 钩子返回越界值时就地截断 */
    }
    return n;
}

/**
 * @brief 按当前发送档位把暂存区这一段交给外设
 * @param rt 运行块，不可为 NULL
 * @retval BSP_UART_OK: 已交给外设（异步）或已发完（阻塞）
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 * @note 只用于 DMA / 中断两档；阻塞档在 BSP_UART_TxKick() 里同步发。
 */
static bsp_uart_status_t BSP_UART_TxStart(bsp_uart_rt_t *rt)
{
    UART_HandleTypeDef *hal = (UART_HandleTypeDef *)rt->cfg->hal;
    HAL_StatusTypeDef st;

    if (rt->tx_mode == BSP_UART_CAP_DMA) {
        st = HAL_UART_Transmit_DMA(hal, rt->cfg->tx_stage, rt->tx_len);
    } else {
        st = HAL_UART_Transmit_IT(hal, rt->cfg->tx_stage, rt->tx_len);
    }
    return (st == HAL_OK) ? BSP_UART_OK : BSP_UART_ERR_HAL;
}

/**
 * @brief 算一段字节发完需要多少毫秒
 * @param hal HAL UART 句柄，不可为 NULL
 * @param len 字节数，不能为 0
 * @retval 阻塞发送的等待上限（毫秒）
 * @note 按 10 bit/字节（8N1）向上取整，再加 2 ms 余量；波特率取自 HAL 句柄
 *       里的配置值，不写死数字。
 */
static uint32_t BSP_UART_SendBudgetMs(const UART_HandleTypeDef *hal, uint16_t len)
{
    uint32_t baud = (hal->Init.BaudRate != 0U) ? hal->Init.BaudRate : 1U;

    return (uint32_t)((((uint32_t)len * 10000U) + baud - 1U) / baud) + 2U;
}

/*==============================================================================
 * 接口实现：初始化与启动
 *============================================================================*/
/**
 * @brief 初始化全部实例：校验配置、探测两面能力、把接收 DMA 改成循环
 * @retval BSP_UART_OK: 全部实例就绪（已就绪的实例会被跳过）
 * @retval BSP_UART_ERR_STATE: 缺 HAL 句柄、缓冲区未配置、外设不在 READY
 * @retval BSP_UART_ERR_HAL: 接收 DMA 改成循环模式时 HAL_DMA_Init() 失败
 * @note 必须在 MX_USARTx_UART_Init() 之后调用，不能在中断里调用。
 *       若 .ioc 把接收 DMA 配成 Normal，这里会改成 Circular 并重新
 *       HAL_DMA_Init()——半区模型依赖硬件循环写，Normal 只收一圈就停。
 */
bsp_uart_status_t BSP_UART_Init(void)
{
    uint32_t i;

    for (i = 0U; i < s_uart_count; i++) {
        const bsp_uart_t *cfg = s_handles[i];
        bsp_uart_rt_t *rt = s_handles[i]->rt;
        UART_HandleTypeDef *hal;

        if (rt == NULL) {
            return BSP_UART_ERR_STATE;
        }
        if (rt->initialized != 0U) {
            continue;
        }
        if ((cfg->hal == NULL) || (cfg->rx_buf == NULL) ||
            (cfg->tx_stage == NULL) || (cfg->rx_buf_size < 16U) ||
            (cfg->tx_stage_size < 16U)) {
            return BSP_UART_ERR_STATE;
        }

        hal = (UART_HandleTypeDef *)cfg->hal;
        if ((hal->gState != HAL_UART_STATE_READY) ||
            (hal->RxState != HAL_UART_STATE_READY)) {
            return BSP_UART_ERR_STATE;
        }

        /* ---------- 1. 接收 DMA 必须是循环模式，否则半区模型只有一圈 ---------- */
        if ((hal->hdmarx != NULL) && (hal->hdmarx->Init.Mode != DMA_CIRCULAR)) {
            hal->hdmarx->Init.Mode = DMA_CIRCULAR;
            if (HAL_DMA_Init(hal->hdmarx) != HAL_OK) {
                return BSP_UART_ERR_HAL;
            }
        }

        /* ---------- 2. 探测两面的能力，先按最高档起步 ---------- */
        rt->cfg         = cfg;
        rt->tx_mode     = BSP_UART_ActualTx(hal);
        rt->rx_mode     = BSP_UART_ActualRx(hal);
        rt->rx_pub      = 0U;
        rt->tx_len      = 0U;
        rt->tx_busy     = 0U;
        rt->ready       = 0U;
        rt->initialized = 1U;
    }

    return BSP_UART_OK;
}

/**
 * @brief 注册钩子并启动接收，失败时向下退档重试
 * @param uart  实例句柄，不可为 NULL
 * @param hooks 钩子表，不可为 NULL，两个回调都必须非空
 * @retval BSP_UART_OK: 已启动
 * @retval BSP_UART_ERR_PARAM: 句柄或钩子非法
 * @retval BSP_UART_ERR_STATE: 未初始化，或已经启动
 * @retval BSP_UART_ERR_HAL: 退到阻塞档仍然启动失败
 * @note 重复调用返回 ERR_STATE；要换钩子先 BSP_UART_Abort()。
 *       退档只发生在启动失败时：DMA 起不来就试中断，中断起不来就只留轮询。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 const bsp_uart_hooks_t *hooks)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    bsp_uart_status_t st;

    if ((rt == NULL) || (rt->cfg == NULL) || (hooks == NULL) ||
        (hooks->rx_push == NULL) || (hooks->tx_fetch == NULL)) {
        return BSP_UART_ERR_PARAM;
    }
    if (rt->initialized == 0U) {
        return BSP_UART_ERR_STATE;
    }
    if (rt->ready != 0U) {
        return BSP_UART_ERR_STATE;
    }

    rt->hooks = *hooks;

    for (;;) {
        st = BSP_UART_RxArm(rt);
        if (st == BSP_UART_OK) {
            break;
        }
        if (rt->rx_mode <= BSP_UART_CAP_BLOCKING) {
            return BSP_UART_ERR_HAL;
        }
        rt->rx_mode--;
    }

    rt->tx_len  = 0U;
    rt->tx_busy = 0U;
    rt->ready   = 1U;
    return BSP_UART_OK;
}

/**
 * @brief 查询是否已启动
 * @param uart 实例句柄，可为 NULL
 * @retval 1: 已启动且接收在跑
 * @retval 0: 未启动、已中止或句柄非法
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);

    return ((rt != NULL) && (rt->ready != 0U)) ? 1U : 0U;
}

/*==============================================================================
 * 接口实现：发送
 *============================================================================*/
/**
 * @brief 通知本层可以发送了，空闲时取一段塞进外设
 * @param uart 实例句柄，不可为 NULL
 * @retval BSP_UART_OK: 已启动，或本来就在跑，或暂时没数据
 * @retval BSP_UART_ERR_PARAM: 句柄非法
 * @retval BSP_UART_ERR_STATE: 未启动
 * @retval BSP_UART_ERR_HAL: 启动失败（已向下退一档，下次调用用新档位重试）
 * @note 阻塞档在本函数里同步发完，期间一直占着调用线程；DMA / 中断档只
 *       启动一段就返回，剩下的由完成回调接续。
 */
bsp_uart_status_t BSP_UART_TxKick(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    UART_HandleTypeDef *hal;
    uint32_t primask;
    bsp_uart_status_t st;

    if ((rt == NULL) || (rt->cfg == NULL)) {
        return BSP_UART_ERR_PARAM;
    }
    if (rt->ready == 0U) {
        return BSP_UART_ERR_STATE;
    }

    hal = (UART_HandleTypeDef *)rt->cfg->hal;

    /* ---------- 阻塞档：取一段发一段，直到设备层没有数据 ---------- */
    if (rt->tx_mode == BSP_UART_CAP_BLOCKING) {
        for (;;) {
            uint16_t n = BSP_UART_TxFetch(rt);
            HAL_StatusTypeDef st;

            if (n == 0U) {
                return BSP_UART_OK;
            }
            st = HAL_UART_Transmit(hal, rt->cfg->tx_stage, n,
                                   BSP_UART_SendBudgetMs(hal, n));
            if (st != HAL_OK) {
                return (st == HAL_TIMEOUT) ? BSP_UART_ERR_TIMEOUT
                                           : BSP_UART_ERR_HAL;
            }
        }
    }

    /* ---------- DMA / 中断档：登记 + 启动，临界区内不等待硬件 ---------- */
    primask = __get_PRIMASK();
    __disable_irq();

    if (rt->tx_busy != 0U) {
        __set_PRIMASK(primask);
        return BSP_UART_OK;
    }
    if (rt->tx_len == 0U) {
        rt->tx_len = BSP_UART_TxFetch(rt);
        if (rt->tx_len == 0U) {
            __set_PRIMASK(primask);
            return BSP_UART_OK;
        }
    }

    rt->tx_busy = 1U;
    st = BSP_UART_TxStart(rt);
    if (st != BSP_UART_OK) {
        rt->tx_busy = 0U;    /* 暂存区里的字节保留，下次用低一档重试 */
        if (rt->tx_mode > BSP_UART_CAP_BLOCKING) {
            rt->tx_mode--;
        }
    }

    __set_PRIMASK(primask);
    return st;
}

/**
 * @brief 查询发送路径是否已经空闲
 * @param uart 实例句柄，可为 NULL
 * @retval 1: 暂存区空、没有在途传输、外设发送状态机已回到 READY
 * @retval 0: 仍在发送、未启动或句柄非法
 * @note 只回答"现在空不空"，不等待；设备层的 Flush 轮询本函数。
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    UART_HandleTypeDef *hal;

    if ((rt == NULL) || (rt->cfg == NULL) || (rt->ready == 0U)) {
        return 0U;
    }

    hal = (UART_HandleTypeDef *)rt->cfg->hal;

    if ((rt->tx_len != 0U) || (rt->tx_busy != 0U) ||
        (hal->gState != HAL_UART_STATE_READY)) {
        return 0U;
    }
    return 1U;
}

/*==============================================================================
 * 接口实现：轮询与中止
 *============================================================================*/
/**
 * @brief 轮询接收：仅接收退到阻塞档时使用
 * @param uart 实例句柄，不可为 NULL
 * @retval BSP_UART_OK: 已处理（可能 0 个字节）
 * @retval BSP_UART_ERR_PARAM: 句柄非法
 * @retval BSP_UART_ERR_STATE: 未启动，或接收不在阻塞档
 * @note 不阻塞、不等待。一次最多取一小批，避免长时间占着调用者；读 DR
 *       会顺带清掉 ORE / FE / NE，所以这里不需要额外的清标志步骤。
 */
bsp_uart_status_t BSP_UART_Poll(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);
    UART_HandleTypeDef *hal;
    uint8_t buf[16];
    uint16_t n = 0U;

    if ((rt == NULL) || (rt->cfg == NULL)) {
        return BSP_UART_ERR_PARAM;
    }
    if (rt->ready == 0U) {
        return BSP_UART_ERR_STATE;
    }
    if (rt->rx_mode != BSP_UART_CAP_BLOCKING) {
        return BSP_UART_ERR_STATE;
    }

    hal = (UART_HandleTypeDef *)rt->cfg->hal;

    while ((n < (uint16_t)sizeof(buf)) &&
           (__HAL_UART_GET_FLAG(hal, UART_FLAG_RXNE) != RESET)) {
        buf[n] = (uint8_t)(hal->Instance->DR & 0xFFU);
        n++;
    }
    BSP_UART_RxGive(rt, buf, n);
    return BSP_UART_OK;
}

/**
 * @brief 中止收发，清掉在途状态
 * @param uart 实例句柄，不可为 NULL
 * @retval BSP_UART_OK: 已停
 * @retval BSP_UART_ERR_PARAM: 句柄非法
 * @retval BSP_UART_ERR_STATE: 未初始化
 * @note 之后可以重新 BSP_UART_Start() 注册钩子；已经交给设备层的字节不受影响。
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart)
{
    bsp_uart_rt_t *rt = BSP_UART_Find(uart);

    if ((rt == NULL) || (rt->cfg == NULL)) {
        return BSP_UART_ERR_PARAM;
    }
    if (rt->initialized == 0U) {
        return BSP_UART_ERR_STATE;
    }

    (void)HAL_UART_Abort((UART_HandleTypeDef *)rt->cfg->hal);
    rt->ready   = 0U;
    rt->tx_busy = 0U;
    rt->tx_len  = 0U;
    rt->rx_pub  = 0U;
    return BSP_UART_OK;
}

/*==============================================================================
 * HAL 回调覆盖
 *
 * 覆盖的是 HAL 的 __weak 回调，不是 USARTx_IRQHandler / DMAx_StreamY_IRQHandler
 * 本身；中断向量仍由工程的中断文件分发。
 *
 *   接收 DMA 档：HAL_UARTEx_ReceiveToIdle_DMA() 把 HT / TC / IDLE 统一送到
 *                  HAL_UARTEx_RxEventCallback()，事件类型在 huart->RxEventType；
 *   接收中断档：HAL_UART_Receive_IT() 每个字节进一次 HAL_UART_RxCpltCallback()；
 *   接收阻塞档：两个回调都不参与，数据由 BSP_UART_Poll() 主动取；
 *   发送 DMA / 中断档：HAL_UART_TxCpltCallback() 一段完成后接续下一段。
 *============================================================================*/
/**
 * @brief 接收事件回调：把 HT / TC / IDLE 换算成同一个写指针位置再交付
 * @param huart HAL UART 句柄
 * @param Size  HAL 给出的事件位置，本文件不用（三种事件含义不一致），
 *              统一改用 RxEventType + DMA 计数器推导
 * @retval 无
 * @note 中断上下文，只算增量并调用 rx_push，不访问任何 OS 对象。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    bsp_uart_rt_t *rt = BSP_UART_FindByHal(huart);
    uint16_t pos;
    uint16_t size;

    (void)Size;

    if ((rt == NULL) || (rt->ready == 0U) ||
        (rt->rx_mode != BSP_UART_CAP_DMA) || (huart->hdmarx == NULL)) {
        return;
    }

    size = rt->cfg->rx_buf_size;

    switch (huart->RxEventType) {
    case HAL_UART_RXEVENT_HT:
        pos = (uint16_t)(size / 2U);      /* 前半区已稳定 */
        break;
    case HAL_UART_RXEVENT_TC:
        pos = size;                       /* 整圈完成 */
        break;
    case HAL_UART_RXEVENT_IDLE:
    default:
        /* 半区内已收到的尾部：用 DMA 剩余计数反推当前写指针 */
        pos = (uint16_t)(size - __HAL_DMA_GET_COUNTER(huart->hdmarx));
        break;
    }

    BSP_UART_RxPublish(rt, pos);
}

/**
 * @brief 接收完成回调（仅中断档）：一个字节到了就交出去，然后重新武装
 * @param huart HAL UART 句柄
 * @retval 无
 * @note 中断上下文。DMA 档不走这里（走 RxEventCallback），阻塞档不武装中断。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_FindByHal(huart);

    if ((rt == NULL) || (rt->ready == 0U) ||
        (rt->rx_mode != BSP_UART_CAP_IT)) {
        return;
    }

    BSP_UART_RxGive(rt, (const uint8_t *)&rt->rx_byte, 1U);
    (void)HAL_UART_Receive_IT(huart, (uint8_t *)&rt->rx_byte, 1U);
}

/**
 * @brief 发送完成回调：清忙标志，接着从设备层取下一段
 * @param huart HAL UART 句柄
 * @retval 无
 * @note 中断上下文。HAL 在回调前已把 gState 与 DMA 状态复位成 READY，
 *       所以这里可以安全地再次启动发送。阻塞档不走这里。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_FindByHal(huart);

    if ((rt == NULL) || (rt->ready == 0U) ||
        (rt->tx_mode == BSP_UART_CAP_BLOCKING)) {
        return;
    }

    rt->tx_busy = 0U;
    rt->tx_len  = 0U;
    (void)BSP_UART_TxKick(rt->cfg);
}

/**
 * @brief 错误回调：ORE / FE / NE 会终止接收，这里按当前档位原地重启
 * @param huart HAL UART 句柄
 * @retval 无
 * @note 上电时对端已在发、或线路干扰导致 ORE 是常态；不重启就永久收不到。
 *       重启失败不重试（反复失败只会更糟），ready 清零让上层能看到。
 *       阻塞档没有中断可重启，读 DR 已清掉标志，直接返回。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    bsp_uart_rt_t *rt = BSP_UART_FindByHal(huart);

    if ((rt == NULL) || (rt->ready == 0U)) {
        return;
    }
    if (rt->rx_mode == BSP_UART_CAP_BLOCKING) {
        return;
    }

    if (BSP_UART_RxArm(rt) != BSP_UART_OK) {
        rt->ready = 0U;   /* 接收彻底停了，让查询接口能反映出来 */
    }
}
