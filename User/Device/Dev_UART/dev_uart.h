/**
 * @file dev_uart.h
 * @brief UART 虚拟外设：循环 DMA 收字节、双向 FIFO 与任务侧搬运
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 数据流：USART RX → 循环 DMA 缓冲（本层持有）→ 中断只发布累计
 *       字节数 → 服务任务把新增字节从 DMA 缓冲搬进接收 FIFO → 应用任务
 *       从接收 FIFO 取数。发送方向反过来：写者把整帧放进发送 FIFO，服务
 *       任务把队首连续段交给 BSP 的 DMA，完成回调用 USART TC（最后一位
 *       离开发送器）确认后归还。
 *
 *       并发契约：Dev_UART_Service()、Dev_UART_Wait() 只由同一个 UART
 *       服务任务调用；接收 FIFO 只允许一个消费者，Peek 返回的指针在对应
 *       字节被 Read() 消费前有效；发送 FIFO 允许多个写者，由 FreeRTOS
 *       互斥量串行化整帧写入（OSAL 重建后改由端口注入）；回调运行于中断
 *       上下文，只发送 ISR 安全通知，不搬运、不加锁、不等待。
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
 * @brief UART 设备层状态码
 * @note 数值即接口契约，已发布值不得重排。
 */
typedef enum {
    DEV_UART_OK          =  0, /* 操作成功 */
    DEV_UART_ERR_PARAM   = -1, /* 参数非法 */
    DEV_UART_ERR_STATE   = -2, /* 状态不允许 */
    DEV_UART_ERR_TIMEOUT = -3, /* 操作超时 */
    DEV_UART_ERR_HAL     = -4  /* 芯片层操作失败 */
} dev_uart_status_t;

/**
 * @brief UART 虚拟外设句柄；布局定义于 dev_uart.c
 */
typedef struct dev_uart dev_uart_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/** @brief UART1 虚拟外设配置句柄 */
extern const dev_uart_t dev_uart1;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 注册当前任务为 UART 服务任务
 * @retval 无
 *
 * @note 在 UART 服务任务里调用一次；中断只向该任务发直接通知。未注册时
 *       通知被丢弃，服务退化为 Dev_UART_Wait() 的有限超时轮询。
 */
void Dev_UART_RegisterTask(void);

/**
 * @brief 初始化 UART1 虚拟外设
 * @retval DEV_UART_OK: 成功或已就绪
 * @retval DEV_UART_ERR_PARAM: 句柄、缓冲或互斥句柄非法
 * @retval DEV_UART_ERR_STATE: 芯片层接收启动被拒绝
 * @retval DEV_UART_ERR_TIMEOUT: 芯片层接收启动超时
 * @retval DEV_UART_ERR_HAL: 芯片层接收启动失败
 *
 * @note 任务上下文调用一次；把两个 FIFO 绑定到静态池并启动循环 DMA。
 */
dev_uart_status_t Dev_UART_Init(void);

/**
 * @brief 任务侧服务：搬运新增接收字节并推进发送
 * @param dev UART 句柄
 * @retval DEV_UART_OK: 本轮服务完成
 * @retval DEV_UART_ERR_PARAM: 句柄非法
 * @retval DEV_UART_ERR_STATE: UART 尚未初始化
 * @retval DEV_UART_ERR_TIMEOUT: 芯片层重新武装暂时失败
 * @retval DEV_UART_ERR_HAL: 芯片层重新武装失败
 *
 * @note 唯一允许推进接收搬运与发送启停的入口；中断只做发布与通知。
 *       接收错误后本函数会在任务上下文重新武装 DMA；错误发生到重装之间
 *       未被发布、仍留在 DMA 窗口里的残留字节会被丢弃，且不计入丢失统计
 *       （Dev_UART_RxOverflow() 只统计溢出与覆盖）。
 */
dev_uart_status_t Dev_UART_Service(const dev_uart_t *dev);

/**
 * @brief 等待 UART 服务任务收到事件
 * @param dev        UART 句柄
 * @param timeout_ms 最大等待时间，单位毫秒
 * @retval DEV_UART_OK: 收到通知
 * @retval DEV_UART_ERR_PARAM: 句柄非法
 * @retval DEV_UART_ERR_STATE: UART 尚未初始化
 * @retval DEV_UART_ERR_TIMEOUT: 等待超时
 *
 * @note 仅由注册为 UART 服务任务的线程调用；裸机主循环不调用此接口。
 */
dev_uart_status_t Dev_UART_Wait(const dev_uart_t *dev, uint32_t timeout_ms);

/**
 * @brief 查询 UART 是否就绪
 * @param dev UART 句柄
 * @retval 1: 设备层与芯片层接收都已就绪
 * @retval 0: 句柄无效、尚未初始化或接收已被错误停止
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev);

/**
 * @brief 查询接收 FIFO 可读字节数，不触发硬件轮询
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 未消费字节数；句柄无效时为 0
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev);

/**
 * @brief 查看接收 FIFO 队首连续数据，不消费数据
 * @param dev  UART 句柄
 * @param data 输出队首地址；无数据或参数非法时写入 NULL
 * @retval 0 至 FIFO 容量 连续可读字节数；参数非法时为 0
 *
 * @note 仅在对应数据被 Dev_UART_Read() 消费前有效；同一 FIFO 只允许一个消费者。
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, const uint8_t **data);

/**
 * @brief 消费接收 FIFO 队首字节
 * @param dev UART 句柄
 * @param len 请求消费数量
 * @retval 0 至请求长度 实际消费数量；句柄无效时为 0
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint16_t len);

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
 *
 * @note 多个写者由 UART 互斥量串行化；帧空间不足时不会部分入队。
 */
dev_uart_status_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *data,
                                 uint16_t len, uint32_t timeout_ms);

/**
 * @brief 等待发送 FIFO 和硬件发送状态全部空闲
 * @param dev        UART 句柄
 * @param timeout_ms 最大等待时间，单位毫秒；0 表示只查询一次
 * @retval DEV_UART_OK: 当前数据全部发送完成
 * @retval DEV_UART_ERR_PARAM: 句柄非法或互斥量尚未创建
 * @retval DEV_UART_ERR_STATE: UART 未就绪
 * @retval DEV_UART_ERR_TIMEOUT: 等待互斥量、发送完成或期限超时
 * @retval DEV_UART_ERR_HAL: 芯片层发送启动失败；队列数据保留以便后续重试
 *
 * @note 返回 OK 表示最后一位已离开发送器（USART TC），可直接切半双工方向。
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms);

/**
 * @brief 查询发送 FIFO 剩余空间
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 可写字节数；句柄无效时为 0
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev);

/**
 * @brief 查询 DMA 覆盖与接收 FIFO 丢弃的累计字节数
 * @param dev UART 句柄
 * @retval 0 至 UINT32_MAX 累计丢失字节数；句柄无效时为 0
 *
 * @note 两个口径：服务任务来不及搬走导致 DMA 环形窗口覆盖的旧字节，
 *       以及接收 FIFO 满时丢弃的新字节。重装接收时未发布的窗口残留不计入。
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev);

/**
 * @brief 清零 DMA 覆盖与接收 FIFO 丢弃计数
 * @param dev UART 句柄
 * @retval 无
 */
void Dev_UART_RxClearOverflow(const dev_uart_t *dev);

#endif /* __DEV_UART_H__ */
