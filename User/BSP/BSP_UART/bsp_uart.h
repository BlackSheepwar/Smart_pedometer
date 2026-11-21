/**
 * @file bsp_uart.h
 * @brief UART 硬件驱动层：能力探测、三档搬运、收发字节的注入接口
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 本层只答芯片机制，不做任何缓冲策略：
 *       1. 初始化时从 HAL 句柄探测收、发两面的搬运能力（DMA / 中断 / 阻塞），
 *          与 bsp_spi / bsp_iic 用同一套档位语义；启动失败时向下退一档；
 *       2. 收发字节不在本层排队：收到的字节立刻交给 rx_push，要发的字节由
 *          tx_fetch 现取，本层只负责把它们塞进外设；
 *       3. 不含任何 OS 调用、不加锁、不等待。缓冲、溢出记账、等待与串行化
 *          都在设备层（dev_uart）；
 *       4. 换到裸机时设备层把等待换成自旋即可，本层一个字都不用改。
 *
 *       所有权：各口 HAL 句柄与两条 DMA 句柄由本模块独占，上层不要再直接调
 *       HAL_UART_Transmit_DMA() 之类的接口，否则本层的忙标志与实际状态脱节。
 */

#ifndef BSP_UART_H
#define BSP_UART_H

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 搬运档位
 *============================================================================*/
/** @brief 0 无功能：该方向不可用（阻塞档恒可用，保留此值让三档编号对齐） */
#define BSP_UART_CAP_NONE     0U

/** @brief 1 阻塞：调用返回时已经搬完，不占用中断 */
#define BSP_UART_CAP_BLOCKING 1U

/** @brief 2 中断：每字节一次外设中断，完成点只在回调里 */
#define BSP_UART_CAP_IT       2U

/** @brief 3 DMA：启动后 CPU 不参与搬字节，完成点只在流中断回调里 */
#define BSP_UART_CAP_DMA      3U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief BSP_UART 返回码
 * @note 数值即契约：已发布的数值不再重排。
 */
typedef enum {
    BSP_UART_OK          = 0,  /* 成功；DMA/中断方式下只表示已经交给外设 */
    BSP_UART_ERR_PARAM   = -1, /* 参数非法 */
    BSP_UART_ERR_STATE   = -2, /* 未初始化 / 未启动 / 档位不匹配 */
    BSP_UART_ERR_TIMEOUT = -3, /* 阻塞方式下外设超时 */
    BSP_UART_ERR_HAL     = -4, /* HAL 启动或重启失败 */
} bsp_uart_status_t;

/**
 * @brief UART 句柄（不透明，布局只在 .c 里出现）
 */
typedef struct bsp_uart bsp_uart_t;

/**
 * @brief 收字节回调：把新到的字节交给上层
 * @param ctx  注册钩子时给的上下文
 * @param data 字节起始地址，仅在本次调用期间有效
 * @param len  字节数，不会为 0
 * @note 在中断上下文调用，必须 ISR 安全；不得阻塞、不得调用非 FromISR 的
 *       OS 接口。实现方负责自己的缓冲与溢出策略。
 */
typedef void (*bsp_uart_rx_push_t)(void *ctx, const uint8_t *data, uint16_t len);

/**
 * @brief 取发送字节：把待发数据填进 buf
 * @param ctx 注册钩子时给的上下文
 * @param buf 目的缓冲区，不可为 NULL
 * @param max 本次最多填多少字节，不会为 0
 * @retval 实际填入的字节数，0 表示当前没有待发数据
 * @note DMA / 中断方式下在中断上下文调用（中断里接着发下一段），阻塞方式下
 *       在调用 BSP_UART_TxKick() 的任务上下文调用；两种情况都必须 ISR 安全。
 */
typedef uint16_t (*bsp_uart_tx_fetch_t)(void *ctx, uint8_t *buf, uint16_t max);

/**
 * @brief 收发钩子：设备层注册进来，本层只调用，不持有
 */
typedef struct {
    bsp_uart_rx_push_t rx_push;  /* 收到的字节往哪里送，必须非空 */
    bsp_uart_tx_fetch_t tx_fetch;/* 要发的字节从哪里取，必须非空 */
    void               *ctx;     /* 原样回传给两个回调 */
} bsp_uart_hooks_t;

