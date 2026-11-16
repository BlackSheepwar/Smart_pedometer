/**
 * @file bsp_uart.h
 * @brief UART 硬件驱动层：DMA 乒乓接收 + 环形发送队列，收发全程不阻塞调用者
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 设计模型（两级缓冲，各解决一个问题）：
 *       1. 硬件级乒乓：DMA 以 Circular 模式循环写一整块接收缓冲，半传输与
 *          传输完成中断把它切成两个半区。DMA 写半区 A 时，半区 B 的内容
 *          已经稳定，可以安全拷走——这一级解决"边收边搬"的撕裂问题。
 *       2. 软件级 FIFO：半区稳定后立刻搬进环形 FIFO。上层何时取、取多少
 *          与数据到达节奏彻底解耦——这一级解决"上层来不及时数据被覆盖"。
 *          只留一级都不行：只有乒乓则上层必须在一个半区时间内消费完，
 *          只有 FIFO 则中断里要逐字节搬运，短帧还得靠轮询发现结尾。
 *       3. IDLE 中断补齐尾巴：短帧（常见 6~16 字节）填不满半区，靠 IDLE
 *          把"半区内已收到的那一段"提前发布，延迟从半个半区降到
 *          1 个字符时间。
 *       4. 发送侧是 FIFO + 分段 DMA：环形 buffer 回卷处物理不连续，一次
 *          DMA 只能发一段，发送完成中断接着发下一段。BSP_UART_Write() 只
 *          做 memcpy 入队，从不等待硬件。
 *
 *       本层不做帧解析、不注册上层回调、不建线程：帧边界属于 Service，
 *       调度节奏属于 TASK。
 *
 *       句柄表驱动：本层代码与 UART 路数无关，每路各有一套缓冲与锁。
 */

#ifndef __BSP_UART_H__
#define __BSP_UART_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief BSP_UART 返回码
 */
typedef enum {
    BSP_UART_OK          = 0,  /* 成功 */
    BSP_UART_ERR_PARAM   = -1, /* 参数非法 */
    BSP_UART_ERR_STATE   = -2, /* 未初始化 / 锁未创建 / 外设状态不对 */
    BSP_UART_ERR_TIMEOUT = -3, /* 等锁超时，或 Flush 超时 */
    BSP_UART_ERR_HAL     = -4, /* HAL 启动或重启接收失败 */
} bsp_uart_status_t;

/**
 * @brief UART 句柄（不透明类型，布局定义在 bsp_uart.c）
 * @note 只描述"哪个 HAL 句柄、哪把锁、缓冲多大"，不含引脚号与波特率；
 *       引脚与帧格式由 CubeMX 的 MX_UARTx_Init() 决定。
 */
typedef struct bsp_uart bsp_uart_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/**
 * @brief USART1：收发均走 DMA，帧格式与波特率由 CubeMX 决定
 * @note 与 USART2 各自独立：两套缓冲、各一把锁，一口的发送排队不会挡住另一口。
 *       上层不取数据时接收 FIFO 只进不出，超容量后按设计丢弃并累加
 *       rx_overflow——这是"接口先留好、数据源后接"的正常状态，不是故障。
 */
extern const bsp_uart_t bsp_uart1;

/**
 * @brief USART2：收发均走 DMA，帧格式与波特率由 CubeMX 决定
 */
extern const bsp_uart_t bsp_uart2;

/*==============================================================================
 * 函数声明
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
bsp_uart_status_t BSP_UART_Init(void);

/**
 * @brief 查询驱动是否已就绪
 * @param uart UART 句柄，不可为 NULL
 * @retval 1: 已初始化且接收 DMA 在跑
 * @retval 0: 未初始化、接收已停或句柄非法
 */
uint8_t BSP_UART_Is_Ready(const bsp_uart_t *uart);

/**
 * @brief 查询接收 FIFO 中已收未读的字节数
 * @param uart UART 句柄，不可为 NULL
 * @retval 可读字节数，最大为 FIFO 容量减一
 * @note 单消费者语义：只应由一个任务调用 Read/Available，多任务同时读会撕裂。
 */
uint16_t BSP_UART_Available(const bsp_uart_t *uart);

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
uint16_t BSP_UART_Peek(const bsp_uart_t *uart, uint8_t *buf, uint16_t len);

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
uint16_t BSP_UART_Read(const bsp_uart_t *uart, uint8_t *buf, uint16_t len);

/**
 * @brief 读取接收溢出计数
 * @param uart UART 句柄，不可为 NULL
 * @retval 上电以来因 FIFO 满而丢弃的字节数
 * @note 累加值，不回绕判断；上层可周期性比对增量来判断"是不是读得太慢"。
 */
uint32_t BSP_UART_Rx_Overflow(const bsp_uart_t *uart);

/**
 * @brief 清零接收溢出计数
 * @param uart UART 句柄，不可为 NULL
 * @retval 无
 */
void BSP_UART_Rx_Clear_Overflow(const bsp_uart_t *uart);

/**
 * @brief 查询发送 FIFO 剩余可写空间
 * @param uart UART 句柄，不可为 NULL
 * @retval 还能排入多少字节
 * @note 并发下数值立刻会变，只适合判断"大概够不够"；要精确就用 Write 的返回值。
 */
uint16_t BSP_UART_Tx_Free(const bsp_uart_t *uart);

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
uint16_t BSP_UART_Write(const bsp_uart_t *uart, const uint8_t *buf, uint16_t len);

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
bsp_uart_status_t BSP_UART_Flush(const bsp_uart_t *uart, uint32_t timeout_ms);

#endif /* __BSP_UART_H__ */
