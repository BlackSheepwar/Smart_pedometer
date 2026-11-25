/**
 * @file screen_hud_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen_hud_gen.h"
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

static lv_style_t * screen_hud_style_meter_seg_on_ref; /* set in create(), read by a binding callback */
static lv_style_t * screen_hud_style_meter_seg_low_ref; /* set in create(), read by a binding callback */
static lv_style_t * screen_hud_style_meter_warn_ref; /* set in create(), read by a binding callback */
static lv_style_t * screen_hud_style_meter_tab_on_ref; /* set in create(), read by a binding callback */
static void screen_hud_lv_label_0_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_1_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_1_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_1_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_2_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_2_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_3_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_3_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_4_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_4_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_5_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_5_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_pane_speed_checked_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_4_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_7_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_8_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_9_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_10_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_11_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_12_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_13_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_14_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_15_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_16_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_17_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_18_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_pane_pos_checked_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_19_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_20_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_21_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_22_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_23_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_10_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_12_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_14_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_14_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_15_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_16_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_26_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_27_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen_hud_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");

    static bool screen_created = false;
    if (screen_created) {
        LV_LOG_WARN("`screen_hud` is already initialized as a permanent screen. Returning with no change.");
        return screen_hud;
    }


    lv_obj_t * the_root = NULL;

    #if LVGL_UI_PROJECT_CHECK_COMPILE_TARGET(LVGL_UI_PROJECT_TARGET_ALL)
    if (lvgl_ui_project_check_target(LVGL_UI_PROJECT_TARGET_ALL)) {
        lv_obj_t * lv_obj_0 = screen_hud;
        lv_obj_set_name_static(lv_obj_0, "screen_hud_#");

        lv_obj_add_style(lv_obj_0, &style_meter_screen, 0);
        lv_obj_t * lv_label_0 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_0, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_0, 16);
        lv_obj_set_y(lv_label_0, 8);
        lv_subject_add_observer_obj(subject_time, screen_hud_lv_label_0_text_binding_cb, lv_label_0, NULL);
        lv_obj_set_style_text_font(lv_label_0, font_num_44, 0);
        lv_obj_set_style_text_color(lv_label_0, COLOR_METER_INK, 0);

        lv_obj_t * lv_label_1 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_1, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_1, 16);
        lv_obj_set_y(lv_label_1, 48);
        lv_subject_add_observer_obj(subject_date, screen_hud_lv_label_1_text_binding_cb, lv_label_1, NULL);
        lv_obj_set_style_text_font(lv_label_1, font_num_20, 0);
        lv_obj_set_style_text_color(lv_label_1, COLOR_METER_MUTED, 0);
        lv_obj_set_style_text_letter_space(lv_label_1, 1, 0);

        lv_obj_t * lv_obj_1 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_1, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_1, 152);
        lv_obj_set_y(lv_obj_1, 52);
        lv_obj_set_width(lv_obj_1, 12);
        lv_obj_set_height(lv_obj_1, 8);
        lv_obj_add_style(lv_obj_1, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_1, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_1_style_enabled_cb, lv_obj_1, NULL);
        lv_obj_add_style(lv_obj_1, &style_meter_seg_low, 0);
        screen_hud_style_meter_seg_low_ref = &style_meter_seg_low;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_1_style_enabled_cb_2, lv_obj_1, NULL);

        lv_obj_t * lv_obj_2 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_2, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_2, 167);
        lv_obj_set_y(lv_obj_2, 52);
        lv_obj_set_width(lv_obj_2, 12);
        lv_obj_set_height(lv_obj_2, 8);
        lv_obj_add_style(lv_obj_2, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_2, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_2_style_enabled_cb, lv_obj_2, NULL);
        lv_obj_add_style(lv_obj_2, &style_meter_seg_low, 0);
        screen_hud_style_meter_seg_low_ref = &style_meter_seg_low;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_2_style_enabled_cb_2, lv_obj_2, NULL);

        lv_obj_t * lv_obj_3 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_3, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_3, 182);
        lv_obj_set_y(lv_obj_3, 52);
        lv_obj_set_width(lv_obj_3, 12);
        lv_obj_set_height(lv_obj_3, 8);
        lv_obj_add_style(lv_obj_3, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_3, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_3_style_enabled_cb, lv_obj_3, NULL);
        lv_obj_add_style(lv_obj_3, &style_meter_seg_low, 0);
        screen_hud_style_meter_seg_low_ref = &style_meter_seg_low;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_3_style_enabled_cb_2, lv_obj_3, NULL);

        lv_obj_t * lv_obj_4 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_4, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_4, 197);
        lv_obj_set_y(lv_obj_4, 52);
        lv_obj_set_width(lv_obj_4, 12);
        lv_obj_set_height(lv_obj_4, 8);
        lv_obj_add_style(lv_obj_4, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_4, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_4_style_enabled_cb, lv_obj_4, NULL);
        lv_obj_add_style(lv_obj_4, &style_meter_seg_low, 0);
        screen_hud_style_meter_seg_low_ref = &style_meter_seg_low;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_4_style_enabled_cb_2, lv_obj_4, NULL);

        lv_obj_t * lv_obj_5 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_5, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_5, 212);
        lv_obj_set_y(lv_obj_5, 52);
        lv_obj_set_width(lv_obj_5, 12);
        lv_obj_set_height(lv_obj_5, 8);
        lv_obj_add_style(lv_obj_5, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_5, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_5_style_enabled_cb, lv_obj_5, NULL);
        lv_obj_add_style(lv_obj_5, &style_meter_seg_low, 0);
        screen_hud_style_meter_seg_low_ref = &style_meter_seg_low;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_5_style_enabled_cb_2, lv_obj_5, NULL);

        lv_obj_t * lv_obj_6 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_6, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_6, 16);
        lv_obj_set_y(lv_obj_6, 72);
        lv_obj_set_width(lv_obj_6, 208);
        lv_obj_set_height(lv_obj_6, 2);
        lv_obj_add_style(lv_obj_6, &style_meter_rule, 0);

        lv_obj_t * stage_panes = lv_obj_create(lv_obj_0);
        lv_obj_set_name(stage_panes, "stage_panes");
        lv_obj_set_align(stage_panes, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(stage_panes, 0);
        lv_obj_set_y(stage_panes, 80);
        lv_obj_set_width(stage_panes, 240);
        lv_obj_set_height(stage_panes, 170);
        lv_obj_set_scrollable(stage_panes, false);
        lv_obj_set_style_bg_opa(stage_panes, 0, 0);
        lv_obj_set_style_border_width(stage_panes, 0, 0);
        lv_obj_set_style_pad_all(stage_panes, 0, 0);
        lv_obj_set_style_radius(stage_panes, 0, 0);
        lv_obj_t * pane_speed = lv_obj_create(stage_panes);
        lv_obj_set_name(pane_speed, "pane_speed");
        lv_obj_set_align(pane_speed, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(pane_speed, 0);
        lv_obj_set_y(pane_speed, 0);
        lv_obj_set_width(pane_speed, 240);
        lv_obj_set_height(pane_speed, 170);
        lv_obj_set_scrollable(pane_speed, false);
        lv_subject_add_observer_obj(subject_mode, screen_hud_pane_speed_checked_binding_cb, pane_speed, NULL);
        lv_obj_add_style(pane_speed, &style_meter_pane, 0);
        lv_obj_add_style(pane_speed, &style_meter_pane_next, LV_STATE_CHECKED);
        lv_obj_t * lv_label_2 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_2, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_2, 16);
        lv_obj_set_y(lv_label_2, 8);
        lv_label_set_text(lv_label_2, "速度");
        lv_obj_set_style_text_font(lv_label_2, font_cjk_22, 0);
        lv_obj_set_style_text_color(lv_label_2, COLOR_METER_INK, 0);
        lv_obj_set_style_text_letter_space(lv_label_2, 1, 0);

        lv_obj_t * lv_label_3 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_3, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_x(lv_label_3, -16);
        lv_obj_set_y(lv_label_3, 14);
        lv_label_set_text(lv_label_3, "KM/H");
        lv_obj_set_style_text_color(lv_label_3, COLOR_METER_MUTED, 0);
        lv_obj_set_style_text_letter_space(lv_label_3, 2, 0);

        lv_obj_t * lv_label_4 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_4, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_4, 16);
        lv_obj_set_y(lv_label_4, 40);
        lv_subject_add_observer_obj(subject_speed_txt, screen_hud_lv_label_4_text_binding_cb, lv_label_4, NULL);
        lv_obj_set_style_text_font(lv_label_4, font_num_68, 0);
        lv_obj_set_style_text_color(lv_label_4, COLOR_METER_INK, 0);

        lv_obj_t * lv_obj_7 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_7, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_7, 16);
        lv_obj_set_y(lv_obj_7, 114);
        lv_obj_set_width(lv_obj_7, 15);
        lv_obj_set_height(lv_obj_7, 14);
        lv_obj_add_style(lv_obj_7, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_7, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_7_style_enabled_cb, lv_obj_7, NULL);

        lv_obj_t * lv_obj_8 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_8, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_8, 33);
        lv_obj_set_y(lv_obj_8, 114);
        lv_obj_set_width(lv_obj_8, 15);
        lv_obj_set_height(lv_obj_8, 14);
        lv_obj_add_style(lv_obj_8, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_8, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_8_style_enabled_cb, lv_obj_8, NULL);

        lv_obj_t * lv_obj_9 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_9, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_9, 50);
        lv_obj_set_y(lv_obj_9, 114);
        lv_obj_set_width(lv_obj_9, 15);
        lv_obj_set_height(lv_obj_9, 14);
        lv_obj_add_style(lv_obj_9, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_9, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_9_style_enabled_cb, lv_obj_9, NULL);

        lv_obj_t * lv_obj_10 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_10, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_10, 67);
        lv_obj_set_y(lv_obj_10, 114);
        lv_obj_set_width(lv_obj_10, 15);
        lv_obj_set_height(lv_obj_10, 14);
        lv_obj_add_style(lv_obj_10, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_10, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_10_style_enabled_cb, lv_obj_10, NULL);

        lv_obj_t * lv_obj_11 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_11, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_11, 84);
        lv_obj_set_y(lv_obj_11, 114);
        lv_obj_set_width(lv_obj_11, 15);
        lv_obj_set_height(lv_obj_11, 14);
        lv_obj_add_style(lv_obj_11, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_11, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_11_style_enabled_cb, lv_obj_11, NULL);

        lv_obj_t * lv_obj_12 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_12, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_12, 101);
        lv_obj_set_y(lv_obj_12, 114);
        lv_obj_set_width(lv_obj_12, 15);
        lv_obj_set_height(lv_obj_12, 14);
        lv_obj_add_style(lv_obj_12, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_12, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_12_style_enabled_cb, lv_obj_12, NULL);

        lv_obj_t * lv_obj_13 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_13, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_13, 118);
        lv_obj_set_y(lv_obj_13, 114);
        lv_obj_set_width(lv_obj_13, 15);
        lv_obj_set_height(lv_obj_13, 14);
        lv_obj_add_style(lv_obj_13, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_13, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_13_style_enabled_cb, lv_obj_13, NULL);

        lv_obj_t * lv_obj_14 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_14, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_14, 135);
        lv_obj_set_y(lv_obj_14, 114);
        lv_obj_set_width(lv_obj_14, 15);
        lv_obj_set_height(lv_obj_14, 14);
        lv_obj_add_style(lv_obj_14, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_14, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_14_style_enabled_cb, lv_obj_14, NULL);

        lv_obj_t * lv_obj_15 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_15, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_15, 152);
        lv_obj_set_y(lv_obj_15, 114);
        lv_obj_set_width(lv_obj_15, 15);
        lv_obj_set_height(lv_obj_15, 14);
        lv_obj_add_style(lv_obj_15, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_15, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_15_style_enabled_cb, lv_obj_15, NULL);

        lv_obj_t * lv_obj_16 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_16, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_16, 169);
        lv_obj_set_y(lv_obj_16, 114);
        lv_obj_set_width(lv_obj_16, 15);
        lv_obj_set_height(lv_obj_16, 14);
        lv_obj_add_style(lv_obj_16, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_16, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_16_style_enabled_cb, lv_obj_16, NULL);

        lv_obj_t * lv_obj_17 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_17, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_17, 186);
        lv_obj_set_y(lv_obj_17, 114);
        lv_obj_set_width(lv_obj_17, 15);
        lv_obj_set_height(lv_obj_17, 14);
        lv_obj_add_style(lv_obj_17, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_17, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_17_style_enabled_cb, lv_obj_17, NULL);

        lv_obj_t * lv_obj_18 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_18, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_18, 203);
        lv_obj_set_y(lv_obj_18, 114);
        lv_obj_set_width(lv_obj_18, 15);
        lv_obj_set_height(lv_obj_18, 14);
        lv_obj_add_style(lv_obj_18, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_18, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_speed, screen_hud_lv_obj_18_style_enabled_cb, lv_obj_18, NULL);

        lv_obj_t * lv_label_5 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_5, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_5, 16);
        lv_obj_set_y(lv_label_5, 136);
        lv_label_set_text(lv_label_5, "0");
        lv_obj_set_style_text_font(lv_label_5, font_num_20, 0);
        lv_obj_set_style_text_color(lv_label_5, COLOR_METER_MUTED, 0);

        lv_obj_t * lv_label_6 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_6, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_6, 106);
        lv_obj_set_y(lv_label_6, 136);
        lv_label_set_text(lv_label_6, "60");
        lv_obj_set_style_text_font(lv_label_6, font_num_20, 0);
        lv_obj_set_style_text_color(lv_label_6, COLOR_METER_MUTED, 0);

        lv_obj_t * lv_label_7 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_7, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_7, 185);
        lv_obj_set_y(lv_label_7, 136);
        lv_label_set_text(lv_label_7, "120");
        lv_obj_set_style_text_font(lv_label_7, font_num_20, 0);
        lv_obj_set_style_text_color(lv_label_7, COLOR_METER_MUTED, 0);

        lv_obj_t * pane_pos = lv_obj_create(stage_panes);
        lv_obj_set_name(pane_pos, "pane_pos");
        lv_obj_set_align(pane_pos, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(pane_pos, 240);
        lv_obj_set_y(pane_pos, 0);
        lv_obj_set_width(pane_pos, 240);
        lv_obj_set_height(pane_pos, 170);
        lv_obj_set_scrollable(pane_pos, false);
        lv_subject_add_observer_obj(subject_mode, screen_hud_pane_pos_checked_binding_cb, pane_pos, NULL);
        lv_obj_add_style(pane_pos, &style_meter_pane, 0);
        lv_obj_add_style(pane_pos, &style_meter_pane_next, LV_STATE_CHECKED);
        lv_obj_t * lv_label_8 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_8, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_8, 16);
        lv_obj_set_y(lv_label_8, 8);
        lv_label_set_text(lv_label_8, "经纬度");
        lv_obj_set_style_text_font(lv_label_8, font_cjk_22, 0);
        lv_obj_set_style_text_color(lv_label_8, COLOR_METER_INK, 0);
        lv_obj_set_style_text_letter_space(lv_label_8, 1, 0);

        lv_obj_t * lv_obj_19 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_19, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_19, 173);
        lv_obj_set_y(lv_obj_19, 22);
        lv_obj_set_width(lv_obj_19, 7);
        lv_obj_set_height(lv_obj_19, 8);
        lv_obj_add_style(lv_obj_19, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_19, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_19_style_enabled_cb, lv_obj_19, NULL);

        lv_obj_t * lv_obj_20 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_20, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_20, 184);
        lv_obj_set_y(lv_obj_20, 19);
        lv_obj_set_width(lv_obj_20, 7);
        lv_obj_set_height(lv_obj_20, 11);
        lv_obj_add_style(lv_obj_20, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_20, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_20_style_enabled_cb, lv_obj_20, NULL);

        lv_obj_t * lv_obj_21 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_21, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_21, 195);
        lv_obj_set_y(lv_obj_21, 16);
        lv_obj_set_width(lv_obj_21, 7);
        lv_obj_set_height(lv_obj_21, 14);
        lv_obj_add_style(lv_obj_21, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_21, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_21_style_enabled_cb, lv_obj_21, NULL);

        lv_obj_t * lv_obj_22 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_22, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_22, 206);
        lv_obj_set_y(lv_obj_22, 13);
        lv_obj_set_width(lv_obj_22, 7);
        lv_obj_set_height(lv_obj_22, 17);
        lv_obj_add_style(lv_obj_22, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_22, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_22_style_enabled_cb, lv_obj_22, NULL);

        lv_obj_t * lv_obj_23 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_23, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_23, 217);
        lv_obj_set_y(lv_obj_23, 10);
        lv_obj_set_width(lv_obj_23, 7);
        lv_obj_set_height(lv_obj_23, 20);
        lv_obj_add_style(lv_obj_23, &style_meter_seg_off, 0);
        lv_obj_add_style(lv_obj_23, &style_meter_seg_on, 0);
        screen_hud_style_meter_seg_on_ref = &style_meter_seg_on;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_23_style_enabled_cb, lv_obj_23, NULL);

        lv_obj_t * lv_label_9 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_9, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_9, 16);
        lv_obj_set_y(lv_label_9, 46);
        lv_label_set_text(lv_label_9, "纬度");
        lv_obj_set_style_text_font(lv_label_9, font_cjk_18, 0);
        lv_obj_set_style_text_color(lv_label_9, COLOR_METER_MUTED, 0);

        lv_obj_t * lv_label_10 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_10, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_10, 62);
        lv_obj_set_y(lv_label_10, 40);
        lv_subject_add_observer_obj(subject_lat, screen_hud_lv_label_10_text_binding_cb, lv_label_10, NULL);
        lv_obj_set_style_text_font(lv_label_10, font_num_30, 0);
        lv_obj_set_style_text_color(lv_label_10, COLOR_METER_INK, 0);

        lv_obj_t * lv_label_11 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_11, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_11, 16);
        lv_obj_set_y(lv_label_11, 86);
        lv_label_set_text(lv_label_11, "经度");
        lv_obj_set_style_text_font(lv_label_11, font_cjk_18, 0);
        lv_obj_set_style_text_color(lv_label_11, COLOR_METER_MUTED, 0);

        lv_obj_t * lv_label_12 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_12, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_12, 62);
        lv_obj_set_y(lv_label_12, 80);
        lv_subject_add_observer_obj(subject_lon, screen_hud_lv_label_12_text_binding_cb, lv_label_12, NULL);
        lv_obj_set_style_text_font(lv_label_12, font_num_30, 0);
        lv_obj_set_style_text_color(lv_label_12, COLOR_METER_INK, 0);

        lv_obj_t * lv_label_13 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_13, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_13, 16);
        lv_obj_set_y(lv_label_13, 134);
        lv_label_set_text(lv_label_13, "卫星");
        lv_obj_set_style_text_font(lv_label_13, font_cjk_18, 0);
        lv_obj_set_style_text_color(lv_label_13, COLOR_METER_MUTED, 0);

        lv_obj_t * lv_label_14 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_14, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_14, 62);
        lv_obj_set_y(lv_label_14, 135);
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_label_14_text_binding_cb, lv_label_14, NULL);
        lv_obj_set_style_text_font(lv_label_14, font_num_20, 0);
        lv_obj_set_style_text_color(lv_label_14, COLOR_METER_INK, 0);
        lv_obj_add_style(lv_label_14, &style_meter_warn, 0);
        screen_hud_style_meter_warn_ref = &style_meter_warn;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_label_14_style_enabled_cb, lv_label_14, NULL);

        lv_obj_t * lv_obj_24 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_24, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_24, 16);
        lv_obj_set_y(lv_obj_24, 256);
        lv_obj_set_width(lv_obj_24, 208);
        lv_obj_set_height(lv_obj_24, 2);
        lv_obj_add_style(lv_obj_24, &style_meter_rule, 0);

        lv_obj_t * lv_label_15 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_15, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_15, 62);
        lv_obj_set_y(lv_label_15, 270);
        lv_label_set_text(lv_label_15, "速度");
        lv_obj_set_style_text_font(lv_label_15, font_cjk_18, 0);
        lv_obj_add_style(lv_label_15, &style_meter_tab_idle, 0);
        lv_obj_add_style(lv_label_15, &style_meter_tab_on, 0);
        screen_hud_style_meter_tab_on_ref = &style_meter_tab_on;
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_label_15_style_enabled_cb, lv_label_15, NULL);

        lv_obj_t * lv_obj_25 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_25, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_25, 110);
        lv_obj_set_y(lv_obj_25, 272);
        lv_obj_set_width(lv_obj_25, 2);
        lv_obj_set_height(lv_obj_25, 14);
        lv_obj_add_style(lv_obj_25, &style_meter_rule, 0);

        lv_obj_t * lv_label_16 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_16, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_16, 124);
        lv_obj_set_y(lv_label_16, 270);
        lv_label_set_text(lv_label_16, "经纬度");
        lv_obj_set_style_text_font(lv_label_16, font_cjk_18, 0);
        lv_obj_add_style(lv_label_16, &style_meter_tab_idle, 0);
        lv_obj_add_style(lv_label_16, &style_meter_tab_on, 0);
        screen_hud_style_meter_tab_on_ref = &style_meter_tab_on;
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_label_16_style_enabled_cb, lv_label_16, NULL);

        lv_obj_t * lv_obj_26 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_26, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_26, 62);
        lv_obj_set_y(lv_obj_26, 293);
        lv_obj_set_width(lv_obj_26, 36);
        lv_obj_set_height(lv_obj_26, 3);
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_obj_26_hidden_binding_cb, lv_obj_26, NULL);
        lv_obj_add_style(lv_obj_26, &style_meter_underline, 0);

        lv_obj_t * lv_obj_27 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_27, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_27, 124);
        lv_obj_set_y(lv_obj_27, 293);
        lv_obj_set_width(lv_obj_27, 54);
        lv_obj_set_height(lv_obj_27, 3);
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_obj_27_hidden_binding_cb, lv_obj_27, NULL);
        lv_obj_add_style(lv_obj_27, &style_meter_underline, 0);

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

