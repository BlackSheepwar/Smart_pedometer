/**
 * @file    bsp_uart.h
 * @brief   UART 芯片端口：探测搬运能力、启动外设并报告传输进度。
 * @author  Ricardo
 * @date    2026-11-22
 *
 * 接收缓冲与发送 FIFO 由设备层持有。DMA 累计量仅包含已完成的半缓冲节点；
 * 空闲事件只用于唤醒。中断路径不复制数据、不等待、不使用 OS。
 */

#ifndef __BSP_UART_H__
#define __BSP_UART_H__

#include <stdint.h>

/*==============================================================================
 * 搬运档位
 *============================================================================*/

/** @brief 0 无功能：该方向不可用（保留此值让三档编号对齐） */
#define BSP_UART_CAP_NONE       0U
/** @brief 1 阻塞：调用返回时已经搬完，不占用中断 */
#define BSP_UART_CAP_BLOCKING   1U
/** @brief 2 中断：每字节一次外设中断，完成点只在回调里 */
#define BSP_UART_CAP_IT         2U
/** @brief 3 DMA：启动后 CPU 不参与搬字节，完成点只在流中断回调里 */
#define BSP_UART_CAP_DMA        3U

/*==============================================================================
 * 事件位
 *============================================================================*/

/** @brief 接收 DMA 到达半缓冲边界 */
#define BSP_UART_EV_RX_HALF     0x00000001U
/** @brief 接收 DMA 到达整缓冲边界 */
#define BSP_UART_EV_RX_FULL     0x00000002U
/** @brief 接收线路空闲 */
#define BSP_UART_EV_RX_IDLE     0x00000004U
/** @brief 中断档收到一个字节 */
#define BSP_UART_EV_RX_BYTE     0x00000008U
/** @brief 发送段完成 */
#define BSP_UART_EV_TX_DONE     0x00000010U
/** @brief 发生接收或发送错误 */
#define BSP_UART_EV_ERROR       0x00000020U

/*==============================================================================
 * 类型定义
 *============================================================================*/

/**
 * @brief 芯片层返回码
 * @note 数值即契约：已发布的数值不再重排。
 */
typedef enum
{
    BSP_UART_OK            =  0,  /**< 操作成功 */
    BSP_UART_ERR_PARAM     = -1,  /**< 参数非法 */
    BSP_UART_ERR_STATE     = -2,  /**< 状态不允许 */
    BSP_UART_ERR_TIMEOUT   = -3,  /**< 阻塞搬运超时 */
    BSP_UART_ERR_HAL       = -4,  /**< HAL 操作失败 */
} bsp_uart_status_t;

/** @brief UART 总线句柄：不透明，布局位于 bsp_uart.c */
typedef struct bsp_uart bsp_uart_t;

/**
 * @brief 中断事件回调：只报告事件，不搬运数据
 * @param ctx     注册上下文
 * @param events  事件位
 *
 * @note 在中断上下文调用；实现只能置标志或发 ISR 安全通知，不得阻塞、
 *       加锁、复制数据或调用 BSP 接口。
 */
typedef void (*bsp_uart_event_fn)(void *ctx, uint32_t events);

/**
 * @brief 发送队列取数回调：返回队首连续区间
 * @param ctx 注册上下文
 * @param data 输出数据地址；无数据时写入 NULL
 * @return uint16_t
 * @retval 1 至 UINT16_MAX 可发送字节数
 * @retval 0 参数无效或队列为空
 */
typedef uint16_t (*bsp_uart_tx_acquire_t)(void *ctx, const uint8_t **data);

/**
 * @brief 发送段完成回调：只推进发送 FIFO 读位置
 * @param ctx  注册上下文
 * @param len  已发送字节数
 * @retval 无
 *
 * @note 在中断上下文调用；只允许有界索引更新。
 */
typedef void (*bsp_uart_tx_release_t)(void *ctx, uint16_t len);

/**
 * @brief UART 端口绑定：由设备层在 Start 时提供
 */
typedef struct
{
    /** 双节点 DMA 接收缓冲起点 */
    uint8_t *dma_buf;
    /** 单个 DMA 节点字节数，必须为不小于 16 的 2 次幂 */
    uint16_t dma_node;
    /** 中断事件通知 */
    bsp_uart_event_fn on_event;
    /** 发送队列连续区间读取 */
    bsp_uart_tx_acquire_t tx_acquire;
    /** 发送队列完成归还 */
    bsp_uart_tx_release_t tx_release;
    /** 所有回调共用上下文 */
    void *ctx;
} bsp_uart_port_t;

