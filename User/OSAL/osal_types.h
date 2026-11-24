/**
 * @file osal_types.h
 * @brief 文件2：注册点的类型入口（RTOS 外用）——建对象要的元素类型只从这里取
 * @author Ricardo
 * @date 2026-11-24
 *
 * @note 两个头文件按内外分工，互不重叠：
 *
 *       本文件对"RTOS 外"——不调 OS 原语、只把对象注册进去的地方。本工程是
 *       STM32CubeMX 模板，注册点是生成文件 Src/freertos.c：
 *       KEY_EVT_Q 要 sizeof(key_event_msg_t)，UART_EVT_Q 要 sizeof(dev_uart_t *)。
 *       两个类型分别定义在 Service 的 svc_key_logic.h 与 Device 的 dev_uart.h，
 *       由本文件统一转出——生成文件每次被覆盖后，注册点只留一行 include。
 *       像 ESP-IDF 那样不自动注册对象的模板，注册点是 User/OSAL/Objects 里的
 *       创建代码，同样只包含本文件。
 *
 *       Src/freertos.c 不需要 osal.h：OS 头它自己带、句柄它自己定义，它缺的
 *       只有元素类型；反过来，跑在 OS 上的模块也不需要本文件——它们的句柄在
 *       osal.h 里，元素类型与它们无关。
 *
 *       分层上不吃亏：本文件只被 RTOS 外的注册点包含，osal.h 与 OSAL 自己的
 *       .c 都不包含它，所以不会出现 OSAL → Service/Device 的依赖边；反过来，
 *       元素类型属于业务层，也不该复制一份到 OSAL 里定义。
 *
 *       现有对象对照，新增队列时同步补一行包含，不要在注册点另写 include：
 *       KEY_EVT_Q  元素 key_event_msg_t，见 svc_key_logic.h
 *       UART_EVT_Q 元素 dev_uart_t *  ，见 dev_uart.h
 */

#ifndef __OSAL_TYPES_H__
#define __OSAL_TYPES_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 功能包含
// 队列元素类型：key_event_msg_t
#include "svc_key_logic.h"
// 队列元素类型：dev_uart_t（UART_EVT_Q 存的是指针）
#include "dev_uart.h"

#endif /* __OSAL_TYPES_H__ */
