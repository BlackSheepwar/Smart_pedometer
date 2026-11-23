/**
 * @file bsp_uart.h
 * @brief UART 芯片层：循环 DMA 收字节、按窗口位置报进度
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 本层只回答"这颗芯片的 UART 怎样搬字节"，不持有任何数据缓冲、
 *       不做 OS 同步。接收缓冲与收发 FIFO 都归设备层（dev_uart），端口
 *       绑定只在 BSP_UART_Start() 里登记一次。中断路径只更新绝对接收
 *       计数并回调通知，不复制数据、不等待。
 *
 *       接收窗口语义：DMA 以 circular 模式搬运 2*dma_node 字节，HAL 在
 *       HT / TC / IDLE 回调里给出的 Size 是"自本次武装以来"的绝对窗口
 *       位置（0..2*dma_node）：HT 固定报 dma_node，TC 固定报
 *       2*dma_node（等价于 0），IDLE 报当前 NDTR 折算的位置。本层用
 *       (pos - last) & (2*dma_node - 1) 求增量并累加到 rx_total，因此
 *       半满、全满、空闲三种事件走同一条路径，短帧不会再丢。
 */

#ifndef __BSP_UART_H__
#define __BSP_UART_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 事件位
 *============================================================================*/
/** @brief 接收 DMA 到达半缓冲边界（窗口位置 = dma_node） */
#define BSP_UART_EV_RX_HALF     0x00000001U
/** @brief 接收 DMA 到达整缓冲边界（窗口位置 = 2*dma_node） */
#define BSP_UART_EV_RX_FULL     0x00000002U
/** @brief 接收线路空闲 */
#define BSP_UART_EV_RX_IDLE     0x00000004U
/** @brief 接收发生错误，本层已停止接收，需上层重新武装 */
#define BSP_UART_EV_RX_ERROR    0x00000008U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 芯片层返回码
 * @note 数值即契约：已发布的数值不再重排。
 */
typedef enum {
    BSP_UART_OK            =  0,  /* 操作成功 */
    BSP_UART_ERR_PARAM     = -1,  /* 参数非法 */
    BSP_UART_ERR_STATE     = -2,  /* 状态不允许 */
    BSP_UART_ERR_TIMEOUT   = -3,  /* 外设等待超时 */
    BSP_UART_ERR_HAL       = -4,  /* HAL 操作失败 */
} bsp_uart_status_t;

/** @brief UART 总线句柄：不透明，布局位于 bsp_uart.c */
typedef struct bsp_uart bsp_uart_t;

/**
 * @brief 接收事件回调：只报告事件与累计字节数，不搬运数据
 * @param ctx    注册上下文
 * @param events 事件位，取值见 BSP_UART_EV_*
 * @param total  自武装以来累计接收的绝对字节数
 * @retval 无
 *
 * @note 在中断上下文调用；实现只能置标志或发 ISR 安全通知，不得阻塞、
 *       加锁、复制数据或调用 BSP 接口。
 */
typedef void (*bsp_uart_rx_event_fn)(void *ctx, uint32_t events, uint32_t total);

/**
 * @brief 发送完成回调：报告一段 DMA 发送的结局
 * @param ctx 注册上下文
 * @param ok  1 = 最后一位已移出（USART TC）；0 = 因错误中止，可能只发出一部分
 * @retval 无
 *
 * @note 在中断上下文调用；只允许有界索引更新与 ISR 安全通知。
 */
typedef void (*bsp_uart_tx_done_fn)(void *ctx, uint8_t ok);

/**
 * @brief UART 端口绑定：由设备层在 Start 时提供
 *
 * @note 接收缓冲由设备层持有；dma_node 是它的半缓冲字节数，缓冲总长为
 *       2*dma_node。DMA 采用 circular 模式，由 CubeMX 配置，本层不改配置。
 */
typedef struct {
    /** 双节点 DMA 接收缓冲起点，长度 2*dma_node */
    uint8_t *dma_buf;
    /** 单个 DMA 节点字节数：16 至 16384 的 2 次幂 */
    uint16_t dma_node;
    /** 接收事件通知 */
    bsp_uart_rx_event_fn on_rx_event;
    /** 发送完成通知 */
    bsp_uart_tx_done_fn on_tx_done;
    /** 回调共用上下文 */
    void *ctx;
} bsp_uart_port_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/** @brief UART1 芯片层句柄 */
extern const bsp_uart_t bsp_uart1;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 校验端口实例：HAL 外设就绪、收发 DMA 均已配置
 * @retval BSP_UART_OK: 所有注册实例均可用
 * @retval BSP_UART_ERR_PARAM: 实例、HAL 外设或运行块为空
 * @retval BSP_UART_ERR_STATE: HAL 收发状态非 READY，或接收 DMA 非循环模式
 *
 * @note 只读检查，不修改 CubeMX 生成的 DMA 配置；本工程要求 .ioc 已把
 *       接收 DMA 配成 circular，否则返回 ERR_STATE 由上层暴露配置问题。
 */
