/**
 * @file app_screen.c
 * @brief 应用层实现：两个 LVGL 界面的创建与切换
 * @author Ricardo
 * @date 2026-11-07
 *
 * @note 两屏的区分只做在视觉上，不引入任何额外硬件依赖：
 *         - SCREEN 1：深蓝底 + 橙色顶条 + 白色 "SCREEN 1"
 *         - SCREEN 2：橙色底 + 深蓝顶条 + 白色 "SCREEN 2"
 *       底色与顶条互为反色，切换瞬间整屏颜色反转，一眼可辨。
 *
 *       文字用英文：工程没有引入中文字库，montserrat 只含 ASCII。
 *       字号用 28（标题）与 14（提示），对应 lv_conf.h 里打开的两个字体。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include "app_screen.h"
#include "lvgl.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 界面总数：两屏循环 */
#define APP_SCREEN_COUNT        2u
/** @brief 顶条宽度（像素） */
#define APP_SCREEN_BAR_W        120
/** @brief 顶条高度（像素） */
#define APP_SCREEN_BAR_H        14
/** @brief 顶条圆角半径（像素） */
#define APP_SCREEN_BAR_RADIUS   7

/** @brief SCREEN 1 的底色：深蓝 */
#define APP_SCREEN_BG_1         0x1B2A4Au
/** @brief SCREEN 1 的顶条色：橙 */
#define APP_SCREEN_ACCENT_1     0xE07A1Fu
/** @brief SCREEN 2 的底色：橙 */
#define APP_SCREEN_BG_2         0xE07A1Fu
/** @brief SCREEN 2 的顶条色：深蓝 */
#define APP_SCREEN_ACCENT_2     0x1B2A4Au

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief 两个 screen 对象，常驻内存，切换不重建 */
static lv_obj_t *s_screens[APP_SCREEN_COUNT];
/** @brief 当前界面索引，0 或 1 */
static uint8_t  s_index;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 按配色与文案构造一屏
 * @param bg     整屏底色，RGB888
 * @param accent 顶条颜色，RGB888
 * @param title  中央标题字符串（montserrat 28）
 * @param hint   下方提示字符串（montserrat 14）
 * @retval 非 NULL: 新创建的 screen 对象
 * @retval NULL: 对象创建失败（LVGL 堆不足）
 * @note 顶条、标题、提示都是 screen 的子对象，随 screen 一起被管理，
 *       不需要单独保存指针：切换时由 LVGL 按对象树重绘。
 */
static lv_obj_t *APP_Screen_Make(uint32_t bg, uint32_t accent,
                                 const char *title, const char *hint)
{
    lv_obj_t *scr;
    lv_obj_t *bar;
    lv_obj_t *label;

    /* ---------- 1. 整屏底色：parent 传 NULL 即创建一个独立 screen ---------- */
    scr = lv_obj_create(NULL);
    if (scr == NULL) {
        return NULL;
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(bg), 0u);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0u);

    /* ---------- 2. 顶条：纯色圆角矩形，作为两屏的第二处颜色差异 ---------- */
    bar = lv_obj_create(scr);
    if (bar != NULL) {
        lv_obj_set_size(bar, APP_SCREEN_BAR_W, APP_SCREEN_BAR_H);
        lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0u);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0u);
        lv_obj_set_style_border_width(bar, 0, 0u);
        lv_obj_set_style_radius(bar, APP_SCREEN_BAR_RADIUS, 0u);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, -100);
    }

    /* ---------- 3. 标题：montserrat 28，白色 ---------- */
    label = lv_label_create(scr);
    if (label != NULL) {
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0u);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFFu), 0u);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    }

    /* ---------- 4. 提示：montserrat 14，说明 B10 会切到哪一屏 ---------- */
    label = lv_label_create(scr);
    if (label != NULL) {
        lv_label_set_text(label, hint);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0u);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFFu), 0u);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 40);
    }

    return scr;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 创建两个界面并加载第 0 屏
 * @retval 无
 * @note 某一屏创建失败时保留 NULL，APP_Screen_Next() 会跳过它：
 *       堆不足时不至于 lv_screen_load(NULL) 触发断言。
 */
void APP_Screen_Init(void)
{
    s_index = 0u;

    /* ---------- 1. 两屏一次性建好，之后只换指针 ---------- */
    s_screens[0] = APP_Screen_Make(APP_SCREEN_BG_1, APP_SCREEN_ACCENT_1,
                                   "SCREEN 1", "B10 -> SCREEN 2");
    s_screens[1] = APP_Screen_Make(APP_SCREEN_BG_2, APP_SCREEN_ACCENT_2,
                                   "SCREEN 2", "B10 -> SCREEN 1");

    /* ---------- 2. 上电先显示 SCREEN 1 ---------- */
    if (s_screens[0] != NULL) {
        lv_screen_load(s_screens[0]);
    }
}

/**
 * @brief 切到下一屏（0 → 1 → 0 循环）
 * @retval 无
 * @note 只改指针并标记重绘，像素输出在下一次 lv_timer_handler()。
 */
void APP_Screen_Next(void)
{
    uint8_t next = (uint8_t)((s_index + 1u) % APP_SCREEN_COUNT);

    if (s_screens[next] == NULL) {
        return;
    }

    s_index = next;
    lv_screen_load(s_screens[s_index]);
}

/**
 * @brief 查询当前界面索引
 * @retval 0: 当前是 SCREEN 1
 * @retval 1: 当前是 SCREEN 2
 */
uint8_t APP_Screen_Index(void)
{
    return s_index;
}
