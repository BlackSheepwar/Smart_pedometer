/**
 * @file    dev_uart.h
 * @brief   串口设备层：收发队列、溢出记账、串行化与超时。
 * @author  Ricardo
 * @date    2026-11-21
 *
 * 队列在本层，芯片层只搬字节：
 *  - 接收队列就是芯片层的 DMA 目标缓冲，本层只维护"读到哪里"；
 *  - 发送队列由本层写入，芯片层通过取数 / 归还回调直接读这块内存，不复制。
 *
 * 换芯片只改 bsp_uart.c；本层的等待、加锁与超时在裸机下只需替换成
 * 对应的任务 / 临界区实现，接口不变。
 */

#ifndef __DEV_UART_H__
#define __DEV_UART_H__

#include <stdint.h>

/*==============================================================================
 * 返回码
 *
 * 数值即契约：已经发布的数值不再重排，新增错误一律往后加。
 *============================================================================*/

/**
 * @brief 串口设备层返回码
 */
typedef enum {
    DEV_UART_OK          = 0,   /* 成功 */
    DEV_UART_ERR_PARAM   = -1,  /* 参数非法：空指针、长度为 0 */
    DEV_UART_ERR_STATE   = -2,  /* 状态不对：未初始化、句柄不匹配 */
    DEV_UART_ERR_TIMEOUT = -3,  /* 等待超时 */
    DEV_UART_ERR_HAL     = -4   /* 芯片层失败 */
} dev_uart_status_t;

/*==============================================================================
 * 类型定义
 *============================================================================*/

/**
 * @brief 串口设备句柄（不透明：布局在 dev_uart.c）
 */
typedef struct dev_uart dev_uart_t;

/*==============================================================================
 * 外部声明
 *============================================================================*/

/**
 * @brief USART1 的设备句柄
 *
 * @note 一个端口一个句柄：端口没有"挂在一条总线上"的寻址概念，
 *       所以只有设备句柄这一层，没有总线句柄。
 */
extern const dev_uart_t dev_uart1;

/*==============================================================================
 * 函数声明
 *============================================================================*/

/**
 * @brief 初始化设备层：登记队列、接管芯片层的收发缓冲
 * @return dev_uart_status_t
 * @retval DEV_UART_OK        队列与芯片层都已就绪
 * @retval DEV_UART_ERR_STATE 已经初始化过，或芯片层未初始化
 * @retval DEV_UART_ERR_HAL   芯片层启动失败
 *
 * @note 队列容量在 dev_uart.c 里，必须是 2 的幂且不小于 16。
 */
dev_uart_status_t Dev_UART_Init(void);

/**
 * @brief 查询端口是否可用
 * @param dev       设备句柄
 * @return uint8_t
 * @retval 1        已初始化且芯片层接收已武装
 * @retval 0        未初始化或芯片层已停
 */
uint8_t Dev_UART_IsReady(const dev_uart_t *dev);

/**
 * @brief 查询接收队列里可读的字节数
 * @param dev       设备句柄
 * @return uint16_t 可读字节数（不超过队列容量）
 *
 * @note 顺带做一次溢出记账：如果累计收到的字节比读指针超出一整圈，
 *       多出来的部分计入 Dev_UART_RxOverflow()，读指针跳到最旧的有效字节。
 */
uint16_t Dev_UART_Available(const dev_uart_t *dev);

/**
 * @brief 看一眼队首字节，不取走
 * @param dev       设备句柄
 * @param byte      输出：队首字节
 * @return uint8_t
 * @retval 1        取到了
 * @retval 0        队列为空或参数非法
 */
uint8_t Dev_UART_Peek(const dev_uart_t *dev, uint8_t *byte);

/**
 * @brief 读接收队列
 * @param dev       设备句柄
 * @param buf       目的缓冲
 * @param len       期望字节数
 * @param timeout_ms 等够 len 个字节的最长时间；0 表示不等待
 * @return uint16_t 实际读出的字节数（可能小于 len）
 *
 * @note 只有单消费者：同一端口不要在两个任务里同时读。
 *       阻塞档下等待期间会驱动芯片层轮询接收。
 */
uint16_t Dev_UART_Read(const dev_uart_t *dev, uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/**
 * @brief 写入发送队列并请求外发
 * @param dev       设备句柄
 * @param buf       源数据
 * @param len       字节数
 * @param timeout_ms 队列满时最多等多久；0 表示不等待
 * @return uint16_t 实际排入队列的字节数
 *
 * @note 本接口是"送进队列"而不是"已经发上线"：中断 / DMA 档返回时
 *       可能还有数据在途，需要等发完时再调用 Dev_UART_Flush()。
 */
uint16_t Dev_UART_Write(const dev_uart_t *dev, const uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/**
 * @brief 等待发送彻底停下
 * @param dev       设备句柄
 * @param timeout_ms 最长等待时间
 * @return dev_uart_status_t
 * @retval DEV_UART_OK         队列空且在途数据已发完
 * @retval DEV_UART_ERR_PARAM  dev 为空
 * @retval DEV_UART_ERR_STATE  未初始化
 * @retval DEV_UART_ERR_TIMEOUT 超时仍有数据
 *
 * @note 等待期间会持续请求芯片层外发，所以不会因为"上一次启动失败"而空等。
 */
dev_uart_status_t Dev_UART_Flush(const dev_uart_t *dev, uint32_t timeout_ms);

/**
 * @brief 查询发送队列剩余空间
 * @param dev       设备句柄
 * @return uint16_t 还能写入的字节数
 */
uint16_t Dev_UART_TxFree(const dev_uart_t *dev);

/**
 * @brief 读取接收溢出计数
 * @param dev       设备句柄
 * @return uint32_t 由于读得不够快而丢掉的累计字节数
 */
uint32_t Dev_UART_RxOverflow(const dev_uart_t *dev);

/**
 * @brief 把接收溢出计数清零
 * @param dev       设备句柄
 * @return dev_uart_status_t
 * @retval DEV_UART_OK        已清零
 * @retval DEV_UART_ERR_PARAM dev 为空
 */
dev_uart_status_t Dev_UART_RxClearOverflow(const dev_uart_t *dev);

#endif /* __DEV_UART_H__ */
