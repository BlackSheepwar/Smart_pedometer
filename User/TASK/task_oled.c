/**
 * @file task_oled.c
 * @brief 任务层：OLED 显示线程体（周期刷新）
 * @author Ricardo
 * @date 2026-10-23
 *
 * @note 线程本身由 CubeMX 按 Smart_pedometer.ioc 的 Tasks01 创建，入口名为 Task_OLED，
 *       这里提供同名强定义，覆盖 freertos.c 里生成的 __weak 空入口。
 *
 *       刷新流程：OLED_NewFrame() 清显存 → 绘制 → OLED_ShowFrame() 整屏上屏。
 *       整屏约 1.1 KB I2C 数据，400 kHz 下约 25 ms，所以刷新周期取 100 ms，
 *       不要用 1 ms 的空转节拍。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include "FreeRTOS.h"
#include "cmsis_os2.h"
// 功能包含
#include "oled_extend.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 上电后等 OLED 内部复位完成再初始化（驱动文档要求 ≥ 20 ms） */
#define TASK_OLED_POWERUP_DELAY_MS  20u
/** @brief 整屏刷新周期（ms）：1.1 KB I2C 传输 + 绘制，不能按 1 ms 追 */
#define TASK_OLED_REFRESH_PERIOD_MS 100u

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief OLED 显示线程体
 * @param argument 未使用
 * @note OLED_Init() 必须在本线程里调用，不能放到 MX_FREERTOS_Init()：
 *       BSP_IIC_Transfer() 走的是"抢锁 → 启动 DMA → 睡在任务通知上等中断"，
 *       而 osKernelStart() 之前 pxCurrentTCB 还是 NULL，
 *       ulTaskNotifyTake() 一解引用就 HardFault（映像里表现为 PC13 快闪）。
 */
void Task_OLED(void *argument)
{
    int a = 0;

    (void)argument;

    osDelay(TASK_OLED_POWERUP_DELAY_MS);
    OLED_Init();

    for (;;) {
        OLED_NewFrame();
        OLED_PrintDecimal(0, 0, a, &afont16x8, OLED_COLOR_NORMAL);
        OLED_ShowFrame();

        a++;            // 每刷新一次，数字加 1
        osDelay(TASK_OLED_REFRESH_PERIOD_MS);
    }
}