/*==============================================================================
 * 外部变量声明
 *============================================================================*/
/**
 * @brief 实例句柄：只导出句柄，运行块与缓冲区定义在 .c 的移植段里
 * @note 每路 UART 一个；上层按句柄区分端口，不直接摸 HAL 句柄。
 */
extern const bsp_uart_t bsp_uart1;

/*==============================================================================
 * 接口声明
 *============================================================================*/
/**
 * @brief 初始化全部实例：校验配置并探测两面的搬运能力
 * @retval BSP_UART_OK: 全部实例就绪（已就绪的实例会被跳过）
 * @retval BSP_UART_ERR_STATE: 缺 HAL 句柄、外设不在 READY、缓冲区未配置
 * @retval BSP_UART_ERR_HAL: 接收 DMA 改成循环模式时 HAL_DMA_Init() 失败
 * @note 必须在 MX_USARTx_UART_Init() 之后调用，不能在中断里调用。
 *       探测只看两件事：DMA 流有没有挂上、缓冲区配了没有；探测结果记在
 *       运行块里，启动与搬运按它选档。
 */
bsp_uart_status_t BSP_UART_Init(void);

/**
 * @brief 注册钩子并启动接收
 * @param uart  实例句柄，不可为 NULL
 * @param hooks 钩子表，不可为 NULL，两个回调都必须非空
 * @retval BSP_UART_OK: 已启动（DMA 方式下接收已武装）
 * @retval BSP_UART_ERR_PARAM: 句柄或钩子非法
 * @retval BSP_UART_ERR_STATE: 未初始化，或已经启动
 * @retval BSP_UART_ERR_HAL: 启动接收失败
 * @note 重复调用直接返回 ERR_STATE；要换钩子先 BSP_UART_Abort()。
 *       启动后收到的每个字节都会经 rx_push 交出去，本层不存。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 const bsp_uart_hooks_t *hooks);

/**
 * @brief 查询是否已启动
 * @param uart 实例句柄，可为 NULL
 * @retval 1: 已启动且接收在跑
 * @retval 0: 未启动、已中止或句柄非法
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart);

/**
 * @brief 通知本层"有新数据可以发"，空闲时立刻取一段塞进外设
 * @param uart 实例句柄，不可为 NULL
 * @retval BSP_UART_OK: 已启动发送，或发送本来就在跑，或暂时没数据
 * @retval BSP_UART_ERR_PARAM: 句柄非法
 * @retval BSP_UART_ERR_STATE: 未启动
 * @note 由设备层在把字节写进自己的发送队列之后调用；阻塞方式下本函数会
 *       一直取到 tx_fetch 返回 0 才返回（数据是同步发出去的）。
 */
bsp_uart_status_t BSP_UART_TxKick(const bsp_uart_t *uart);

/**
 * @brief 查询发送路径是否已经空闲
 * @param uart 实例句柄，可为 NULL
 * @retval 1: 当前没有在途数据，外设发送状态机已回到 READY
 * @retval 0: 仍在发送、未启动或句柄非法
 * @note 只回答"现在空不空"，不等待。设备层的 Flush 轮询本函数。
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart);

/**
 * @brief 轮询接收：仅当接收退到阻塞档时使用，把已到的字节交给 rx_push
 * @param uart 实例句柄，不可为 NULL
 * @retval BSP_UART_OK: 已处理（可能有 0 个字节）
 * @retval BSP_UART_ERR_PARAM: 句柄非法
 * @retval BSP_UART_ERR_STATE: 未启动，或接收不在阻塞档
 * @note 不阻塞、不等待；DMA / 中断档下返回 ERR_STATE。裸机工程在主循环里
 *       周期性调用，或由设备层在读写入口调用。
 */
bsp_uart_status_t BSP_UART_Poll(const bsp_uart_t *uart);

/**
 * @brief 中止收发：停掉 DMA / 中断，清掉在途状态
 * @param uart 实例句柄，不可为 NULL
 * @retval BSP_UART_OK: 已停
 * @retval BSP_UART_ERR_PARAM: 句柄非法
 * @retval BSP_UART_ERR_STATE: 未初始化
 * @note 之后可以重新 BSP_UART_Start()；已交给设备层的字节不受影响。
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart);

#endif /* BSP_UART_H */
