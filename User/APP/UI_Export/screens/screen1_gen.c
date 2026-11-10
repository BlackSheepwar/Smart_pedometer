/**
 * @file screen1_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen1_gen.h"
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

static void screen1_lv_arc_0_max_value_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen1_lv_arc_0_value_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen1_lv_label_0_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen1_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");

    static bool screen_created = false;
    if (screen_created) {
        LV_LOG_WARN("`screen1` is already initialized as a permanent screen. Returning with no change.");
        return screen1;
    }


    lv_obj_t * the_root = NULL;

    #if LVGL_UI_PROJECT_CHECK_COMPILE_TARGET(LVGL_UI_PROJECT_TARGET_ALL)
    if (lvgl_ui_project_check_target(LVGL_UI_PROJECT_TARGET_ALL)) {
        lv_obj_t * lv_obj_0 = screen1;
        lv_obj_set_name_static(lv_obj_0, "screen1_#");

        lv_obj_add_style(lv_obj_0, &style_screen, 0);
        lv_obj_t * lv_arc_0 = lv_arc_create(lv_obj_0);
        lv_obj_set_align(lv_arc_0, LV_ALIGN_CENTER);
        lv_obj_set_y(lv_arc_0, -20);
        lv_obj_set_width(lv_arc_0, 150);
        lv_obj_set_height(lv_arc_0, 150);
        lv_arc_set_bg_start_angle(lv_arc_0, 135);
        lv_arc_set_bg_end_angle(lv_arc_0, 405);
        lv_arc_set_min_value(lv_arc_0, 0);
        lv_subject_add_observer_obj(subject_goal, screen1_lv_arc_0_max_value_binding_cb, lv_arc_0, NULL);
        lv_subject_add_observer_obj(subject_steps, screen1_lv_arc_0_value_binding_cb, lv_arc_0, NULL);
        lv_obj_set_style_pad_all(lv_arc_0, 0, 0);
        lv_obj_set_style_bg_opa(lv_arc_0, 0, 0);
        lv_obj_set_style_border_width(lv_arc_0, 0, 0);
        lv_obj_set_style_arc_width(lv_arc_0, ARC_THICKNESS, 0);
        lv_obj_set_style_arc_width(lv_arc_0, ARC_THICKNESS, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(lv_arc_0, COLOR_TRACK, 0);
        lv_obj_set_style_arc_color(lv_arc_0, COLOR_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(lv_arc_0, 0, LV_PART_KNOB);

        lv_obj_t * lv_label_0 = lv_label_create(lv_obj_0);
        lv_subject_add_observer_obj(subject_steps, screen1_lv_label_0_text_binding_cb, lv_label_0, NULL);
        lv_obj_set_align(lv_label_0, LV_ALIGN_CENTER);
        lv_obj_set_y(lv_label_0, -34);
        lv_obj_set_style_text_font(lv_label_0, font_num_36, 0);
        lv_obj_set_style_text_color(lv_label_0, COLOR_TEXT, 0);

        lv_obj_t * lv_label_1 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_1, "STEPS");
        lv_obj_set_align(lv_label_1, LV_ALIGN_CENTER);
        lv_obj_set_y(lv_label_1, 4);
        lv_obj_set_style_text_font(lv_label_1, font_16, 0);
        lv_obj_set_style_text_color(lv_label_1, COLOR_SOFT, 0);

        lv_obj_t * lv_label_2 = lv_label_create(lv_obj_0);
        lv_label_set_text(lv_label_2, "B1  NEXT");   /* 本地改动：原 "B10  NEXT"（切屏键在本工程是 B1） */
        lv_obj_set_align(lv_label_2, LV_ALIGN_BOTTOM_MID);
        lv_obj_set_y(lv_label_2, -16);
        lv_obj_set_style_text_font(lv_label_2, font_16, 0);
        lv_obj_set_style_text_color(lv_label_2, COLOR_MUTED, 0);

        the_root = lv_obj_0;
    }
    #endif

    if (the_root) screen_created = true;

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void screen1_lv_arc_0_max_value_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_arc_set_max_value(obj, lv_subject_get_int(subject_goal));
}

static void screen1_lv_arc_0_value_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_arc_set_value(obj, lv_subject_get_int(subject_steps));
}

static void screen1_lv_label_0_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    char eval_buf_1[LV_XML_EVAL_STRING_BUF_SIZE];
    lv_snprintf(eval_buf_1, sizeof(eval_buf_1), "%d", (int)(lv_subject_get_int(subject_steps)));
    lv_label_set_text(obj, eval_buf_1);
}

