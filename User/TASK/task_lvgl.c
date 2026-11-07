/**
 * @file task_lvgl.c
 * @brief 任务层：LVGL 显示线程体（等切换通知 → 切屏 → 推进渲染）
 * @author Ricardo
 * @date 2026-11-07
 *
 * @note 分工：
 *         - Svc_LVGL      管"怎么把 LVGL 画到 ST7789 上"；
 *         - APP_Screen    管"有哪几屏、当前是哪一屏"；
 *         - 本文件        只回答"多久跑一次、什么时候切屏"。
 *       业务判断（B10 按下算切屏请求）在 APP_Key_Cmd，通知经显示服务的
 *       Svc_LVGL_Request_Switch() 发出，本线程只做接收与执行。
 *
 *       为什么用"带超时的 Acquire"而不是另起一个切屏线程：
 *       LVGL 不是线程安全的，切屏与 lv_timer_handler() 必须在同一线程。
 *       用 10 ms 超时等信号量，既能及时响应按键，又能保证渲染节拍不断。
 *
 *       毫秒时基不在这里：OS 节拍到毫秒的换算已移入 Svc_LVGL（本层
 *       不再持有任何"时间格式"的知识），本文件只回答"多久调一次 Step"。
 *
 *       线程由 CubeMX 按 .ioc 的 Tasks01 创建（LVGL_T），这里提供同名强定义，
 *       覆盖 Src/freertos.c 里生成的 __weak 空入口。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "cmsis_os2.h"
// 功能包含
#include "svc_lvgl.h"
#include "app_screen.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 等切换通知的超时（ms），同时也是 lv_timer_handler 的调用周期 */
#define TASK_LVGL_PERIOD_MS         10u
/** @brief 初始化失败后的空转节拍（ms）：不再调用任何 LVGL 接口 */
#define TASK_LVGL_FAULT_DELAY_MS    1000u

/*==============================================================================
 * 外部声明
 *============================================================================*/
/**
 * @brief 切换请求二值信号量：由 CubeMX 在 Src/freertos.c 里创建
 * @note 生产者是 Svc_LVGL_Request_Switch()（按键命令线程调用），消费者是本线程。
 */
extern osSemaphoreId_t LVGL_SW_SEMHandle;

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief LVGL 显示线程体
 * @param argument 未使用
 * @retval 无
 * @note 初始化失败时空转：面板没起来时继续调 LVGL 只会在 NULL display 上
 *       操作，比停在这里更难排查。失败原因看 Svc_LVGL_Init() 的返回值。
 */
void Task_LVGL(void *argument)
{

    (void)argument;

    /* ---------- 1. 显示服务先于界面：screen 需要挂到 display 上 ---------- */
    if (Svc_LVGL_Init() != 0) {
        for (;;) {
            (void)osDelay(TASK_LVGL_FAULT_DELAY_MS);
        }
    }
    APP_Screen_Init();

    /* ---------- 3. 等通知 → 切屏 → 推进渲染，三件事都在本线程 ---------- */
    for (;;) {
        if (osSemaphoreAcquire(LVGL_SW_SEMHandle, TASK_LVGL_PERIOD_MS) == osOK) {
            APP_Screen_Next();
        }

        /* 无论有没有按键都要推进：动画、重绘与屏幕首次上屏都靠它 */
        Svc_LVGL_Step();
    }
}
