/**
 * @file app_ui_port.c
 * @brief 应用层：LVGL Pro 导出界面的本地适配层实现
 * @author Ricardo
 * @date 2026-11-09
 *
 * @note 本文件是本工程唯一直接引用生成代码符号的地方（screen1、
 *       subject_steps、lvgl_ui_project_init）。生成代码在 ../UI_Export/，
 *       两者是并列目录：UI_Export 只被编辑器整体覆盖，本文件从不被覆盖。
 *       其余文件一律只见 app_ui_port.h，所以重新导出 UI 时受影响的
 *       就只剩这里，而这里本身不需要改——只需重做头文件顶部列出的
 *       那两处（lv_conf 开关、删翻译段）。
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
#include "lvgl_ui_project.h"     /* 生成代码的聚合头：init + screen1 + subjects */

/*==============================================================================
 * 静态变量
 *============================================================================*/
/**
 * @brief 界面是否已初始化完成
 * @note 用来挡住"Init 之前就写 subject"的调用。subject_steps 在
 *       lvgl_ui_project_init() 里才被创建，提前写入是空指针解引用。
 */
static bool s_ready = false;

/**
 * @brief 上一次真正写进 subject 的步数
 * @note 初值 -1 保证第一次写入一定生效（合法步数 >= 0）。
 *       有了它，App_UI_Set_Steps() 在值未变化时可以直接返回：
 *       lv_subject_set_int() 即使新值等于旧值也会遍历全部 observer，
 *       而显示线程每 10 ms 就会调一次本函数，这个短路是必要的。
 */
static int32_t s_shown = -1;

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化导出界面并加载到 display
 * @retval 无
 * @note 顺序不能换：先 init（建 subject、建 screen1 及其 13 个控件），
 *       再 lv_screen_load()。反过来会加载一个 NULL 屏幕。
 */
void App_UI_Init(void)
{
    lvgl_ui_project_init(NULL);
    lv_screen_load(screen1);

    s_shown = -1;
    s_ready = true;
}

/**
 * @brief 把步数写进界面
 * @param steps 要显示的步数
 * @retval 无
 * @note 夹取范围与 globals.xml 里 subject_steps 的 min/max 一致。
 *       上限夹取同时保护了圆弧：arc 的 max 绑定的是 subject_goal（10000），
 *       steps 超过 goal 时 LVGL 自己也会把 value 夹到 max，不会画出越界的弧。
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
    lv_subject_set_int(subject_steps, steps);
}
