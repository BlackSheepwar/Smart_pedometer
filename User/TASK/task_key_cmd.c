/**
 * @file task_key_cmd.c
 * @brief 任务层：按键命令线程体（阻塞收事件 → 交给命令表）
 * @author Ricardo
 * @date 2026-10-23
 *
 * @note 这个线程和 Task_Key_Scan 的分工：
 *         Task_Key_Scan  只负责"按时采样"，从不阻塞；
 *         Task_Key_Cmd   睡在事件队列上，来一条处理一条。
 *       队列把两级解耦：业务 handler 慢一点，采样节拍不会跟着慢。
 *
 *       取事件的代码写在这里，不在按键服务里：服务只负责消抖和投递，
 *       一旦把"怎么取、等多久"塞进服务，服务就被绑在 RTOS 上了。
 *
 *       线程本身由 CubeMX 按 .ioc 的 Tasks01 创建，这里只提供同名强定义，
 *       覆盖 freertos.c 里生成的 __weak 空入口。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include "FreeRTOS.h"
#include "cmsis_os2.h"
// 功能包含
#include "app_key_cmd.h"
#include "dev_st7789.h"
#include "app_level.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/**
 * @brief 上电后等面板供电与内部复位完成的延时（ms）
 * @note 只是等电源稳定，不承担"让出 CPU"的职责：Dev_ST7789_Init() 内部
 *       的复位/上电等待已经改用 osDelay，会主动让出 CPU，期间 OLED 任务
 *       照常能跑。
 */
#define TASK_KEY_CMD_POWERUP_DELAY_MS   200u

/*==============================================================================
 * 外部声明
 *============================================================================*/
/**
 * @brief 事件队列句柄：由 CubeMX 按 Smart_pedometer.ioc 的 Queues01 在 Src/freertos.c 里创建
 * @note 深度 8、元素 key_event_msg_t、静态分配；生产者是按键服务，消费者是本线程。
 */
extern osMessageQueueId_t KEY_EVT_QHandle;

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 按键命令线程体
 * @param argument 未使用
 * @note 一直阻塞在队列上，被投递唤醒后执行一条命令；
 *       没有事件时线程不占 CPU（由内核挂起，直到队列非空）。
 */
void Task_Key_Cmd(void *argument)
{
    key_event_msg_t msg;

    (void)argument;
    APP_Level_Init();

    /* ---------- 1. 显示先起，再进事件循环 ---------- */
    osDelay(TASK_KEY_CMD_POWERUP_DELAY_MS);

    if (Dev_ST7789_Init(&dev_st7789_0) == 0) {
        /* 设备层不再替上层清屏，上电这一帧由应用层刷；这里借用按键模块
           的测试填充，避免显存随机噪声停在屏上 */
        (void)APP_Key_Cmd_TestFillScreen(0x0210u);
    }

    /* ---------- 2. 阻塞收事件，来一条处理一条 ---------- */
    for (;;) {
        if (osMessageQueueGet(KEY_EVT_QHandle, &msg, NULL, osWaitForever) == osOK) {
            APP_Key_Cmd_Packet(msg.key, msg.event);
        }
    }
}
