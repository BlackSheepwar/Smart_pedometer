/**
 * @file    task_uart.c
 * @brief   UART 接收搬运与发送推进任务入口。
 * @author  Ricardo
 * @date    2026-11-22
 */

#include <stddef.h>

#include "dev_uart.h"
#include "dev_uart_port.h"
#include "task_uart.h"

/*==============================================================================
 * 函数实现
 *============================================================================*/

/**
 * @brief 等待 UART 事件并服务接收与发送
 * @param argument RTOS 任务参数，未使用
 * @retval 无
 *
 * @note 超时唤醒用于兜底处理；裸机移植时由主循环周期调用
 *       Dev_UART_Service()，本任务入口不参与设备协议解析。
 */
void Task_UART(void *argument)
{
    (void)argument;
    if (Dev_UART_PortRegisterTask != NULL)
    {
        Dev_UART_PortRegisterTask();
    }

    for (;;)
    {
        (void)Dev_UART_Wait(&dev_uart1, 10U);
        (void)Dev_UART_Service(&dev_uart1);
    }
}
