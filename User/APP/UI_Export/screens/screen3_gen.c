/**
 * @file screen3_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen3_gen.h"
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

static lv_style_t * screen3_style_seg_lit_ref; /* set in create(), read by a binding callback */
static void screen3_lv_label_1_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen3_lv_obj_1_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen3_lv_obj_2_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen3_lv_obj_3_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen3_lv_obj_4_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen3_lv_obj_5_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen3_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if LVGL_UI_PROJECT_CHECK_COMPILE_TARGET(LVGL_UI_PROJECT_TARGET_ALL)
    if (lvgl_ui_project_check_target(LVGL_UI_PROJECT_TARGET_ALL)) {
        lv_obj_t * lv_obj_0 = lv_obj_create(NULL);
        lv_obj_set_name_static(lv_obj_0, "screen3_#");

        lv_obj_add_style(lv_obj_0, &style_screen_hud, 0);
        lv_obj_t * lv_label_0 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_0, "LOAD");
        lv_obj_set_align(lv_label_0, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_0, 14);
        lv_obj_set_y(lv_label_0, 16);
        lv_obj_set_style_text_font(lv_label_0, font_16, 0);
        lv_obj_set_style_text_color(lv_label_0, COLOR_HUD_SOFT, 0);

        lv_obj_t * lv_label_1 = lv_label_create(lv_obj_0);
        lv_subject_add_observer_obj(subject_cnt3, screen3_lv_label_1_text_binding_cb, lv_label_1, NULL);
        lv_obj_set_align(lv_label_1, LV_ALIGN_CENTER);
        lv_obj_set_y(lv_label_1, -52);
        lv_obj_set_style_text_font(lv_label_1, font_num_36, 0);
        lv_obj_set_style_text_color(lv_label_1, COLOR_HUD_LIT, 0);

        lv_obj_t * lv_label_2 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_2, "PERCENT");
        lv_obj_set_align(lv_label_2, LV_ALIGN_CENTER);
        lv_obj_set_y(lv_label_2, -14);
        lv_obj_set_style_text_font(lv_label_2, font_16, 0);
        lv_obj_set_style_text_color(lv_label_2, COLOR_HUD_SOFT, 0);

        lv_obj_t * lv_obj_1 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_1, LV_ALIGN_TOP_MID);
        lv_obj_set_x(lv_obj_1, -60);
        lv_obj_set_y(lv_obj_1, 188);
        lv_obj_set_width(lv_obj_1, 26);
        lv_obj_set_height(lv_obj_1, 44);
        lv_obj_add_style(lv_obj_1, &style_seg_off, 0);
        lv_obj_add_style(lv_obj_1, &style_seg_lit, 0);
        screen3_style_seg_lit_ref = &style_seg_lit;
        lv_subject_add_observer_obj(subject_cnt3, screen3_lv_obj_1_style_enabled_cb, lv_obj_1, NULL);

        lv_obj_t * lv_obj_2 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_2, LV_ALIGN_TOP_MID);
        lv_obj_set_x(lv_obj_2, -30);
        lv_obj_set_y(lv_obj_2, 188);
        lv_obj_set_width(lv_obj_2, 26);
        lv_obj_set_height(lv_obj_2, 44);
        lv_obj_add_style(lv_obj_2, &style_seg_off, 0);
        lv_obj_add_style(lv_obj_2, &style_seg_lit, 0);
        screen3_style_seg_lit_ref = &style_seg_lit;
        lv_subject_add_observer_obj(subject_cnt3, screen3_lv_obj_2_style_enabled_cb, lv_obj_2, NULL);

        lv_obj_t * lv_obj_3 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_3, LV_ALIGN_TOP_MID);
        lv_obj_set_x(lv_obj_3, 0);
        lv_obj_set_y(lv_obj_3, 188);
        lv_obj_set_width(lv_obj_3, 26);
        lv_obj_set_height(lv_obj_3, 44);
        lv_obj_add_style(lv_obj_3, &style_seg_off, 0);
        lv_obj_add_style(lv_obj_3, &style_seg_lit, 0);
        screen3_style_seg_lit_ref = &style_seg_lit;
        lv_subject_add_observer_obj(subject_cnt3, screen3_lv_obj_3_style_enabled_cb, lv_obj_3, NULL);

        lv_obj_t * lv_obj_4 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_4, LV_ALIGN_TOP_MID);
        lv_obj_set_x(lv_obj_4, 30);
        lv_obj_set_y(lv_obj_4, 188);
        lv_obj_set_width(lv_obj_4, 26);
        lv_obj_set_height(lv_obj_4, 44);
        lv_obj_add_style(lv_obj_4, &style_seg_off, 0);
        lv_obj_add_style(lv_obj_4, &style_seg_lit, 0);
        screen3_style_seg_lit_ref = &style_seg_lit;
        lv_subject_add_observer_obj(subject_cnt3, screen3_lv_obj_4_style_enabled_cb, lv_obj_4, NULL);

        lv_obj_t * lv_obj_5 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_5, LV_ALIGN_TOP_MID);
        lv_obj_set_x(lv_obj_5, 60);
        lv_obj_set_y(lv_obj_5, 188);
        lv_obj_set_width(lv_obj_5, 26);
        lv_obj_set_height(lv_obj_5, 44);
        lv_obj_add_style(lv_obj_5, &style_seg_off, 0);
        lv_obj_add_style(lv_obj_5, &style_seg_lit, 0);
        screen3_style_seg_lit_ref = &style_seg_lit;
        lv_subject_add_observer_obj(subject_cnt3, screen3_lv_obj_5_style_enabled_cb, lv_obj_5, NULL);

        lv_obj_t * lv_label_3 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_3, "B1  NEXT");   /* 本地改动：原 "B10  NEXT" */
        lv_obj_set_align(lv_label_3, LV_ALIGN_BOTTOM_MID);
        lv_obj_set_y(lv_label_3, -16);
        lv_obj_set_style_text_font(lv_label_3, font_16, 0);
        lv_obj_set_style_text_color(lv_label_3, COLOR_HUD_SOFT, 0);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void screen3_lv_label_1_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    char eval_buf_1[LV_XML_EVAL_STRING_BUF_SIZE];
    lv_snprintf(eval_buf_1, sizeof(eval_buf_1), "%d", (int)(lv_subject_get_int(subject_cnt3)));
    lv_label_set_text(obj, eval_buf_1);
}

static void screen3_lv_obj_1_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen3_style_seg_lit_ref, 0, lv_subject_get_int(subject_cnt3) >= 20);
}

static void screen3_lv_obj_2_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen3_style_seg_lit_ref, 0, lv_subject_get_int(subject_cnt3) >= 40);
}

static void screen3_lv_obj_3_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen3_style_seg_lit_ref, 0, lv_subject_get_int(subject_cnt3) >= 60);
}

static void screen3_lv_obj_4_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen3_style_seg_lit_ref, 0, lv_subject_get_int(subject_cnt3) >= 80);
}

static void screen3_lv_obj_5_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen3_style_seg_lit_ref, 0, lv_subject_get_int(subject_cnt3) >= 100);
}

