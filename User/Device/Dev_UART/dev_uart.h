/**
 * @file dev_uart.h
 * @brief UART 虚拟外设：写入串行化、超时等待、发完判定
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 本层只导出设备句柄：总线对象定义在 .c 里，不出头文件、不写 extern。
 *       UART 是点对点链路，没有"一条总线挂多台设备"的寻址概念，所以这里
 *       只有设备句柄一个名字——每个物理口就是一个设备。
 *
 *       本层负责"谁现在用这个口、写满时等多久、发完没有、超时怎么办"；
 *       芯片侧的 DMA 启动、乒乓接收、FIFO 搬运、错误重启都在 bsp_uart。
 *       换芯片只改 bsp_uart 的实现，本层的接口不变。
 *
 *       读侧是单消费者：同一口只允许一个任务调用 Read / Peek / Available，
 *       多任务消费要在更上层汇聚。写侧多任务可并发，本层用互斥量排队，
 *       一次 Write 的字节在发送队列里连续，不会被另一个任务插进去。
 */

#ifndef __DEV_UART_H__
#define __DEV_UART_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief DEV_UART 返回码
 * @note 与 bsp_uart_status_t 的语义一一对应；上层只认这一套。
 */
typedef enum {
    DEV_UART_OK          = 0,  /* 成功 */
    DEV_UART_ERR_PARAM   = -1, /* 参数非法，或句柄不在表里 */
    DEV_UART_ERR_STATE   = -2, /* 未初始化，或 bsp_uart 未就绪 */
    DEV_UART_ERR_TIMEOUT = -3, /* 等总线锁、等发送空间、或等发完超时 */
    DEV_UART_ERR_HAL     = -4, /* HAL 启动或重启接收失败 */
} dev_uart_status_t;

/**
 * @brief UART 设备句柄（不透明，定义在 dev_uart.c）
 * @note 一个实例对应一个物理口：bsp 句柄、互斥量、运行块。
 */
typedef struct dev_uart dev_uart_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/**
 * @brief USART1 对应的设备句柄
 * @note 帧格式与波特率由 CubeMX 决定，不在句柄里重复登记。
 */
extern const dev_uart_t dev_uart1;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化本层：检查互斥量并探测 bsp_uart 是否就绪
 * @retval DEV_UART_OK: 全部实例就绪
 * @retval DEV_UART_ERR_STATE: 缺 bsp 句柄、互斥量尚未创建，或 bsp_uart 未就绪
 * @note 必须在 BSP_UART_Init() 之后、USER CODE BEGIN RTOS_MUTEX 内调用。
 *       探测走 BSP_UART_IsReady() 这个公开接口，不去读 BSP 句柄内部字段。
 */
dev_uart_status_t Dev_UART_Init(void);

/**
 * @brief 查询设备是否可用
 * @param dev 设备句柄，可为 NULL
 * @retval 1: 已初始化且 bsp_uart 就绪
 * @retval 0: 否
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev);

/**
 * @brief 查询接收 FIFO 中已收未读的字节数
 * @param dev 设备句柄，可为 NULL
 * @retval 可读字节数，0 表示没有数据或句柄非法
 * @note 单消费者语义：同一口只允许一个任务调用。
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev);

/**
 * @brief 窥视接收数据，不取走
 * @param dev 设备句柄，可为 NULL
 * @param buf 接收缓冲区，可为 NULL（此时只返回可窥视长度）
 * @param len 最多看多少字节，不能为 0
 * @retval 实际看到的字节数
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, uint8_t *buf, uint16_t len);

/**
 * @brief 读取数据：等"够 len 字节"或超时，再取走当前全部可读字节
 * @param dev        设备句柄，可为 NULL
 * @param buf        接收缓冲区，不可为 NULL
 * @param len        期望字节数，不能为 0
 * @param timeout_ms 最多等多少毫秒；0 表示不等，取当前可读的部分
 * @retval 实际取到的字节数（≤ len），0 表示超时且没有数据
 * @note 返回时可能不足 len：超时、或对端只发来这么多。要"整包处理"的
 *       调用者应先用 Peek 看长度字段，再决定是否 Read。
 *       会推进读索引。单消费者语义：同一口只允许一个任务调用。
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint8_t *buf, uint16_t len,
                       uint32_t timeout_ms);

/**
 * @brief 写入数据：等发送队列腾出空间，最多等到超时
 * @param dev        设备句柄，可为 NULL
 * @param buf        待发送数据，不可为 NULL
 * @param len        请求发送的字节数，不能为 0
 * @param timeout_ms 最多等多少毫秒；0 表示立刻返回（能排多少排多少）
 * @retval 实际排入队列的字节数（≤ len）
 * @note 多任务可并发调用：本层用互斥量排队，一次调用排入的字节在队列里
 *       连续，不会被另一个任务插入——"一帧一次 Write"即帧级原子。
 *       排入即返回，不代表已发到线上；要确认发完用 Dev_UART_Flush()。
 *       会短暂阻塞等锁，不得在中断中调用。
 */
uint16_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *buf, uint16_t len,
                        uint32_t timeout_ms);

/**
 * @brief 等待发送队列排空且最后一个字节移出移位寄存器
 * @param dev        设备句柄，可为 NULL
 * @param timeout_ms 超时预算（毫秒），0 表示用默认 100 ms
 * @retval DEV_UART_OK: 已发完
 * @retval DEV_UART_ERR_PARAM: 句柄非法
 * @retval DEV_UART_ERR_STATE: 未初始化或 bsp_uart 未就绪
 * @retval DEV_UART_ERR_TIMEOUT: 等锁或等发完超时
 * @note 每 1 ms 让出一次 CPU，不要在中断里调用。返回时对端已收到最后
 *       一个字节的停止位。
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms);

/**
 * @brief 查询发送 FIFO 剩余可写空间
 * @param dev 设备句柄，可为 NULL
 * @retval 还能排入多少字节
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev);

/**
 * @brief 读取接收溢出计数
 * @param dev 设备句柄，可为 NULL
 * @retval 上电以来因 FIFO 满而丢弃的字节数
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev);

/**
 * @brief 清零接收溢出计数
 * @param dev 设备句柄，可为 NULL
 * @retval 无
 */
void Dev_UART_RxClearOverflow(const dev_uart_t *dev);

#endif /* __DEV_UART_H__ */