static void screen_hud_lv_label_0_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_label_set_text(obj, lv_subject_get_string(subject_time));
}

static void screen_hud_lv_label_1_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_label_set_text(obj, lv_subject_get_string(subject_date));
}

static void screen_hud_lv_obj_1_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_batt) > 0);
}

static void screen_hud_lv_obj_1_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_low_ref, 0, lv_subject_get_int(subject_batt) <= 20);
}

static void screen_hud_lv_obj_2_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_batt) > 20);
}

static void screen_hud_lv_obj_2_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_low_ref, 0, lv_subject_get_int(subject_batt) <= 20);
}

static void screen_hud_lv_obj_3_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_batt) > 40);
}

static void screen_hud_lv_obj_3_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_low_ref, 0, lv_subject_get_int(subject_batt) <= 20);
}

static void screen_hud_lv_obj_4_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_batt) > 60);
}

static void screen_hud_lv_obj_4_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_low_ref, 0, lv_subject_get_int(subject_batt) <= 20);
}

static void screen_hud_lv_obj_5_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_batt) > 80);
}

static void screen_hud_lv_obj_5_style_enabled_cb_2(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_low_ref, 0, lv_subject_get_int(subject_batt) <= 20);
}

static void screen_hud_pane_speed_checked_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_checked(obj, lv_subject_get_int(subject_mode) == 1);
}

