/**
 * @file app_key_cmd.c
 * @brief 按键命令表：本工程"哪个键的哪个事件做什么"
 * @author Ricardo
 * @date 2026-11-10
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
 *       ★当前的分工（板子丝印 → 动作）★
 *         B1  / PB1  → 在"速度 / 经纬度"之间翻转（只在 DOWN 动作一次）
 *         B10 / PB10 → 直达速度面板
 *         B11 / PB11 → 直达经纬度面板
 *         PC13       → 每按一次翻转板载 LED（PB2）；长按松手时强制熄灭
 *
 *       B10/B11 是"直达"而不是"上下翻"：只有两块面板时，上下翻的两颗键
 *       会做同一件事，按哪颗都一样，反而让人怀疑键坏了。直达的语义在
 *       面板变多以后也不用改——想扩成 N 块时再改成 prev/next 即可。
 *
 *       处理函数一律无参、只做短动作：它们跑在 Task_Key_Cmd 线程里，
 *       一旦阻塞，8 格事件队列就会撑满、新事件被丢弃。
 *
 *       三类动作各自只碰一样东西，谁都不越界：
 *         翻面板 → APP_Screen_Toggle_Mode()   只翻一个标志；
 *         选面板 → APP_Screen_Select_Mode()   只写同一个标志；
 *         灯     → Dev_LED_Write()            写一个 GPIO。
 *       真正的 subject 写入都在 Task_LVGL 里（见 app_screen.h），三者都
 *       不碰 LVGL、不碰 SPI、不发跨线程唤醒。界面刷新与换面板落地由显示线程
 *       按 10 ms 节拍轮询完成，因此按键线程与界面线程之间零耦合，
 *       换面板滞后一个节拍（10 ms），不可感知。
 *
 *       依赖说明：本文件因此多了一条 app_key_cmd → app_screen 的边。
 *       它只用到"翻面板 / 选面板"这两个写标志的入口，不涉及任何 lv_ 类型；
 *       app_screen.h 里也没有 lv_ 类型，所以 LVGL 不会顺着这条边漏到 APP 层。
 *
 *       本层不碰 OS：这里一行内核调用都没有；TASK 层因此只负责
 *       "取事件、调分发"两件事，不承担任何 UI 通知策略。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
// 功能包含
#include "app_key_cmd.h"
#include "app_screen.h"         /* APP_Screen_Toggle_Mode()：请求换面板，只翻标志 */
#include "dev_led.h"
#include "svc_key_cmd.h"        /* svc_key_cmd_entry_t：表的元素类型 */

/*==============================================================================
 * 按键事件处理函数
 *============================================================================*/
/**
 * @brief 翻转板载 LED（按下亮、再按下灭）
 * @retval 无
 */
static void Key_Led_Toggle(void)
{
    Dev_LED_Toggle(&dev_led_1);
    APP_Screen_Toggle_Mode();
}

/**
 * @brief 关闭板载 LED（长按松手时强制熄灭）
 * @retval 无
 */
static void Key_Led_Off(void)
{
    Dev_LED_Write(&dev_led_1, LED_OFF);
}

/*==============================================================================
 * 命令表
 *============================================================================*/
/**
 * @brief 键 × 事件 → 处理函数
 * @note 只列有意义的组合；没列的组合查表查不到，直接丢弃。
 *       三颗面板键都只绑 DOWN：换面板是"一次性"动作，绑 LONG/REPEAT 会在
 *       长按期间来回翻，松手停在哪一块面板不可预期。
 *       LED 键按下即翻转、长按松手熄灭，CLICK / LONG_UP 之外的组合不动作。
 *       表的顺序不影响语义（各组合互不重复），按"一颗键一组"排列只是为了好读。
 *       导出符号名全大写，是为了让调用点一眼看出"这是配置数据，不是函数"。
 */
const svc_key_cmd_entry_t APP_KEY_CMD_TABLE[] = {
    { &dev_key_led,    KEY_EVENT_DOWN,    Key_Led_Toggle  },
    { &dev_key_led,    KEY_EVENT_LONG_UP, Key_Led_Off     },
};

/**
 * @brief 命令表行数
 * @note 在定义处算好再导出：别的翻译单元看见的表是不完整类型，算不了 sizeof。
 *       uint8_t 与服务层 Svc_Key_Cmd_Dispatch 的 count 形参同宽，调用点不必转换。
 */
const uint8_t APP_KEY_CMD_COUNT =
    (uint8_t)(sizeof(APP_KEY_CMD_TABLE) / sizeof(APP_KEY_CMD_TABLE[0]));