/** @brief UART1 芯片层句柄 */
extern const bsp_uart_t bsp_uart1;

/*==============================================================================
 * 函数声明
 *============================================================================*/

/**
 * @brief 检查 HAL 状态并探测收发搬运档位
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        探测完成
 * @retval BSP_UART_ERR_PARAM 外设句柄无效
 * @retval BSP_UART_ERR_STATE 外设尚未就绪
 * @retval BSP_UART_ERR_HAL   DMA 配置失败
 */
bsp_uart_status_t BSP_UART_Init(void);

/**
 * @brief 绑定设备层缓冲与回调并启动接收
 * @param uart 总线句柄
 * @param port 端口绑定
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        接收已启动
 * @retval BSP_UART_ERR_PARAM 绑定参数非法
 * @retval BSP_UART_ERR_STATE 尚未初始化或外设状态不允许
 * @retval BSP_UART_ERR_HAL   可用档位全部启动失败
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart, const bsp_uart_port_t *port);

/**
 * @brief 读取已提交的累计接收字节数
 * @param uart 总线句柄
 * @return uint32_t
 * @retval 0 至 UINT32_MAX 累计字节数；句柄无效时为 0
 *
 * @note DMA 档只计入完整节点，节点长度为端口绑定的 dma_node；空闲事件仅唤醒服务任务。
 */
uint32_t BSP_UART_RxTotal(const bsp_uart_t *uart);

/**
 * @brief 查询接收是否已启动
 * @param uart 总线句柄
 * @return uint8_t
 * @retval 1 UART 已就绪
 * @retval 0 句柄无效或 UART 未就绪
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart);

/**
 * @brief 推进接收输入；阻塞档轮询，其它档由硬件异步更新
 * @param uart 总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK 接收推进完成
 * @retval BSP_UART_ERR_PARAM 参数为空
 * @retval BSP_UART_ERR_STATE 接收尚未启动
 */
bsp_uart_status_t BSP_UART_RxService(const bsp_uart_t *uart);

/**
 * @brief 请求发送 FIFO 继续外发
 * @param uart 总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK          已启动或队列为空
 * @retval BSP_UART_ERR_PARAM   参数为空
 * @retval BSP_UART_ERR_STATE   尚未启动或绑定不完整
 * @retval BSP_UART_ERR_TIMEOUT 阻塞发送超时，未归还该段
 * @retval BSP_UART_ERR_HAL     HAL 启动失败，未归还该段
 *
 * @note 任务上下文调用；异步档只启动，不等待。DMA 错误回调中，HAL 已结束 TX 状态，
 *       此时归还当前整段（可能已部分发送）；接收错误不推进发送队列。DMA 中断必须及时处理，
 *       不得屏蔽超过一个节点接收时长，以免 HT/TC 标志合并导致累计计数低估。
 */
bsp_uart_status_t BSP_UART_TxKick(const bsp_uart_t *uart);

/**
 * @brief 查询发送是否完全空闲
 * @param uart 总线句柄
 * @return uint8_t
 * @retval 1 句柄无效或发送已空闲
 * @retval 0 发送仍在进行
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart);

/**
 * @brief 中止 UART 收发并归还在途发送段
 * @param uart 总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK 中止成功
 * @retval BSP_UART_ERR_PARAM 句柄为空
 * @retval BSP_UART_ERR_HAL HAL 中止失败
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart);

/**
 * @brief 原子读取并清零累计硬件错误位
 * @param uart 总线句柄
 * @param out 错误位输出地址；为空时函数不操作
 * @retval 无
 *
 * @note 句柄无效或 out 为空时不操作；正常返回时 out 收到此前累计的错误位。
 */
void BSP_UART_Errors(const bsp_uart_t *uart, uint32_t *out);

/**
 * @brief 查询收发当前生效档位
 * @param uart 总线句柄
 * @param tx_mode 发送档位输出，可为空
 * @param rx_mode 接收档位输出，可为空
 * @retval 无
 *
 * @note 句柄无效时不操作；非空输出指针分别收到当前发送和接收档位。
 */
void BSP_UART_Modes(const bsp_uart_t *uart, uint8_t *tx_mode, uint8_t *rx_mode);

#endif /* __BSP_UART_H__ */
