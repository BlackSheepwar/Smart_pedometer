/**
 * @file screen2_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen2_gen.h"
#include "../lvgl_ui_project.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/***********************
 *  STATIC VARIABLES
 **********************/

/***********************
 *  STATIC PROTOTYPES
 **********************/

static void screen2_lv_label_1_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen2_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if LVGL_UI_PROJECT_CHECK_COMPILE_TARGET(LVGL_UI_PROJECT_TARGET_ALL)
    if (lvgl_ui_project_check_target(LVGL_UI_PROJECT_TARGET_ALL)) {
        lv_obj_t * lv_obj_0 = lv_obj_create(NULL);
        lv_obj_set_name_static(lv_obj_0, "screen2_#");

        lv_obj_add_style(lv_obj_0, &style_screen_paper, 0);
        lv_obj_t * lv_label_0 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_0, "COUNT");
        lv_obj_set_align(lv_label_0, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_0, 14);
        lv_obj_set_y(lv_label_0, 80);
        lv_obj_set_style_text_font(lv_label_0, font_16, 0);
        lv_obj_set_style_text_color(lv_label_0, COLOR_PAPER_MUTED, 0);

        lv_obj_t * lv_label_1 = lv_label_create(lv_obj_0);
        lv_subject_add_observer_obj(subject_cnt2, screen2_lv_label_1_text_binding_cb, lv_label_1, NULL);
        lv_obj_set_align(lv_label_1, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_1, 14);
        lv_obj_set_y(lv_label_1, 110);
        lv_obj_set_style_text_font(lv_label_1, font_num_36, 0);
        lv_obj_set_style_text_color(lv_label_1, COLOR_PAPER_INK, 0);

        lv_obj_t * lv_bar_0 = lv_bar_create(lv_obj_0);
        lv_obj_set_align(lv_bar_0, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_bar_0, 14);
        lv_obj_set_y(lv_bar_0, 176);
        lv_obj_set_width(lv_bar_0, 144);
        lv_obj_set_height(lv_bar_0, 10);
        lv_bar_set_min_value(lv_bar_0, 0);
        /* 本地改动：量程 20000 → 100（满量程 = 计数值上限）。
         * 其余两处 20000 分别由 lvgl_ui_project_gen.c 的 subject_cnt2 上限
         * 与下方 "/ 20000" 标签承担，三处必须一起改。 */
        lv_bar_set_max_value(lv_bar_0, 100);
        lv_bar_bind_value(lv_bar_0, subject_cnt2);
        lv_obj_set_style_radius(lv_bar_0, 5, 0);
        lv_obj_set_style_radius(lv_bar_0, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(lv_bar_0, COLOR_PAPER_TRACK, 0);
        lv_obj_set_style_bg_opa(lv_bar_0, (255 * 100 / 100), 0);
        lv_obj_set_style_bg_color(lv_bar_0, COLOR_PAPER_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(lv_bar_0, (255 * 100 / 100), LV_PART_INDICATOR);

        lv_obj_t * lv_label_2 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_2, "/ 100");   /* 本地改动：原 "/ 20000" */
        lv_obj_set_align(lv_label_2, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_2, 14);
        lv_obj_set_y(lv_label_2, 196);
        lv_obj_set_style_text_font(lv_label_2, font_16, 0);
        lv_obj_set_style_text_color(lv_label_2, COLOR_PAPER_MUTED, 0);

        lv_obj_t * lv_label_3 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_3, "B1  NEXT");   /* 本地改动：原 "B10  NEXT" */
        lv_obj_set_align(lv_label_3, LV_ALIGN_BOTTOM_MID);
        lv_obj_set_y(lv_label_3, -16);
        lv_obj_set_style_text_font(lv_label_3, font_16, 0);
        lv_obj_set_style_text_color(lv_label_3, COLOR_PAPER_MUTED, 0);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void screen2_lv_label_1_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    char eval_buf_1[LV_XML_EVAL_STRING_BUF_SIZE];
    lv_snprintf(eval_buf_1, sizeof(eval_buf_1), "%d", (int)(lv_subject_get_int(subject_cnt2)));
    lv_label_set_text(obj, eval_buf_1);
}

