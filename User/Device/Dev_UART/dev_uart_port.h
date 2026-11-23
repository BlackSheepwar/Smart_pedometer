/**
 * @file    dev_uart_port.h
 * @brief   UART 虚拟外设的可替换同步端口契约。
 * @author  Ricardo
 * @date    2026-11-22
 */

#ifndef __DEV_UART_PORT_H__
#define __DEV_UART_PORT_H__

#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/

/**
 * @brief UART 互斥端口句柄；布局定义于端口实现文件。
 */
typedef struct dev_uart_port_lock dev_uart_port_lock_t;

/**
 * @brief 读取单调递增的系统节拍。
 * @retval 当前系统 tick
 */
typedef uint32_t (*dev_uart_port_ticks_fn)(void);

/**
 * @brief 在超时内获取互斥量。
 * @param lock 不透明互斥句柄
 * @param timeout_ms 等待上限，单位毫秒
 * @retval 1 成功获取
 * @retval 0 获取失败或超时
 */
typedef uint8_t (*dev_uart_port_lock_fn)(dev_uart_port_lock_t *lock,
                                         uint32_t timeout_ms);

/**
 * @brief 释放互斥量。
 * @param lock 不透明互斥句柄
 * @retval 无
 */
typedef void (*dev_uart_port_unlock_fn)(dev_uart_port_lock_t *lock);

/**
 * @brief 等待服务任务事件。
 * @param timeout_ms 等待上限，单位毫秒
 * @retval 1 收到事件
 * @retval 0 等待超时
 */
typedef uint8_t (*dev_uart_port_wait_fn)(uint32_t timeout_ms);

/**
 * @brief 将当前任务注册为 UART 服务任务。
 * @retval 无
 */
typedef void (*dev_uart_port_register_fn)(void);

/**
 * @brief 延时当前调用者。
 * @param timeout_ms 延时时间，单位毫秒
 * @retval 无
 */
typedef void (*dev_uart_port_delay_fn)(uint32_t timeout_ms);

/**
 * @brief 从中断上下文通知 UART 服务任务。
 * @retval 无
 */
typedef void (*dev_uart_port_signal_isr_fn)(void);

/**
 * @brief 进入短临界区并保存恢复状态。
 * @retval 0 端口定义的初始状态或已恢复状态；其它值由端口实现解释
 */
typedef uint32_t (*dev_uart_port_critical_enter_fn)(void);

/**
 * @brief 按进入时保存的状态退出短临界区。
 * @param state 进入时保存的端口状态
 * @retval 无
 */
typedef void (*dev_uart_port_critical_exit_fn)(uint32_t state);

/**
 * @brief 检查互斥端口句柄是否已经绑定。
 * @param lock 不透明互斥句柄
 * @retval 1 端口对象已绑定
 * @retval 0 端口对象无效
 */
typedef uint8_t (*dev_uart_port_valid_fn)(dev_uart_port_lock_t *lock);

/**
 * @brief UART 等待、互斥、时间与通知操作集合。
 */
typedef struct
{
    dev_uart_port_ticks_fn ticks; /**< 读取单调系统节拍 */
    dev_uart_port_lock_fn lock; /**< 任务态获取发送互斥量 */
    dev_uart_port_unlock_fn unlock; /**< 任务态释放发送互斥量 */
    dev_uart_port_wait_fn wait; /**< 服务任务等待 ISR 通知 */
    dev_uart_port_delay_fn delay; /**< 写者等待 FIFO 空间 */
    dev_uart_port_signal_isr_fn signal_isr; /**< ISR 安全通知服务任务 */
    dev_uart_port_critical_enter_fn critical_enter; /**< 进入短临界区 */
    dev_uart_port_critical_exit_fn critical_exit; /**< 恢复临界区状态 */
    dev_uart_port_valid_fn valid; /**< 验证互斥句柄绑定 */
} dev_uart_port_ops_t;

/*==============================================================================
 * 端口绑定
 *============================================================================*/

/**
 * @brief UART1 互斥端口对象。
 */
extern dev_uart_port_lock_t *UART1_LOCKHandle_port;

/**
 * @brief 当前端口的服务任务注册函数；无任务模型时可设为 NULL。
 */
extern dev_uart_port_register_fn Dev_UART_PortRegisterTask;

/**
 * @brief 当前平台实现的 UART 同步操作集合。
 */
extern const dev_uart_port_ops_t dev_uart_port_ops;

#endif
