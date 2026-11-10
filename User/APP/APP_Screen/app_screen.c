/**
 * @file app_screen.c
 * @brief 应用层：界面生命周期、切屏与数据同步实现
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 本文件只有几十行，看起来"薄"，但它承担的是界面语义的所有权：
 *       谁建屏、当前显示哪一屏、什么时候刷、刷什么值。
 *       把这些散到 Task_LVGL 里会让线程体同时知道 UI 和业务；
 *       散到 app_ui_port 里又会让"适配层"变成"业务层"。
 *
 *       线程模型（本模块唯一的复杂点）：
 *         s_pending  写入者 = Task_Key_Cmd（APP_Screen_Next）
 *         s_pending  读取者 = Task_LVGL（APP_Screen_Step）
 *         s_active   只被 Task_LVGL 读写，记录"已经真正显示的是哪一屏"
 *       两个变量都是 8 位对齐量，Cortex-M4 上单条 STR/LDR 不可分割，
 *       所以不加锁也不会有"半个索引"。volatile 只是防止编译器把
 *       s_pending 提到寄存器里缓存，导致显示线程永远读不到新请求。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include "app_screen.h"
// 功能包含
#include "app_step.h"            /* 业务数据来源 */
#include "app_ui_port.h"         /* 界面适配层：Init / Show / Set_Steps */

/*==============================================================================
 * 静态变量
 *============================================================================*/
/**
 * @brief 待显示的屏序号（按键线程写）
 * @note 初值 0：上电显示 screen1。用 volatile 的理由见文件头。
 */
static volatile uint8_t s_pending = 0;

/**
 * @brief 已经真正加载到 display 的屏序号（显示线程独占）
 * @note 与 s_pending 分开存：请求和"已生效"是两件事，
 *       混成一个变量会让显示线程误判"已经切过了"。
 */
static uint8_t s_active = 0;

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 建立界面并显示首帧
 * @retval 无
 * @note 先 Init 再补一次 Step：导出代码里 subject 的初值是演示数据
 *       （8432 / 12480 / 62），不立刻覆盖的话上电会先闪一下。
 *       App_UI_Init() 已经把三屏的 subject 清零，所以这里补 Step 的作用
 *       是把业务侧的当前值（可能是 0）与界面拉齐，顺便走通一次同步路径。
 */
void APP_Screen_Init(void)
{
    s_pending = 0;
    s_active  = 0;

    App_UI_Init();          /* 建三屏 + 统一 0~100 量程 + 加载第 0 屏 */
    APP_Screen_Step();      /* 数值与界面拉齐 */
}

/**
 * @brief 请求切到下一屏（循环）
 * @retval 无
 * @note 只改 s_pending，不碰 LVGL。单写者，所以"读-改-写"三步之间
 *       不会被打断；将来若出现第二个写入者（例如另一颗键），必须改成
 *       临界区，否则两次按键可能只前进一格。
 */
void APP_Screen_Next(void)
{
    uint8_t next = (uint8_t)(s_pending + 1u);

    if (next >= APP_UI_SCREEN_COUNT) {
        next = 0;
    }

    s_pending = next;
}

/**
 * @brief 把一个显示节拍的数据同步到界面
 * @retval 无
 * @note 先落地切屏请求，再同步数值。顺序有意义：先切屏，新屏的第一帧
 *       就带着当前计数，不会出现"旧屏的值已经刷新、新屏还是旧数据"的
 *       中间态（虽然只有一帧，但在 30 fps 下看得见）。
 *       去重放在 App_UI_Set_Steps() 里（它记得上次写入的值），
 *       本函数不重复持有第二份缓存——两份缓存迟早会不同步。
 */
void APP_Screen_Step(void)
{
    uint8_t wanted = s_pending;

    if (wanted != s_active) {
        s_active = wanted;
        App_UI_Show(s_active);
    }

    App_UI_Set_Steps(APP_Step_Total());
}