bsp_uart_status_t BSP_UART_Init(void);

/**
 * @brief 绑定设备层缓冲与回调，并启动循环 DMA 接收
 * @param uart 总线句柄
 * @param port 端口绑定
 * @retval BSP_UART_OK: 接收已启动
 * @retval BSP_UART_ERR_PARAM: 句柄、HAL、缓冲或回调非法
 * @retval BSP_UART_ERR_STATE: 已经启动或 HAL 正忙
 * @retval BSP_UART_ERR_HAL: HAL 接收启动失败
 *
 * @note 任务上下文调用；启动后本层不再等待，进度只由中断回调报告。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 const bsp_uart_port_t *port);

/**
 * @brief 读取自武装以来累计接收的绝对字节数
 * @param uart 总线句柄
 * @retval 0 至 UINT32_MAX 累计字节数；句柄无效时为 0
 *
 * @note 半满、全满、空闲三种事件都会推进该计数；调用者用它计算需要从
 *       DMA 缓冲搬运的增量，不得假设它只按节点对齐。
 */
uint32_t BSP_UART_RxTotal(const bsp_uart_t *uart);

/**
 * @brief 查询接收是否处于武装状态
 * @param uart 总线句柄
 * @retval 1: 接收 DMA 已武装
 * @retval 0: 句柄无效、尚未启动或接收已被错误停止
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart);

/**
 * @brief 接收错误后重新武装循环 DMA
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 接收已重新武装
 * @retval BSP_UART_ERR_PARAM: 句柄、端口或运行块非法
 * @retval BSP_UART_ERR_STATE: 尚未启动，或 HAL 仍忙、稍后可重试
 * @retval BSP_UART_ERR_TIMEOUT: 中止旧接收超时
 * @retval BSP_UART_ERR_HAL: HAL 重新武装失败
 *
 * @note 任务上下文调用。重新武装把 HAL 窗口位置清零，但 rx_total 保持
 *       连续；调用者必须以"本次调用成功时的 BSP_UART_RxTotal()"为新的
 *       物理基准，重新对齐 DMA 缓冲的读写位置。
 */
bsp_uart_status_t BSP_UART_RxRearm(const bsp_uart_t *uart);

/**
 * @brief 启动一段 DMA 发送
 * @param uart 总线句柄
 * @param data 数据起点，回调前必须保持有效
 * @param len  字节数，1 至 65535
 * @retval BSP_UART_OK: DMA 已启动
 * @retval BSP_UART_ERR_PARAM: 句柄、数据或长度非法
 * @retval BSP_UART_ERR_STATE: 尚未启动、已有一段在途，或 HAL 正忙
 * @retval BSP_UART_ERR_TIMEOUT: HAL 启动超时
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 任务上下文调用；完成点只会由 on_tx_done 报告。同一实例在途期间
 *       再次调用返回 ERR_STATE，由设备层串行化。
 */
bsp_uart_status_t BSP_UART_TxStart(const bsp_uart_t *uart,
                                   const uint8_t *data, uint16_t len);

/**
 * @brief 查询发送是否空闲
 * @param uart 总线句柄
 * @retval 1: 句柄无效或当前无在途段
 * @retval 0: 仍有一段 DMA 在途
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart);

/**
 * @brief 中止收发并结束在途发送段
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 中止成功
 * @retval BSP_UART_ERR_PARAM: 句柄、HAL 或运行块为空
 * @retval BSP_UART_ERR_HAL: HAL 中止失败
 *
 * @note 任务上下文调用；在途发送段按"失败"通知设备层，由其归还队列。
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart);

/**
 * @brief 原子读取并清零累计硬件错误位
 * @param uart 总线句柄
 * @param out  错误位输出地址；为空时不操作
 * @retval 无
 *
 * @note 位定义与 HAL_UART_ERROR_* 一致；读后清零，避免同一错误被重复统计。
 */
void BSP_UART_Errors(const bsp_uart_t *uart, uint32_t *out);

#endif /* __BSP_UART_H__ */
