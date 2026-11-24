/**
 * @file task_key_scan.c
 * @brief 任务层：按键扫描线程体
 * @author Ricardo
 * @date 2026-10-22
 *
 * @note 这一层只做"节拍 + 调一步"，不含任何业务判断：
 *       周期在 .ioc 的任务配置和下面的宏里，业务在 APP 层。
 *       这样调周期只改一个数，改业务不碰调度。
 *
 *       时间前提：本工程 configTICK_RATE_HZ == 1000，即 1 tick = 1 ms。
 *       这个前提由下面的 _Static_assert 锁在**编译期**，不靠注释口头约定：
 *       tick 频率一旦被改动，这里立刻编译失败，而不是让 10 ms 采样悄悄
 *       变成 20 ms（现象只是"按键变迟钝"，几乎无法反推到 tick 频率）。
 *       TASK 层的 task_lvgl.c 用同一前提，两处各有静态断言。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
// 功能包含
#include "osal.h"
#include "svc_key_logic.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 按键扫描周期（ms），与 20 ms 消抖窗口对应 */
#define TASK_KEY_SCAN_PERIOD_MS     10u
/**
 * @brief 同一个周期的 tick 数
 * @note 因为 1 tick = 1 ms，两者数值相等；分成两个名字是为了让"毫秒"与
 *       "tick"在代码里不混用——osDelayUntil() 收的是 tick，业务服务收的是 ms。
 */
#define TASK_KEY_SCAN_PERIOD_TICKS  TASK_KEY_SCAN_PERIOD_MS

/**
 * @brief 锁住"1 tick = 1 ms"这个时间前提
 * @note 不能用 #if 代替：configTICK_RATE_HZ 带 (TickType_t) 强制转换，
 *       预处理器不认识这个类型名，会把它替换成 0 并报语法错误。
 *       _Static_assert 由编译器在语义分析阶段求值，不产生任何机器码。
 */
_Static_assert(configTICK_RATE_HZ == 1000,
               "Task_Key_Scan 假设 1 tick = 1 ms；tick 频率变了必须改回 tick<->ms 的换算写法");

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 按键扫描线程体
 * @param argument 未使用
 * @retval 无
 * @note 用 osDelayUntil 按**绝对**节拍唤醒：每一拍的目标时刻是
 *       "上一拍目标 + 10 ms"，而不是"从此刻起等 10 ms"。后者会把本拍的
 *       处理耗时与调度延迟累加进下一拍，长期运行采样周期越拖越长。
 *
 *       但 osDelayUntil 只保证"尽量准时"，不保证一定准时：被更高优先级
 *       任务（例如整屏刷新的 LVGL 线程）抢占时，可能连着错过几拍。所以
 *       报给业务层的间隔必须取**实测 tick 差**，不能用常量——消抖窗口与
 *       长按判定都按毫秒记账，用常量会在被抢占后把时间轴拉快。
 *
 *       落后超过一个周期时把 next_tick 拉回当前时刻：否则下一轮
 *       next_tick += period 仍落在过去，osDelayUntil 会连续立即返回，
 *       形成忙等（elapsed_ms 每次都是 0，什么活也没干却占满 CPU）。
 */
void Task_Key_Scan(void *argument)
{
    uint32_t next_tick;
    uint32_t last_tick;
    uint32_t now;
    uint32_t elapsed_ms;

    (void)argument;

    /* ---------- 1. 以当前时刻为基准，第一拍由循环里的 += 决定 ---------- */
    last_tick = osKernelGetTickCount();
    next_tick = last_tick;

    for (;;) {
        next_tick += TASK_KEY_SCAN_PERIOD_TICKS;

        (void)osDelayUntil(next_tick);

        /* ---------- 2. 实测间隔：1 tick = 1 ms，直接相减，无乘法也无除法 ---------- */
        now        = osKernelGetTickCount();
        elapsed_ms = now - last_tick;   /* 无符号减法，49.7 天回绕后仍正确 */
        last_tick  = now;

        /* 落后超过一个周期就重新对齐，避免连续多拍空转 */
        if ((int32_t)(now - next_tick) > 0) {
            next_tick = now;
        }

        if (elapsed_ms == 0u) {
            continue;
        }

        /* 业务接口收 uint16_t：被长时间抢占时饱和到上限，不让它回绕成小值 */
        if (elapsed_ms > 0xFFFFu) {
            elapsed_ms = 0xFFFFu;
        }

        Svc_Key_Logic_Tick((uint16_t)elapsed_ms);
    }
}