static void screen_hud_lv_label_4_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_label_set_text(obj, lv_subject_get_string(subject_speed_txt));
}

static void screen_hud_lv_obj_7_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 5);
}

static void screen_hud_lv_obj_8_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 15);
}

static void screen_hud_lv_obj_9_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 25);
}

static void screen_hud_lv_obj_10_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 35);
}

static void screen_hud_lv_obj_11_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 45);
}

static void screen_hud_lv_obj_12_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 55);
}

static void screen_hud_lv_obj_13_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 65);
}

static void screen_hud_lv_obj_14_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 75);
}

static void screen_hud_lv_obj_15_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 85);
}

static void screen_hud_lv_obj_16_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 95);
}

static void screen_hud_lv_obj_17_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 105);
}

static void screen_hud_lv_obj_18_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_speed) >= 115);
}

static void screen_hud_pane_pos_checked_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_checked(obj, lv_subject_get_int(subject_mode) == 1);
}

static void screen_hud_lv_obj_19_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_sat) >= 2);
}

static void screen_hud_lv_obj_20_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_sat) >= 4);
}

static void screen_hud_lv_obj_21_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_sat) >= 6);
}

static void screen_hud_lv_obj_22_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_sat) >= 8);
}

static void screen_hud_lv_obj_23_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_seg_on_ref, 0, lv_subject_get_int(subject_sat) >= 10);
}

static void screen_hud_lv_label_10_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_label_set_text(obj, lv_subject_get_string(subject_lat));
}

static void screen_hud_lv_label_12_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_label_set_text(obj, lv_subject_get_string(subject_lon));
}

static void screen_hud_lv_label_14_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    char eval_buf_1[LV_XML_EVAL_STRING_BUF_SIZE];
    lv_snprintf(eval_buf_1, sizeof(eval_buf_1), "%d", (int)(lv_subject_get_int(subject_sat)));
    lv_label_set_text(obj, eval_buf_1);
}

static void screen_hud_lv_label_14_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_warn_ref, 0, lv_subject_get_int(subject_sat) < 4);
}

static void screen_hud_lv_label_15_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_tab_on_ref, 0, lv_subject_get_int(subject_mode) == 0);
}

static void screen_hud_lv_label_16_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_meter_tab_on_ref, 0, lv_subject_get_int(subject_mode) == 1);
}

static void screen_hud_lv_obj_26_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_hidden(obj, lv_subject_get_int(subject_mode) == 1);
}

static void screen_hud_lv_obj_27_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_hidden(obj, lv_subject_get_int(subject_mode) == 0);
}

