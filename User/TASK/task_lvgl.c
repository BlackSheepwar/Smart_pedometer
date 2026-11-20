/**
 * @file task_lvgl.c
 * @brief 任务层：LVGL 显示线程体（每个节拍同步数据 → 推进渲染）
 * @author Ricardo
 * @date 2026-11-07
 *
 * @note 分工：
 *         - Svc_LVGL      管"怎么把 LVGL 画到 ST7789 上"；
 *         - APP_Screen    管"界面什么时候建、当前显示哪块面板、每拍刷什么值"；
 *         - 本文件        只回答"多久跑一次、按什么顺序跑"。
 *       业务判断（PC13 只触发"换面板"）在 APP_Key_Cmd / APP_Screen，
 *       界面形态在 UI_Export（LVGL Pro 导出），
 *       数据同步与换面板落地在 APP_Screen；本线程只做
 *       "按节拍驱动"这一件事。
 *
 *       为什么节拍固定 10 ms 而不是事件驱动：
 *       LVGL 不是线程安全的，所有 lv_ 调用必须集中在同一个线程。
 *       本线程用 osDelay 定节拍：界面数据由本线程轮询（滞后一个节拍
 *       10 ms，人眼不可感知），没有"必须立刻上屏"的异步事件，所以不需要
 *       一条唤醒通道。
 *       将来若要低延迟唤醒，加信号量并把 osDelay 换成带超时的 Acquire，
 *       改动只在本文件与 .ioc，Svc_LVGL 一行不用动。
 *
 *       毫秒时基与毫秒延时都在本文件实现并注入 Svc_LVGL：Service 层因此
 *       不读时钟、也不提供延时手段，"换 OS 只改 TASK"这条约定对显示链路
 *       成立。时基函数里的 _Static_assert 把 configTICK_RATE_HZ == 1000
 *       锁在编译期，所以 osDelay() 的参数可以直接按毫秒数传，不再需要
 *       freq 折算。
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
 * 内部函数
 *============================================================================*/
/**
 * @brief LVGL 的毫秒时基来源
 * @retval 自内核启动起的毫秒数
 * @note lv_tick_get() 每被调用一次就回调本函数一次（不做任何缓存），
 *       一次 lv_timer_handler() 大约取 6~12 次时间，所以本函数必须廉价。
 *
 *       本工程 configTICK_RATE_HZ == 1000，1 tick 恰好等于 1 ms，因此
 *       不需要任何换算，直接把 tick 计数当毫秒返回。下面那条静态断言
 *       把这个前提锁在**编译期**：将来有人在 CubeMX 里把 tick 频率改成
 *       100 Hz 或 128 Hz，这里会编译失败，而不是悄悄让整条时间轴慢 10 倍
 *       ——后者表现为"动画卡顿、按键反应变慢"，几乎不可能反推
 *       到 tick 频率这个根因。
 *
 *       若确实需要支持任意 tick 频率，换算应写成：
 *           ms = (uint32_t)(((uint64_t)tick * 1000u) / freq)
 *       中间量必须用 uint64_t：tick × 1000 在 32 位下的溢出点是
 *       4 294 967 tick（1 kHz 下仅 71.6 分钟，不是 49 天），且必须
 *       先乘后除，先除会把整数部分丢光。代价是每次取时间都要跑一次
 *       64 位除法（__aeabi_uldivmod，约 100~200 周期），按每秒约
 *       1000 次估算约占 0.1% CPU，在 168 MHz 上不值得付。
 *
 *       osKernelGetTickCount() 内部带 IS_IRQ() 分支，任务与中断上下文
 *       都能安全调用。
 */
static uint32_t Task_LVGL_Tick_Ms(void)
{
    _Static_assert(configTICK_RATE_HZ == 1000,
                   "Task_LVGL_Tick_Ms 假设 1 tick = 1 ms；tick 频率变了必须改回换算写法");

    return osKernelGetTickCount();
}

/**
 * @brief 面板初始化时序的毫秒延时来源
 * @param ms 需要等待的毫秒数
 * @retval 无
 * @note 用 osDelay 而不是忙等：面板复位那约 465 ms 内会主动让出 CPU，
 *       按键扫描与 OLED 任务照常运行。设备层只描述"按手册要等 N 毫秒"，
 *       具体怎么等由本函数决定——这是"换 OS 只改 TASK"的落点之一。
 */
static void Task_LVGL_Delay_Ms(uint32_t ms)
{
    (void)osDelay(ms);
}

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
    if (Svc_LVGL_Init(Task_LVGL_Tick_Ms, Task_LVGL_Delay_Ms) != 0) {
        for (;;) {
            (void)osDelay(TASK_LVGL_FAULT_DELAY_MS);
        }
    }
    APP_Screen_Init();

    /* ---------- 2. 同步数据 → 推进渲染，两件事都在本线程 ---------- */
    for (;;) {
        osDelay(TASK_LVGL_PERIOD_MS);

        /* 先同步业务数据（值没变时只有一次整数比较，不产生 LVGL 调用），
         * 再推进 LVGL：写在 subject 上的新值会在同一次 handler 里被渲染，
         * 顺序反过来会让新值多等一个节拍才上屏。 */
        APP_Screen_Step();

        /* 无论数据有没有变都要推进：动画、重绘与屏幕首次上屏都靠它 */
        Svc_LVGL_Step();
    }
}
