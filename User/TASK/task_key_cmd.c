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
 *       本线程是"表"与"机制"的唯一接合点：表由 APP 导出
 *       （APP_KEY_CMD_TABLE），查表执行由 Svc_Key_Cmd 提供，两者在这里
 *       拼成一次调用。接合点只有一处，所以换表、换服务都不牵动彼此：
 *       APP 不知道谁在查表，服务不知道表里装的是谁的键。
 *
 *       屏幕不归本线程所有：ST7789 的初始化与所有绘制都在 Task_LVGL，
 *       本线程既不 Dev_ST7789_Init() 也不直接刷屏。B10 的动作只是把
 *       APP_Step 里的计数加一（改一个整数），界面刷新由 Task_LVGL
 *       每 10 ms 轮询完成，所以本线程只做"取事件、调分发"两件事——
 *       不碰 LVGL、不碰信号量，也不承担 UI 通知策略。
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
#include "app_key_cmd.h"        /* APP_KEY_CMD_TABLE：本工程的动作表 */
#include "svc_key_cmd.h"        /* Svc_Key_Cmd_Dispatch：查表机制 */
#include "svc_key_logic.h"      /* key_event_msg_t：队列元素类型 */

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
 * @retval 无
 * @note 一直阻塞在队列上，被投递唤醒后执行一条命令；
 *       没有事件时线程不占 CPU（由内核挂起，直到队列非空）。
 *       分发返回值（命中与否）本线程不关心：表里没列的组合按设计就是
 *       静默丢弃，不需要在这里分支。
 */
void Task_Key_Cmd(void *argument)
{
    key_event_msg_t msg;

    (void)argument;

    /* ---------- 阻塞收事件，来一条处理一条 ---------- */
    for (;;) {
        if (osMessageQueueGet(KEY_EVT_QHandle, &msg, NULL, osWaitForever) == osOK) {
            Svc_Key_Cmd_Dispatch(APP_KEY_CMD_TABLE, APP_KEY_CMD_COUNT,
                                 msg.key, msg.event);
        }
    }
}
