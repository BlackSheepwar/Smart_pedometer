/**
 * @file    bsp_uart.h
 * @brief   串口芯片层：能力探测、三档搬运、收发接口。
 * @author  Ricardo
 * @date    2026-11-21
 *
 * 本层只做芯片机制，不做缓存、不加锁、不等待、不含任何 OS 调用：
 *  - 接收：把字节写进上层给的缓冲，只维护"收了多少字节"的绝对计数；
 *  - 发送：向上层要一段连续数据（指针 + 长度），搬完再告诉上层"这段用完了"。
 *
 * 缓冲、队列、溢出记账、超时、串行化都在设备层，所以换一颗芯片只重写本文件，
 * 设备层与业务代码不动。
 *
 * 三种搬运档位由 BSP_UART_Init() 从 HAL 句柄探测（挂没挂 DMA 流、方向是否可用），
 * 启动失败时依次向下退档：DMA -> 中断 -> 阻塞。探测结果只存在于运行块里。
 *
 * 中断里只做"记位置 / 记完成 / 交给 HAL 启动下一段"，不搬字节：
 *  - 接收 DMA：中断里只累计绝对计数，字节由 DMA 直接写进缓冲；
 *  - 发送：完成中断里只把上一段还给上层、再取下一段的指针；
 *  - 阻塞档不产生中断。
 */

#ifndef __BSP_UART_H__
#define __BSP_UART_H__

#include <stdint.h>

/*==============================================================================
 * 搬运档位
 *
 * 档位数值由低到高表示能力从弱到强；启动失败时只能向下退，不能向上跳。
 * 探测只发生在 BSP_UART_Init()：看方向是否允许、看有没有挂 DMA 流。
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
 * 返回码
 *
 * 数值即契约：已经发布的数值不再重排，新增错误一律往后加。
 *============================================================================*/

/**
 * @brief 串口芯片层返回码
 * @note 数值即契约：已经发布的数值不再重排，新增错误一律往后加。
 */
typedef enum {
    BSP_UART_OK          = 0,  /* 成功；异步方式下表示已经交给外设 */
    BSP_UART_ERR_PARAM   = -1, /* 参数非法：空指针、长度越界、缓冲不合规 */
    BSP_UART_ERR_STATE   = -2, /* 未初始化、上一笔还没收尾、外设忙 */
    BSP_UART_ERR_TIMEOUT = -3, /* 阻塞档等待外设超时 */
    BSP_UART_ERR_HAL     = -4, /* HAL 返回失败，或外设进入错误态 */
} bsp_uart_status_t;

/*==============================================================================
 * 类型定义
 *============================================================================*/

/**
 * @brief 串口总线句柄（不透明：布局在 bsp_uart.c）
 */
typedef struct bsp_uart bsp_uart_t;

/**
 * @brief 发送侧取数回调：把队首一段连续数据交给芯片层
 * @param ctx        上层注册的上下文（设备层运行块）
 * @param data       输出：这段数据的起始地址；没有数据时写 NULL
 * @retval >0        这段可用的字节数（必须与 data 指向的连续区间一致）
 * @retval 0         当前没有待发数据
 *
 * @note 只在任务上下文或发送完成中断里调用；实现方必须是纯索引运算，
 *       不得在其中阻塞、加锁或调用芯片层接口。
 */
typedef uint16_t (*bsp_uart_tx_acquire_t)(void *ctx, const uint8_t **data);

/**
 * @brief 发送侧归还回调：告诉上层"这段已经搬完，可以丢掉"
 * @param ctx        上层注册的上下文（设备层运行块）
 * @param len        已搬完的字节数
 *
 * @note 与 bsp_uart_tx_acquire_t 成对使用；只有这一对接口决定发送队列的推进。
 */
typedef void (*bsp_uart_tx_release_t)(void *ctx, uint16_t len);

/**
 * @brief 发送侧取数 / 归还回调集合
 */
typedef struct {
    bsp_uart_tx_acquire_t acquire;   /* 取一段连续数据；不能为空 */
    bsp_uart_tx_release_t release;   /* 归还已搬完的字节；不能为空 */
    void                 *ctx;       /* 透传给上面两个回调 */
} bsp_uart_tx_ops_t;

/*==============================================================================
 * 外部声明
 *============================================================================*/

/**
 * @brief USART1 的总线句柄
 *
 * @note 布局与运行块都在 bsp_uart.c；句柄本身只登记"用哪个外设"，
 *       缓冲与回调由 BSP_UART_Start() 在运行时登记。
 */
extern const bsp_uart_t bsp_uart1;

