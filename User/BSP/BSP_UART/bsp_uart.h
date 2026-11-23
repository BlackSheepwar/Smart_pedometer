/**
 * @file bsp_uart.h
 * @brief UART 芯片层：循环 DMA 收字节、窗口累计、分段 DMA 发送
 * @author Ricardo
 * @date 2026-11-23
 *
 * @note 本层不持有数据缓冲、不做 OS 同步，只回答"这颗芯片怎么搬字节"。
 *       接收用 circular DMA 搬 2*node 字节，HAL 在 HT/TC/IDLE 给出的 Size
 *       是自本次武装以来的窗口位置，本层用 (pos - last) & (2N - 1) 累计成
 *       绝对字节数，上层读这个数决定搬多少。发送把上层给的连续内存直接
 *       交给 DMA，完成点取 USART TC（最后一位离开发送器）。
 *       回调只回传本实例句柄，本层不保存任何上层指针。
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
/** @brief 芯片层返回码 */
typedef enum {
    BSP_UART_OK        =  0,  /* 操作成功 */
    BSP_UART_ERR_PARAM = -1,  /* 参数非法 */
    BSP_UART_ERR_STATE = -2,  /* 状态不允许 */
    BSP_UART_ERR_HAL   = -3,  /* HAL 操作失败 */
} bsp_uart_status_t;

/** @brief UART 总线句柄：不透明，布局在 bsp_uart.c */
typedef struct bsp_uart bsp_uart_t;

/**
 * @brief 接收事件通知：这一路有新字节或接收出错
 * @param uart 触发事件的实例
 * @retval 无
 *
 * @note 中断上下文调用；只允许置标志或发 ISR 安全通知，不搬运数据。
 */
typedef void (*bsp_uart_rx_fn)(const bsp_uart_t *uart);

/**
 * @brief 发送完成通知
 * @param uart 触发事件的实例
 * @param ok   1 = 最后一位已移出；0 = 错误中止，可能只发出一部分
 * @retval 无
 *
 * @note 中断上下文调用；只允许有界索引更新与 ISR 安全通知。
 */
typedef void (*bsp_uart_tx_done_fn)(const bsp_uart_t *uart, uint8_t ok);

/**
 * @brief 端口绑定：接收缓冲与两个通知都由上层提供
 *
 * @note 接收缓冲长度必须是 2*node，DMA 用 circular 模式，由 CubeMX 配置。
 */
typedef struct {
    uint8_t *buf;                    /** 接收缓冲，长度 2*node */
    uint16_t node;                   /** 半缓冲字节数：16 至 16384 的 2 次幂 */
    bsp_uart_rx_fn on_rx;            /** 接收事件 */
    bsp_uart_tx_done_fn on_tx_done;  /** 发送完成 */
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
 * @brief 绑定接收缓冲与通知，启动循环 DMA 接收
 * @param uart 总线句柄
 * @param port 端口绑定
 * @retval BSP_UART_OK: 接收已启动
 * @retval BSP_UART_ERR_PARAM: 缓冲、通知或节点长度非法
 * @retval BSP_UART_ERR_STATE: 接收 DMA 未配置成 circular
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 任务上下文调用一次；之后本层不再等待，进度只由中断累计。
 */
bsp_uart_status_t BSP_UART_Start(const bsp_uart_t *uart,
                                 const bsp_uart_port_t *port);

/**
 * @brief 读取自武装以来累计接收的绝对字节数
 * @param uart 总线句柄
 * @retval 0 至 UINT32_MAX 累计字节数
 *
 * @note 半满、全满、空闲三种事件都会推进该计数，不得假设它按节点对齐。
 */
uint32_t BSP_UART_RxTotal(const bsp_uart_t *uart);

/**
 * @brief 查询接收是否在武装状态
 * @param uart 总线句柄
 * @retval 1: 接收 DMA 在跑
 * @retval 0: 尚未启动或已被线路错误停止
 */
uint8_t BSP_UART_IsReady(const bsp_uart_t *uart);

/**
 * @brief 接收出错后重新武装循环 DMA
 * @param uart 总线句柄
 * @retval BSP_UART_OK: 已重新武装
 * @retval BSP_UART_ERR_STATE: 中止旧接收失败，或接收 DMA 不是 circular
 * @retval BSP_UART_ERR_HAL: HAL 重新武装失败
 *
 * @note 任务上下文调用。HAL 出错时通常已把接收收尾成 READY，这时不再中止，
 *       只有仍在收才先停。rx_total 保持连续，调用者必须以本次成功时的
 *       RxTotal() 为新基准重新对齐缓冲位置。
 */
bsp_uart_status_t BSP_UART_RxRearm(const bsp_uart_t *uart);

/**
 * @brief 启动一段 DMA 发送
 * @param uart 总线句柄
 * @param data 数据起点，回调前必须保持有效
 * @param len  字节数
 * @retval BSP_UART_OK: DMA 已启动
 * @retval BSP_UART_ERR_STATE: 已有一段在途
 * @retval BSP_UART_ERR_HAL: HAL 启动失败
 *
 * @note 任务上下文调用；完成点只由 on_tx_done 报告。
 */
bsp_uart_status_t BSP_UART_TxStart(const bsp_uart_t *uart,
                                   const uint8_t *data, uint16_t len);

#endif /* __BSP_UART_H__ */
