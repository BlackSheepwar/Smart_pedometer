/**
 * @file app_key_cmd.c
 * @brief 按键命令表：本工程"哪个键的哪个事件做什么"
 * @author Ricardo
 * @date 2026-10-23
 *
 * @note 本文件只提供一张表，以及表里引用的处理函数，没有任何对外函数。
 *       查表匹配在 Service/Svc_Key_Cmd，接线在 TASK：取到一条事件后直接
 *       调 Svc_Key_Cmd_Dispatch(APP_KEY_CMD_TABLE, APP_KEY_CMD_COUNT, ...)。
 *
 *       为什么把分发机制交给服务、自己只留数据：
 *         查表是"机制"，与产品无关，谁都能用；表里填什么动作是"策略"，
 *         是本工程的产品行为。两者放在一个文件里时，服务层的判据
 *         （能脱离硬件单测、与产品无关、有第二个使用者）永远无法满足。
 *         表本身是 const，链接期进 .rodata，导出后也没有被改写的风险，
 *         所以不需要一个包装函数替它挡着。
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
 *       本层不碰 OS：这里一行内核调用都没有；TASK 层因此只负责
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
#include "svc_key_cmd.h"        /* svc_key_cmd_entry_t：表的元素类型 */

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
 * @brief 翻转板载 LED（按下亮、再按下灭）
 * @retval 无
 */
static void Key_Led_Toggle(void)
{
    Dev_LED_Toggle(&dev_led_1);
}

/**
 * @brief 关闭板载 LED（长按松手时强制熄灭）
 * @retval 无
 */
static void Key_Led_Off(void)
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
 *       表的顺序不影响语义（各组合互不重复），按"一颗键一组"排列只是为了好读。
 *       导出符号名全大写，是为了让调用点一眼看出"这是配置数据，不是函数"。
 */
const svc_key_cmd_entry_t APP_KEY_CMD_TABLE[] = {
    { &dev_key_inc,   KEY_EVENT_DOWN,    Key_Inc         },
    { &dev_key_inc,   KEY_EVENT_LONG,    Key_Inc         },
    { &dev_key_inc,   KEY_EVENT_REPEAT,  Key_Inc         },

    { &dev_key_dec,   KEY_EVENT_DOWN,    Key_Dec         },
    { &dev_key_dec,   KEY_EVENT_LONG,    Key_Dec         },
    { &dev_key_dec,   KEY_EVENT_REPEAT,  Key_Dec         },

    { &dev_key_led,   KEY_EVENT_DOWN,    Key_Led_Toggle  },
    { &dev_key_led,   KEY_EVENT_LONG_UP, Key_Led_Off     },

    { &dev_key_color, KEY_EVENT_DOWN,    Key_Step_Inc    },
    { &dev_key_color, KEY_EVENT_LONG,    Key_Step_Inc    },
    { &dev_key_color, KEY_EVENT_REPEAT,  Key_Step_Inc    },
};

/**
 * @brief 命令表行数
 * @note 在定义处算好再导出：别的翻译单元看见的表是不完整类型，算不了 sizeof。
 *       uint8_t 与服务层 Svc_Key_Cmd_Dispatch 的 count 形参同宽，调用点不必转换。
 */
const uint8_t APP_KEY_CMD_COUNT =
    (uint8_t)(sizeof(APP_KEY_CMD_TABLE) / sizeof(APP_KEY_CMD_TABLE[0]));
