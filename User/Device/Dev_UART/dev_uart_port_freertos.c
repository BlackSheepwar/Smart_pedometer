/**
 * @file    dev_uart_port_freertos.c
 * @brief   UART 虚拟外设的 FreeRTOS 端口适配。
 * @author  Ricardo
 * @date    2026-11-22
 */

#include "FreeRTOS.h"
#include "stm32f4xx.h"
#include "dev_uart_port.h"
#include "task.h"
#include "cmsis_os.h"

/*==============================================================================
 * 类型定义与端口绑定
 *============================================================================*/

/**
 * @brief FreeRTOS UART 互斥端口句柄布局。
 */
struct dev_uart_port_lock
{
    osMutexId_t *mutex; /**< CubeMX 创建的 CMSIS 互斥量句柄地址 */
};

/**
 * @brief UART1 互斥量由 CubeMX 生成的 RTOS 初始化代码创建。
 */
extern osMutexId_t UART1_LOCKHandle;

/**
 * @brief UART 服务线程句柄，由服务线程启动时注册。
 */
static TaskHandle_t s_uart_service_task;

/**
 * @brief UART1 互斥端口对象。
 */
static struct dev_uart_port_lock s_uart1_port_lock =
{
    .mutex = &UART1_LOCKHandle
};

/**
 * @brief UART1 锁的可替换端口句柄。
 */
dev_uart_port_lock_t *UART1_LOCKHandle_port = &s_uart1_port_lock;

/**
 * @brief 将当前 FreeRTOS 线程注册为 UART 服务任务。
 * @retval 无
 */
static void Dev_UART_PortRegisterTaskImpl(void)
{
    s_uart_service_task = xTaskGetCurrentTaskHandle();
}

/**
 * @brief FreeRTOS UART 服务任务注册入口。
 */
dev_uart_port_register_fn Dev_UART_PortRegisterTask = Dev_UART_PortRegisterTaskImpl;

/*==============================================================================
 * 内部函数
 *============================================================================*/

/**
 * @brief 读取 FreeRTOS 系统节拍。
 * @return uint32_t
 * @retval 当前系统 tick 数
 */
static uint32_t Dev_UART_PortTicks(void)
{
    return osKernelGetTickCount();
}

/**
 * @brief 进入 FreeRTOS 端口短临界区。
 * @return uint32_t
 * @retval 进入前的中断屏蔽状态
 */
static uint32_t Dev_UART_PortCriticalEnter(void)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    return state;
}

/**
 * @brief 按进入前状态退出 FreeRTOS 端口短临界区。
 * @param state 进入前的中断屏蔽状态
 * @retval 无
 */
static void Dev_UART_PortCriticalExit(uint32_t state)
{
    if (state == 0U)
    {
        __enable_irq();
    }
}

/**
 * @brief 检查 UART 互斥量是否已经由 CubeMX 创建。
 * @param lock 不透明互斥句柄
 * @return uint8_t
 * @retval 1 互斥量可用
 * @retval 0 互斥量尚未创建或句柄无效
 */
static uint8_t Dev_UART_PortValid(dev_uart_port_lock_t *lock)
{
    return ((lock != NULL) && (lock->mutex != NULL) && (*lock->mutex != NULL)) ? 1U : 0U;
}

/**
 * @brief 在限定时间内获取 CubeMX 创建的 UART 互斥量。
 * @param lock 不透明互斥句柄
 * @param timeout_ms 等待上限，单位毫秒
 * @return uint8_t
 * @retval 1 成功获取互斥量
 * @retval 0 获取失败或等待超时
 */
static uint8_t Dev_UART_PortLock(dev_uart_port_lock_t *lock, uint32_t timeout_ms)
{
    if ((lock == NULL) || (lock->mutex == NULL) || (*lock->mutex == NULL))
    {
        return 0U;
    }
    return (osMutexAcquire(*lock->mutex, pdMS_TO_TICKS(timeout_ms)) == osOK) ? 1U : 0U;
}

/**
 * @brief 释放 UART 互斥量。
 * @param lock 不透明互斥句柄
 * @retval 无
 */
static void Dev_UART_PortUnlock(dev_uart_port_lock_t *lock)
{
    if ((lock != NULL) && (lock->mutex != NULL) && (*lock->mutex != NULL))
    {
        (void)osMutexRelease(*lock->mutex);
    }
}

/**
 * @brief 等待当前服务任务的直接通知。
 * @param timeout_ms 等待上限，单位毫秒
 * @return uint8_t
 * @retval 1 收到通知
 * @retval 0 等待超时
 */
static uint8_t Dev_UART_PortWait(uint32_t timeout_ms)
{
    return (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) != 0U) ? 1U : 0U;
}

/**
 * @brief 延时当前调用任务，不消费 UART 服务通知。
 * @param timeout_ms 延时时间，单位毫秒
 * @retval 无
 */
static void Dev_UART_PortDelay(uint32_t timeout_ms)
{
    (void)osDelay(timeout_ms);
}

/**
 * @brief 从中断上下文安全地通知 UART 服务任务。
 * @retval 无
 *
 * @note 服务任务尚未注册时不通知；其有限等待超时可恢复处理。
 */
static void Dev_UART_PortSignalIsr(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t task = s_uart_service_task;

    if (task != NULL)
    {
        vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/*==============================================================================
 * 端口操作表
 *============================================================================*/

/**
 * @brief FreeRTOS UART 端口同步操作表。
 */
const dev_uart_port_ops_t dev_uart_port_ops =
{
    .ticks = Dev_UART_PortTicks,
    .lock = Dev_UART_PortLock,
    .unlock = Dev_UART_PortUnlock,
    .wait = Dev_UART_PortWait,
    .delay = Dev_UART_PortDelay,
    .signal_isr = Dev_UART_PortSignalIsr,
    .critical_enter = Dev_UART_PortCriticalEnter,
    .critical_exit = Dev_UART_PortCriticalExit,
    .valid = Dev_UART_PortValid
};
