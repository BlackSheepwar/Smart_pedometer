/**
 * @file dev_uart.h
 * @brief UART 虚拟外设：循环 DMA 接收、双向 FIFO、任务侧搬运
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 接收：中断把"有事件那一路"的句柄投进事件队列；服务任务取出句柄
 *       后把那一路 DMA 缓冲里的新增字节搬进接收 FIFO，应用再从 FIFO 取数。
 *
 *       发送：写者持互斥量把整帧写进发送 FIFO，没有在途段时顺手启动第一
 *       段；每段发完在 TC 中断里归还并原地续链下一段，整条链不经过任务，
 *       也不需要任何临界区（写者之间靠互斥量，写者与中断不并发）。
 *
 *       并发契约：全模块一条事件队列，只允许一条服务任务消费；接收 FIFO
 *       只允许一个消费者，Peek 的指针在对应字节被 Read() 前有效；发送
 *       FIFO 允许多个写者，由互斥量串行化整帧写入。
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
/** @brief 设备层返回码 */
typedef enum {
    DEV_UART_OK        =  0, /* 操作成功 */
    DEV_UART_ERR_PARAM = -1, /* 参数非法 */
    DEV_UART_ERR_STATE = -2, /* 状态不允许或空间不足 */
    DEV_UART_ERR_HAL   = -3, /* 芯片层操作失败 */
} dev_uart_status_t;

/** @brief UART 虚拟外设句柄：不透明，布局在 dev_uart.c */
typedef struct dev_uart dev_uart_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/** @brief UART1 虚拟外设句柄 */
extern const dev_uart_t dev_uart1;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化一路 UART 虚拟外设
 * @param dev 虚拟外设句柄
 * @retval DEV_UART_OK: 成功
 * @retval DEV_UART_ERR_STATE: 接收 DMA 未配置成 circular
 * @retval DEV_UART_ERR_HAL: 芯片层启动接收失败
 *
 * @note 任务上下文调用，每路一次。事件队列由 CubeMX 建立（UART_EVT_Q），
 *       调用前必须已存在，否则中断无处投递。
 */
dev_uart_status_t Dev_UART_Init(const dev_uart_t *dev);

/**
 * @brief 阻塞等待一路待服务的串口
 * @param dev 输出：待服务的实例
 * @retval 无
 *
 * @note 没有超时：串口闲着就睡着，来事件才醒。队列给谁就服务谁，
 *       因此一条任务能服务多路。
 */
void Dev_UART_Wait(const dev_uart_t **dev);

/**
 * @brief 服务一路串口：搬运新增接收字节、按需重装接收
 * @param dev UART 句柄
 * @retval DEV_UART_OK: 本轮完成
 * @retval DEV_UART_ERR_STATE: 芯片层重装接收被拒
 * @retval DEV_UART_ERR_HAL: 芯片层重装接收失败
 *
 * @note 只处理接收；发送整条链由写者和 TC 中断推进，不经过这里。接收
 *       错误后在这里重装 DMA；错误到重装之间留在窗口里的残留字节丢弃，
 *       不计入丢失。
 */
dev_uart_status_t Dev_UART_Service(const dev_uart_t *dev);

/**
 * @brief 查询接收 FIFO 可读字节数
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 未消费字节数
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev);

/**
 * @brief 查看接收 FIFO 队首连续数据，不消费
 * @param dev  UART 句柄
 * @param data 输出队首地址
 * @retval 0 至 FIFO 容量 连续可读字节数
 *
 * @note 指针在对应字节被 Dev_UART_Read() 消费前有效。
 */
uint16_t Dev_UART_Peek(const dev_uart_t *dev, const uint8_t **data);

/**
 * @brief 消费接收 FIFO 队首字节
 * @param dev UART 句柄
 * @param len 请求消费数量
 * @retval 0 至请求长度 实际消费数量
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint16_t len);

/**
 * @brief 将完整数据帧写入发送 FIFO
 * @param dev        UART 句柄
 * @param data       数据起点
 * @param len        数据长度，1 至 FIFO 容量
 * @param timeout_ms 等互斥量的上限，单位毫秒
 * @retval DEV_UART_OK: 全部写入，发送已推进
 * @retval DEV_UART_ERR_PARAM: 长度非法
 * @retval DEV_UART_ERR_STATE: 空间不足（未写入）或互斥量等待超时
 * @retval DEV_UART_ERR_HAL: 芯片层启动发送失败；整帧仍在队列里
 *
 * @note 多个写者由互斥量串行化，帧不会部分入队。空间不足立即返回，
 *       设备层不睡眠——用 Dev_UART_TxFree() 自己决定重试节奏。
 */
dev_uart_status_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *data,
                                 uint16_t len, uint32_t timeout_ms);

/**
 * @brief 查询发送 FIFO 剩余空间
 * @param dev UART 句柄
 * @retval 0 至 FIFO 容量 可写字节数
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev);

/**
 * @brief 查询 DMA 覆盖与接收 FIFO 丢弃的累计字节数
 * @param dev UART 句柄
 * @retval 0 至 UINT32_MAX 累计丢失字节数
 *
 * @note 两个口径：服务任务来不及搬导致 DMA 环形窗口覆盖的旧字节，以及
 *       接收 FIFO 满时丢弃的新字节；重装接收时的窗口残留不计入。
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev);

/**
 * @brief 查询发送失败的累计段数
 * @param dev UART 句柄
 * @retval 0 至 UINT32_MAX 因错误中止的发送段数
 *
 * @note 失败段可能只发出一部分，帧完整性由协议层自行处理。
 */
uint32_t Dev_UART_TxErrors(const dev_uart_t *dev);

#endif /* __DEV_UART_H__ */
