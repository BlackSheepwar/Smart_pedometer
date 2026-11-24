/**
 * @file osal.h
 * @brief 文件1：OS 声明兼容层（RTOS 内用）——包一层 OS 头，统一 extern 句柄
 * @author Ricardo
 * @date 2026-11-24
 *
 * @note 两个头文件按内外分工，互不重叠：
 *
 *       本文件对"RTOS 内"——已经跑在 OS 上、要调 OS 原语的代码：Device 的
 *       dev_iic / dev_spi / dev_uart、Service 的 svc_key_logic、TASK 的线程体。
 *       它们只从本文件拿"OS 能调什么"和"句柄叫什么"，模块里不再出现
 *       FreeRTOS.h 或 cmsis_os2.h。
 *
 *       对"RTOS 外"的文件不包含本文件：生成器维护的 Src/freertos.c 只负责
 *       注册对象，OS 头它自己带、句柄它自己定义，需要的只有元素类型——
 *       那是文件2 osal_types.h 的事。本工程 6 个业务文件含 osal.h，
 *       Src/freertos.c 只含 osal_types.h，两者无一交叉。
 *
 *       本文件只做包装与声明，不重新定义任何 OS 原语：上层照旧直接写
 *       osMutexAcquire / osMessageQueuePut / osDelayUntil /
 *       osKernelGetTickCount。包一层不是为了再抽象一次，而是把 OS 依赖
 *       关进一个文件：换 OS、换生成器、换句柄命名，只改这里。
 *
 *       句柄归属：本工程是 STM32CubeMX 模板，对象在 .ioc 的 Tasks01 /
 *       Queues01 / Mutexes01 里配好，由 Src/freertos.c 生成并创建，所有权
 *       留在生成文件里。本文件只声明句柄的名字（extern），不创建、不复制、
 *       不搬进 User/——CubeMX 重新生成只覆盖 Src/。
 *
 *       句柄声明在这里是刻意的例外：句柄不透明约定约束的是本工程自己定义的
 *       句柄；OS 句柄由 OS 定义，本文件只声明它们的地址，不碰内容。
 *
 *       注册对象要用的元素类型不在本文件：那是 osal_types.h（文件2）的事，
 *       只被 RTOS 外的注册点包含。
 */

#ifndef __OSAL_H__
#define __OSAL_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "cmsis_os2.h"

/*==============================================================================
 * 外部变量
 *============================================================================*/
/**
 * @brief 互斥量 IIC_LOCK 的句柄：I2C1 总线所有权
 * @note Dev_IIC 的每路总线各认一把，写 .ioc 的 Mutexes01。
 */
extern osMutexId_t IIC_LOCKHandle;

/**
 * @brief 互斥量 SPI1_LOCK 的句柄：SPI1 总线所有权
 * @note Dev_SPI 的一帧事务期间一直持有。
 */
extern osMutexId_t SPI1_LOCKHandle;

/**
 * @brief 互斥量 UART1_LOCK 的句柄：UART1 发送所有权
 * @note 只在整帧写进发送 FIFO 的那一段被持有。
 */
extern osMutexId_t UART1_LOCKHandle;

/**
 * @brief 队列 KEY_EVT_Q 的句柄：按键事件（元素 key_event_msg_t）
 * @note 生产者是 Svc_Key_Logic，消费者是 Svc_Key_Logic_Wait() 的调用者。
 */
extern osMessageQueueId_t KEY_EVT_QHandle;

/**
 * @brief 队列 UART_EVT_Q 的句柄：待服务的 UART 实例（元素 dev_uart_t*）
 * @note 生产者是 UART 接收中断，消费者是 Task_UART。
 */
extern osMessageQueueId_t UART_EVT_QHandle;

/**
 * @brief 线程 defaultTask 的句柄：CubeMX 默认建的那条空转线程
 * @note 本工程不用它干活，保留在 .ioc 里只为不改生成文件的结构。
 */
extern osThreadId_t defaultTaskHandle;

/**
 * @brief 线程 KEY_T 的句柄：按键扫描
 * @note 强定义在 TASK/task_key_scan.c，覆盖 freertos.c 的 __weak 入口。
 */
extern osThreadId_t KEY_THandle;

/**
 * @brief 线程 KEY_CMD_T 的句柄：按键命令分发
 * @note 强定义在 TASK/task_key_cmd.c。
 */
extern osThreadId_t KEY_CMD_THandle;

/**
 * @brief 线程 LVGL_T 的句柄：显示刷新
 * @note 强定义在 TASK/task_lvgl.c；它是全工程唯一调用 lv_ 接口的线程。
 */
extern osThreadId_t LVGL_THandle;

/**
 * @brief 线程 UART_T 的句柄：UART 接收事件服务
 * @note 强定义在 TASK/task_uart.c。
 */
extern osThreadId_t UART_THandle;

#endif /* __OSAL_H__ */