/*==============================================================================
 * 函数声明
 *============================================================================*/

/**
 * @brief 初始化串口芯片层：探测两路搬运档位
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        探测完成，可以把外设交给 BSP_UART_Start()
 * @retval BSP_UART_ERR_PARAM HAL 句柄为空
 * @retval BSP_UART_ERR_STATE 外设不在 READY，或被别的模块占用
 * @retval BSP_UART_ERR_HAL   接收 DMA 流存在但重新配置失败
 *
 * @note 探测只看两件事：方向的允许位、hdmatx/hdmarx 是否为 NULL。
 *       重复调用只刷新探测结果，不会动正在跑的传输。
 */
bsp_uart_status_t BSP_UART_Init(void);

/**
 * @brief 启动接收，并登记发送侧回调
 * @param uart      串口总线句柄
 * @param rx_buf    上层的接收缓冲；DMA 模式下 DMA 会直接写这块内存
 * @param rx_size   接收缓冲字节数，必须是 2 的幂且不小于 16
 * @param tx_ops    发送侧取数 / 归还回调；不能为空
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        接收已武装，发送回调已登记
 * @retval BSP_UART_ERR_PARAM 指针为空、缓冲不合规、回调为空
 * @retval BSP_UART_ERR_STATE 未初始化、已经在跑、或外设忙
 * @retval BSP_UART_ERR_HAL   三档都启动失败
 *
 * @note 接收档位从 UART 掉到阻塞时只在探测结果上退化，调用者只需看返回值。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 uint8_t *rx_buf,
                                 uint16_t rx_size,
                                 const bsp_uart_tx_ops_t *tx_ops);

/**
 * @brief 查询接收到的绝对字节数
 * @param uart      串口总线句柄
 * @return uint32_t 从上电起累计收到的字节数（自然回绕）
 *
 * @note 数值只增不减，缓冲下标按"计数 & (rx_size - 1)"取。
 *       阻塞档下由 BSP_UART_RxPoll() 推进；DMA 与中断档由芯片自己推进。
 */
uint32_t BSP_UART_RxCount(const bsp_uart_t *uart);

/**
 * @brief 查询接收是否就绪
 * @param uart      串口总线句柄
 * @return uint8_t
 * @retval 1        接收已经武装、可以取数
 * @retval 0        未初始化、启动失败或已被中止
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart);

/**
 * @brief 把接收缓冲里的字节搬进外设（仅阻塞档使用）
 * @param uart      串口总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        已把当前可读的字节全部搬完
 * @retval BSP_UART_ERR_PARAM uart 为空
 * @retval BSP_UART_ERR_STATE 未启动
 *
 * @note DMA 与中断档下调用是本层空操作：数据由芯片自己写进缓冲。
 *       阻塞档下一次最多搬 32 字节，逐字节轮询，不做等待。
 */
bsp_uart_status_t BSP_UART_RxPoll(const bsp_uart_t *uart);

/**
 * @brief 请求发送队列继续外发
 * @param uart      串口总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        已有数据在途，或队列已空
 * @retval BSP_UART_ERR_PARAM uart 为空
 * @retval BSP_UART_ERR_STATE 未启动，或外设暂时忙
 * @retval BSP_UART_ERR_TIMEOUT 阻塞档等待外设超时（数据仍在队列里）
 * @retval BSP_UART_ERR_HAL   三档都启动失败（数据仍在队列里）
 *
 * @note 阻塞档会一直搬到队列空才返回，返回即表示数据已经离开发送线；
 *       中断与 DMA 档只启动一段就返回，完成点看发送完成回调。
 */
bsp_uart_status_t BSP_UART_TxKick(const bsp_uart_t *uart);

/**
 * @brief 查询发送是否已经停稳
 * @param uart      串口总线句柄
 * @return uint8_t
 * @retval 1        没有在途数据，外设处于 READY
 * @retval 0        还有数据在途，或外设没回到 READY
 */
uint8_t BSP_UART_TxIdle(const bsp_uart_t *uart);

/**
 * @brief 中止收发并清掉就绪标记
 * @param uart      串口总线句柄
 * @return bsp_uart_status_t
 * @retval BSP_UART_OK        已中止
 * @retval BSP_UART_ERR_PARAM uart 为空
 * @retval BSP_UART_ERR_HAL   外设拒绝中止
 *
 * @note 已经交给 DMA 的发送数据不再归还，上层需要自己决定是否重发；
 *       接收计数保留，避免中止后把旧数据再交付一遍。
 */
bsp_uart_status_t BSP_UART_Abort(const bsp_uart_t *uart);

#endif /* __BSP_UART_H__ */
