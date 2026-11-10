/**
 * @file app_ui_port.c
 * @brief 应用层：LVGL Pro 导出界面的本地适配层实现（三屏）
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 本文件是本工程唯一直接引用生成代码符号的地方（screen1/2/3、
 *       subject_*、lvgl_ui_project_init）。生成代码在 ../UI_Export/，
 *       两者是并列目录：UI_Export 只被编辑器整体覆盖，本文件从不被覆盖。
 *       其余文件一律只见 app_ui_port.h，所以重新导出 UI 时受影响的
 *       就只剩这里，而这里本身不需要改——只需重做头文件顶部列出的
 *       那四处（lv_conf 开关、删翻译段、screen2 量程、按键提示文案）。
 *
 *       所有函数都必须在 LVGL 线程（Task_LVGL）里调用，理由见头文件。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdbool.h>
#include <stddef.h>
#include "app_ui_port.h"
// 功能包含
#include "lvgl_ui_project.h"     /* 生成代码的聚合头：init + 三个 screen + subjects */

/*==============================================================================
 * 静态变量
 *============================================================================*/
/**
 * @brief 三块屏的句柄，下标即 App_UI_Show() 的 index
 * @note screen1 由 init_gen() 建成常驻屏；screen2 / screen3 由各自的
 *       create 函数新建，导出代码不为它们保留全局指针，所以指针由本层持有。
 *       三块屏在生命周期内都常驻 LVGL 堆，切屏只做 lv_screen_load()，
 *       不销毁重建——重建一次要重跑整段构造代码，10 ms 节拍里做不完，
 *       也会让 observer 与控件的绑定关系重新建立一遍。
 */
static lv_obj_t *s_screens[APP_UI_SCREEN_COUNT];

/**
 * @brief 界面是否已初始化完成
 * @note 用来挡住"Init 之前就写 subject / 切屏"的调用。subject 在
 *       lvgl_ui_project_init() 里才被创建，提前写入是空指针解引用。
 */
static bool s_ready = false;

/**
 * @brief 上一次真正写进 subject 的计数值
 * @note 初值 -1 保证第一次写入一定生效（合法计数 >= 0）。
 *       有了它，App_UI_Set_Steps() 在值未变化时可以直接返回：
 *       lv_subject_set_int() 即使新值等于旧值也会遍历全部 observer，
 *       而显示线程每 10 ms 就会调一次本函数，这个短路是必要的。
 */
static int32_t s_shown = -1;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把一个 subject 改成本工程的量程并清零
 * @param subject 目标 subject，导出代码创建的
 * @param min     下限
 * @param value   初值（同时作为满量程时的目标值使用）
 * @retval 无
 * @note 导出代码给每个 subject 都写了演示量程（200000 / 20000 / 100），
 *       本函数在 Init 里统一覆盖。放在这里而不是改生成代码，是为了让
 *       重新导出后仍然自动生效。
 */
static void App_UI_Setup_Subject(lv_subject_t *subject, int32_t min, int32_t value)
{
    if (subject == NULL) {
        return;
    }

    lv_subject_set_min_value_int(subject, min);
    lv_subject_set_max_value_int(subject, APP_UI_STEPS_MAX);
    lv_subject_set_int(subject, value);
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 建三块屏、统一量程、加载首屏
 * @retval 无
 * @note 顺序不能换：先 init（建 subject、建 screen1），再建另外两块屏，
 *       最后才 lv_screen_load()。反过来会加载一个 NULL 屏幕。
 */
void App_UI_Init(void)
{
    lvgl_ui_project_init(NULL);

    /* ---------- 1. 收齐三块屏的句柄 ---------- */
    s_screens[0] = screen1;                 /* 常驻屏，init_gen 已建好 */
    s_screens[1] = screen2_create();        /* 各建一次，指针自持 */
    s_screens[2] = screen3_create();

    /* ---------- 2. 把演示量程换成本工程的 0~100 ---------- */
    /* subject_steps：screen1 的大号数字 + 圆弧 value
     * subject_goal ：screen1 圆弧的 max，设成满量程 => 100 时圆环闭合
     * subject_cnt2 ：screen2 的数字 + 进度条 value（条的量程在 screen2_gen.c）
     * subject_cnt3 ：screen3 的数字 + 5 段点阵（>=20/40/60/80/100 逐段点亮） */
    App_UI_Setup_Subject(subject_steps, 0, 0);
    App_UI_Setup_Subject(subject_goal,  1, APP_UI_STEPS_MAX);
    App_UI_Setup_Subject(subject_cnt2,  0, 0);
    App_UI_Setup_Subject(subject_cnt3,  0, 0);

    /* ---------- 3. 上屏 ---------- */
    lv_screen_load(s_screens[0]);

    s_shown = -1;                           /* 让第一次 Set_Steps 必定生效 */
    s_ready = true;
}

/**
 * @brief 切换到第 index 块屏
 * @param index 屏序号，0 = screen1，1 = screen2，2 = screen3
 * @retval 无
 * @note 越界直接返回：循环取模属于 APP_Screen 的策略，本层只做"显示哪一块"。
 *       句柄为 NULL 说明构造失败（LVGL 堆不足），此时保持当前屏不变，
 *       比加载一个空指针安全。
 */
void App_UI_Show(uint8_t index)
{
    if (!s_ready || (index >= APP_UI_SCREEN_COUNT)) {
        return;
    }

    if (s_screens[index] == NULL) {
        return;
    }

    lv_screen_load(s_screens[index]);
}

/**
 * @brief 把计数值写进三块屏
 * @param steps 要显示的计数
 * @retval 无
 * @note 夹取范围与 APP_STEP_MAX 一致。上限同时保护了圆弧与进度条：
 *       两者的 max 都已经是 100，值超过也不会画出越界的弧。
 */
void App_UI_Set_Steps(int32_t steps)
{
    if (!s_ready) {
        return;
    }

    if (steps < 0) {
        steps = 0;
    } else if (steps > APP_UI_STEPS_MAX) {
        steps = APP_UI_STEPS_MAX;
    }

    if (steps == s_shown) {
        return;
    }

    s_shown = steps;

    /* 三块屏显示同一路计数：一次写三份，切屏时不会有旧值闪现 */
    lv_subject_set_int(subject_steps, steps);
    lv_subject_set_int(subject_cnt2, steps);
    lv_subject_set_int(subject_cnt3, steps);
}
