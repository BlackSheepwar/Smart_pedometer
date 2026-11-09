/**
 * @file app_key_cmd.c
 * @brief 按键命令分发（配置表驱动）
 * @author Ricardo
 * @date 2026-10-23
 *
 * @note 参照 RC_Dog 的 app_key_cmd.c：命令不写在 if 里，全部集中到
 *       APP_KEY_CMD_TABLE 一张表，表里一行 = 一个"键 × 事件 → 动作"。
 *       新增按键或新增事件，只需写一个 handler 再在表里加一行。
 *
 *       当前的分工：
 *         单击   → 档位 ±1
 *         长按   → 满 200 ms 后开始连发 REPEAT，每次 ±1（灯带跟着一格一格动）
 *         B10    → 步数 +1；按住不放则 LONG 后每 200 ms 继续 +1
 *
 *       处理函数一律无参、只做短动作：它们跑在 Task_Key_Cmd 线程里，
 *       一旦阻塞，8 格事件队列就会撑满、新事件被丢弃。
 *
 *       B10 走的是"只改业务状态、不碰界面"的路线：handler 里只调
 *       APP_Step_Add() 改一个整数，既没有 LVGL 调用，也不唤醒显示线程。
 *       界面线程本来就以 10 ms 周期在跑，每个节拍读一次计数，
 *       值变了才写 subject。计数显示滞后一个节拍（10 ms），
 *       比"按键线程直接刷屏"和"每次按键都发一次跨线程唤醒"都简单。
 *
 *       本层不碰 OS：这里一行内核调用都没有（上一版 B10 调用的
 *       Svc_LVGL_Request_Update 已移除）；TASK 层因此只负责
 *       "取事件、调命令表"两件事，不承担任何 UI 通知策略。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
// 功能包含
#include "app_key_cmd.h"
#include "app_level.h"
#include "app_step.h"
#include "dev_led.h"

/*==============================================================================
 * 内部类型
 *============================================================================*/
/**
 * @brief 命令表条目：哪个键的哪个事件调哪个处理函数
 */
typedef struct {
    const dev_key_t *key;
    key_event_t      event;
    void           (*handler)(void);
} app_key_cmd_entry_t;

/*==============================================================================
 * 按键事件处理函数
 *============================================================================*/
/**
 * @brief 档位 +1
 * @retval 无
 */
static void Key_Inc(void)
{
    APP_Level_Step(1);
}

/**
 * @brief 档位 -1
 * @retval 无
 */
static void Key_Dec(void)
{
    APP_Level_Step(-1);
}

/**
 * @brief 翻转板载 LED
 * @retval 无
 */
static void Key_ON(void)
{
    Dev_LED_Toggle(&dev_led_1);
}

/**
 * @brief 关闭板载 LED
 * @retval 无
 */
static void Key_OFF(void)
{
    Dev_LED_Write(&dev_led_1, LED_OFF);
}

/**
 * @brief B10 界面键：步数 +1
 * @retval 无
 * @note 只改 APP_Step 里的一个整数：不碰 LVGL、不碰 SPI、不碰信号量。
 *       绘制发生在 Task_LVGL —— 它每 10 ms 读一次计数，值变了才写 subject，
 *       由 LVGL 的 observer 自动刷新中央大号数字与圆弧进度。
 *       因此本函数执行时间是常数，不会拖慢 8 格深的事件队列。
 *
 *       长按连续计数不在这里实现：命令表把同一个 handler 绑到
 *       DOWN / LONG / REPEAT 三个事件上，长按阈值与连发间隔来自
 *       Svc_Key_Logic 的配置表（dev_key_color 当前是 200 ms / 200 ms）。
 */
static void Key_Step_Inc(void)
{
    APP_Step_Add(1);
}

/*==============================================================================
 * 命令表
 *============================================================================*/
/**
 * @brief 键 × 事件 → 处理函数
 * @note 只列有意义的组合；没列的组合查表查不到，直接丢弃。
 *       档位在按下（DOWN）当拍就动，长按由 LONG 与 REPEAT 负责连发，
 *       CLICK / LONG_UP 不产生动作。
 */
static const app_key_cmd_entry_t APP_KEY_CMD_TABLE[] = {
    { &dev_key_inc, KEY_EVENT_DOWN,    Key_Inc    },
    { &dev_key_inc, KEY_EVENT_LONG,    Key_Inc    },
    { &dev_key_inc, KEY_EVENT_REPEAT,  Key_Inc    },

    { &dev_key_dec, KEY_EVENT_DOWN,    Key_Dec    },
    { &dev_key_dec, KEY_EVENT_LONG,    Key_Dec    },
    { &dev_key_dec, KEY_EVENT_REPEAT,  Key_Dec    },

    { &dev_key_led, KEY_EVENT_DOWN,    Key_ON     },
    { &dev_key_led, KEY_EVENT_LONG_UP, Key_OFF    },

    { &dev_key_color, KEY_EVENT_DOWN,   Key_Step_Inc },
    { &dev_key_color, KEY_EVENT_LONG,   Key_Step_Inc },
    { &dev_key_color, KEY_EVENT_REPEAT, Key_Step_Inc },
};

/**
 * @brief 命令表行数
 */
#define APP_KEY_CMD_COUNT  (sizeof(APP_KEY_CMD_TABLE) / sizeof(APP_KEY_CMD_TABLE[0]))

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 按键命令分发入口
 * @param key   按键设备句柄（与表里存的是同一个对象地址，按地址比较）
 * @param event 按键事件（DOWN / CLICK / LONG / REPEAT / LONG_UP）
 * @retval 无
 * @note 由 Task_Key_Cmd 每取到一条事件调用一次；查不到组合就静默丢弃。
 *       句柄是 const 全局对象，地址唯一，所以"是不是这个键"等价于
 *       "是不是这个地址"；运行期拷贝句柄会让这条判断失效。
 */
void APP_Key_Cmd_Packet(const dev_key_t *key, key_event_t event)
{
    uint8_t index;

    for (index = 0u; index < (uint8_t)APP_KEY_CMD_COUNT; ++index) {
        if ((APP_KEY_CMD_TABLE[index].key == key) &&
            (APP_KEY_CMD_TABLE[index].event == event)) {
            APP_KEY_CMD_TABLE[index].handler();
            return;
        }
    }
}
