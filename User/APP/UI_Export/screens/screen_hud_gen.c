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

static lv_style_t * screen_hud_style_ice_block_on_ref; /* set in create(), read by a binding callback */
static lv_style_t * screen_hud_style_ice_warn_text_ref; /* set in create(), read by a binding callback */
static lv_style_t * screen_hud_style_ice_block_hi_ref; /* set in create(), read by a binding callback */
static void screen_hud_lv_label_0_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_1_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_9_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_10_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_11_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_12_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_13_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_pane_speed_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_3_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_pane_pos_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_10_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_12_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_14_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_14_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_28_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_29_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_30_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_31_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_32_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_34_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_obj_35_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_15_style_text_color_binding_cb(lv_observer_t * observer, lv_subject_t * subject);
static void screen_hud_lv_label_16_style_text_color_binding_cb(lv_observer_t * observer, lv_subject_t * subject);

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

        lv_obj_add_style(lv_obj_0, &style_screen_ice, 0);
        lv_obj_t * lv_obj_1 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_1, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_1, 7);
        lv_obj_set_y(lv_obj_1, 7);
        lv_obj_set_width(lv_obj_1, 12);
        lv_obj_set_height(lv_obj_1, 2);
        lv_obj_add_style(lv_obj_1, &style_ice_frame, 0);

        lv_obj_t * lv_obj_2 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_2, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_2, 7);
        lv_obj_set_y(lv_obj_2, 7);
        lv_obj_set_width(lv_obj_2, 2);
        lv_obj_set_height(lv_obj_2, 12);
        lv_obj_add_style(lv_obj_2, &style_ice_frame, 0);

        lv_obj_t * lv_obj_3 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_3, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_3, 153);
        lv_obj_set_y(lv_obj_3, 7);
        lv_obj_set_width(lv_obj_3, 12);
        lv_obj_set_height(lv_obj_3, 2);
        lv_obj_add_style(lv_obj_3, &style_ice_frame, 0);

        lv_obj_t * lv_obj_4 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_4, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_4, 163);
        lv_obj_set_y(lv_obj_4, 7);
        lv_obj_set_width(lv_obj_4, 2);
        lv_obj_set_height(lv_obj_4, 12);
        lv_obj_add_style(lv_obj_4, &style_ice_frame, 0);

        lv_obj_t * lv_obj_5 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_5, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_5, 7);
        lv_obj_set_y(lv_obj_5, 311);
        lv_obj_set_width(lv_obj_5, 12);
        lv_obj_set_height(lv_obj_5, 2);
        lv_obj_add_style(lv_obj_5, &style_ice_frame, 0);

        lv_obj_t * lv_obj_6 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_6, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_6, 7);
        lv_obj_set_y(lv_obj_6, 301);
        lv_obj_set_width(lv_obj_6, 2);
        lv_obj_set_height(lv_obj_6, 12);
        lv_obj_add_style(lv_obj_6, &style_ice_frame, 0);

        lv_obj_t * lv_obj_7 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_7, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_7, 153);
        lv_obj_set_y(lv_obj_7, 311);
        lv_obj_set_width(lv_obj_7, 12);
        lv_obj_set_height(lv_obj_7, 2);
        lv_obj_add_style(lv_obj_7, &style_ice_frame, 0);

        lv_obj_t * lv_obj_8 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_8, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_8, 163);
        lv_obj_set_y(lv_obj_8, 301);
        lv_obj_set_width(lv_obj_8, 2);
        lv_obj_set_height(lv_obj_8, 12);
        lv_obj_add_style(lv_obj_8, &style_ice_frame, 0);

        lv_obj_t * lv_label_0 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_0, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_0, 14);
        lv_obj_set_y(lv_label_0, 14);
        lv_subject_add_observer_obj(subject_time, screen_hud_lv_label_0_text_binding_cb, lv_label_0, NULL);
        lv_obj_set_style_text_font(lv_label_0, font_num_36, 0);
        lv_obj_set_style_text_color(lv_label_0, COLOR_ICE_INK, 0);

        lv_obj_t * lv_label_1 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_1, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_1, 14);
        lv_obj_set_y(lv_label_1, 52);
        lv_subject_add_observer_obj(subject_date, screen_hud_lv_label_1_text_binding_cb, lv_label_1, NULL);
        lv_obj_set_style_text_letter_space(lv_label_1, 1, 0);
        lv_obj_set_style_text_color(lv_label_1, COLOR_ICE_MUTED, 0);

        lv_obj_t * lv_obj_9 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_9, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_9, 125);
        lv_obj_set_y(lv_obj_9, 55);
        lv_obj_set_width(lv_obj_9, 5);
        lv_obj_set_height(lv_obj_9, 10);
        lv_obj_add_style(lv_obj_9, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_9, &style_ice_block_on, 0);
        screen_hud_style_ice_block_on_ref = &style_ice_block_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_9_style_enabled_cb, lv_obj_9, NULL);

        lv_obj_t * lv_obj_10 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_10, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_10, 132);
        lv_obj_set_y(lv_obj_10, 55);
        lv_obj_set_width(lv_obj_10, 5);
        lv_obj_set_height(lv_obj_10, 10);
        lv_obj_add_style(lv_obj_10, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_10, &style_ice_block_on, 0);
        screen_hud_style_ice_block_on_ref = &style_ice_block_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_10_style_enabled_cb, lv_obj_10, NULL);

        lv_obj_t * lv_obj_11 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_11, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_11, 139);
        lv_obj_set_y(lv_obj_11, 55);
        lv_obj_set_width(lv_obj_11, 5);
        lv_obj_set_height(lv_obj_11, 10);
        lv_obj_add_style(lv_obj_11, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_11, &style_ice_block_on, 0);
        screen_hud_style_ice_block_on_ref = &style_ice_block_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_11_style_enabled_cb, lv_obj_11, NULL);

        lv_obj_t * lv_obj_12 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_12, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_12, 146);
        lv_obj_set_y(lv_obj_12, 55);
        lv_obj_set_width(lv_obj_12, 5);
        lv_obj_set_height(lv_obj_12, 10);
        lv_obj_add_style(lv_obj_12, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_12, &style_ice_block_on, 0);
        screen_hud_style_ice_block_on_ref = &style_ice_block_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_12_style_enabled_cb, lv_obj_12, NULL);

        lv_obj_t * lv_obj_13 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_13, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_13, 153);
        lv_obj_set_y(lv_obj_13, 55);
        lv_obj_set_width(lv_obj_13, 5);
        lv_obj_set_height(lv_obj_13, 10);
        lv_obj_add_style(lv_obj_13, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_13, &style_ice_block_on, 0);
        screen_hud_style_ice_block_on_ref = &style_ice_block_on;
        lv_subject_add_observer_obj(subject_batt, screen_hud_lv_obj_13_style_enabled_cb, lv_obj_13, NULL);

        lv_obj_t * lv_obj_14 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_14, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_14, 14);
        lv_obj_set_y(lv_obj_14, 70);
        lv_obj_set_width(lv_obj_14, 144);
        lv_obj_set_height(lv_obj_14, 1);
        lv_obj_add_style(lv_obj_14, &style_ice_hair, 0);

        lv_obj_t * pane_speed = lv_obj_create(lv_obj_0);
        lv_obj_set_name(pane_speed, "pane_speed");
        lv_obj_set_align(pane_speed, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(pane_speed, 0);
        lv_obj_set_y(pane_speed, 76);
        lv_obj_set_width(pane_speed, 172);
        lv_obj_set_height(pane_speed, 160);
        lv_obj_set_scrollable(pane_speed, false);
        lv_subject_add_observer_obj(subject_mode, screen_hud_pane_speed_hidden_binding_cb, pane_speed, NULL);
        lv_obj_add_style(pane_speed, &style_ice_pane, 0);
        lv_obj_t * lv_label_2 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_2, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_2, 14);
        lv_obj_set_y(lv_label_2, 4);
        lv_label_set_text(lv_label_2, "速度");
        lv_obj_set_style_text_font(lv_label_2, font_cjk_16, 0);
        lv_obj_set_style_text_letter_space(lv_label_2, 2, 0);
        lv_obj_set_style_text_color(lv_label_2, COLOR_ICE_ACCENT, 0);

        lv_obj_t * lv_label_3 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_3, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_3, 14);
        lv_obj_set_y(lv_label_3, 26);
        lv_subject_add_observer_obj(subject_speed_txt, screen_hud_lv_label_3_text_binding_cb, lv_label_3, NULL);
        lv_obj_set_style_text_font(lv_label_3, font_num_44, 0);
        lv_obj_set_style_text_color(lv_label_3, COLOR_ICE_INK, 0);

        lv_obj_t * lv_label_4 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_4, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_4, 14);
        lv_obj_set_y(lv_label_4, 74);
        lv_label_set_text(lv_label_4, "KM/H");
        lv_obj_set_style_text_letter_space(lv_label_4, 2, 0);
        lv_obj_set_style_text_color(lv_label_4, COLOR_ICE_MUTED, 0);

        lv_obj_t * lv_obj_15 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_15, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_15, 14);
        lv_obj_set_y(lv_obj_15, 106);
        lv_obj_set_width(lv_obj_15, 1);
        lv_obj_set_height(lv_obj_15, 12);
        lv_obj_add_style(lv_obj_15, &style_ice_tick, 0);

        lv_obj_t * lv_obj_16 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_16, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_16, 26);
        lv_obj_set_y(lv_obj_16, 112);
        lv_obj_set_width(lv_obj_16, 1);
        lv_obj_set_height(lv_obj_16, 6);
        lv_obj_add_style(lv_obj_16, &style_ice_tick, 0);

        lv_obj_t * lv_obj_17 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_17, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_17, 38);
        lv_obj_set_y(lv_obj_17, 106);
        lv_obj_set_width(lv_obj_17, 1);
        lv_obj_set_height(lv_obj_17, 12);
        lv_obj_add_style(lv_obj_17, &style_ice_tick, 0);

        lv_obj_t * lv_obj_18 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_18, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_18, 50);
        lv_obj_set_y(lv_obj_18, 112);
        lv_obj_set_width(lv_obj_18, 1);
        lv_obj_set_height(lv_obj_18, 6);
        lv_obj_add_style(lv_obj_18, &style_ice_tick, 0);

        lv_obj_t * lv_obj_19 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_19, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_19, 62);
        lv_obj_set_y(lv_obj_19, 106);
        lv_obj_set_width(lv_obj_19, 1);
        lv_obj_set_height(lv_obj_19, 12);
        lv_obj_add_style(lv_obj_19, &style_ice_tick, 0);

        lv_obj_t * lv_obj_20 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_20, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_20, 74);
        lv_obj_set_y(lv_obj_20, 112);
        lv_obj_set_width(lv_obj_20, 1);
        lv_obj_set_height(lv_obj_20, 6);
        lv_obj_add_style(lv_obj_20, &style_ice_tick, 0);

        lv_obj_t * lv_obj_21 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_21, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_21, 86);
        lv_obj_set_y(lv_obj_21, 106);
        lv_obj_set_width(lv_obj_21, 1);
        lv_obj_set_height(lv_obj_21, 12);
        lv_obj_add_style(lv_obj_21, &style_ice_tick, 0);

        lv_obj_t * lv_obj_22 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_22, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_22, 98);
        lv_obj_set_y(lv_obj_22, 112);
        lv_obj_set_width(lv_obj_22, 1);
        lv_obj_set_height(lv_obj_22, 6);
        lv_obj_add_style(lv_obj_22, &style_ice_tick, 0);

        lv_obj_t * lv_obj_23 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_23, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_23, 110);
        lv_obj_set_y(lv_obj_23, 106);
        lv_obj_set_width(lv_obj_23, 1);
        lv_obj_set_height(lv_obj_23, 12);
        lv_obj_add_style(lv_obj_23, &style_ice_tick, 0);

        lv_obj_t * lv_obj_24 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_24, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_24, 122);
        lv_obj_set_y(lv_obj_24, 112);
        lv_obj_set_width(lv_obj_24, 1);
        lv_obj_set_height(lv_obj_24, 6);
        lv_obj_add_style(lv_obj_24, &style_ice_tick, 0);

        lv_obj_t * lv_obj_25 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_25, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_25, 134);
        lv_obj_set_y(lv_obj_25, 106);
        lv_obj_set_width(lv_obj_25, 1);
        lv_obj_set_height(lv_obj_25, 12);
        lv_obj_add_style(lv_obj_25, &style_ice_tick, 0);

        lv_obj_t * lv_obj_26 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_26, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_26, 146);
        lv_obj_set_y(lv_obj_26, 112);
        lv_obj_set_width(lv_obj_26, 1);
        lv_obj_set_height(lv_obj_26, 6);
        lv_obj_add_style(lv_obj_26, &style_ice_tick, 0);

        lv_obj_t * lv_obj_27 = lv_obj_create(pane_speed);
        lv_obj_set_align(lv_obj_27, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_27, 158);
        lv_obj_set_y(lv_obj_27, 106);
        lv_obj_set_width(lv_obj_27, 1);
        lv_obj_set_height(lv_obj_27, 12);
        lv_obj_add_style(lv_obj_27, &style_ice_tick, 0);

        lv_obj_t * lv_bar_0 = lv_bar_create(pane_speed);
        lv_obj_set_align(lv_bar_0, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_bar_0, 14);
        lv_obj_set_y(lv_bar_0, 120);
        lv_obj_set_width(lv_bar_0, 144);
        lv_obj_set_height(lv_bar_0, 4);
        lv_bar_set_min_value(lv_bar_0, 0);
        lv_bar_set_max_value(lv_bar_0, 120);
        lv_bar_bind_value(lv_bar_0, subject_speed);
        lv_obj_set_style_pad_all(lv_bar_0, 0, 0);
        lv_obj_set_style_radius(lv_bar_0, 0, 0);
        lv_obj_set_style_radius(lv_bar_0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(lv_bar_0, 0, 0);
        lv_obj_set_style_outline_width(lv_bar_0, 0, 0);
        lv_obj_set_style_shadow_width(lv_bar_0, 0, 0);
        lv_obj_set_style_bg_color(lv_bar_0, COLOR_ICE_TRACK, 0);
        lv_obj_set_style_bg_opa(lv_bar_0, (255 * 100 / 100), 0);
        lv_obj_set_style_bg_color(lv_bar_0, COLOR_ICE_HI, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(lv_bar_0, (255 * 100 / 100), LV_PART_INDICATOR);

        lv_obj_t * lv_label_5 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_5, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_5, 14);
        lv_obj_set_y(lv_label_5, 128);
        lv_label_set_text(lv_label_5, "0");
        lv_obj_set_style_text_color(lv_label_5, COLOR_ICE_MUTED, 0);

        lv_obj_t * lv_label_6 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_6, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_6, 77);
        lv_obj_set_y(lv_label_6, 128);
        lv_label_set_text(lv_label_6, "60");
        lv_obj_set_style_text_color(lv_label_6, COLOR_ICE_MUTED, 0);

        lv_obj_t * lv_label_7 = lv_label_create(pane_speed);
        lv_obj_set_align(lv_label_7, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_7, 131);
        lv_obj_set_y(lv_label_7, 128);
        lv_label_set_text(lv_label_7, "120");
        lv_obj_set_style_text_color(lv_label_7, COLOR_ICE_MUTED, 0);

        lv_obj_t * pane_pos = lv_obj_create(lv_obj_0);
        lv_obj_set_name(pane_pos, "pane_pos");
        lv_obj_set_align(pane_pos, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(pane_pos, 0);
        lv_obj_set_y(pane_pos, 76);
        lv_obj_set_width(pane_pos, 172);
        lv_obj_set_height(pane_pos, 160);
        lv_obj_set_scrollable(pane_pos, false);
        lv_subject_add_observer_obj(subject_mode, screen_hud_pane_pos_hidden_binding_cb, pane_pos, NULL);
        lv_obj_add_style(pane_pos, &style_ice_pane, 0);
        lv_obj_t * lv_label_8 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_8, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_8, 14);
        lv_obj_set_y(lv_label_8, 4);
        lv_label_set_text(lv_label_8, "经纬度");
        lv_obj_set_style_text_font(lv_label_8, font_cjk_16, 0);
        lv_obj_set_style_text_letter_space(lv_label_8, 1, 0);
        lv_obj_set_style_text_color(lv_label_8, COLOR_ICE_ACCENT, 0);

        lv_obj_t * lv_label_9 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_9, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_9, 14);
        lv_obj_set_y(lv_label_9, 24);
        lv_label_set_text(lv_label_9, "纬度");
        lv_obj_set_style_text_font(lv_label_9, font_cjk_16, 0);
        lv_obj_set_style_text_letter_space(lv_label_9, 1, 0);
        lv_obj_set_style_text_color(lv_label_9, COLOR_ICE_MUTED, 0);

        lv_obj_t * lv_label_10 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_10, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_10, 14);
        lv_obj_set_y(lv_label_10, 42);
        lv_subject_add_observer_obj(subject_lat, screen_hud_lv_label_10_text_binding_cb, lv_label_10, NULL);
        lv_obj_set_style_text_font(lv_label_10, font_num_28, 0);
        lv_obj_set_style_text_color(lv_label_10, COLOR_ICE_INK, 0);

        lv_obj_t * lv_label_11 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_11, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_11, 14);
        lv_obj_set_y(lv_label_11, 78);
        lv_label_set_text(lv_label_11, "经度");
        lv_obj_set_style_text_font(lv_label_11, font_cjk_16, 0);
        lv_obj_set_style_text_letter_space(lv_label_11, 1, 0);
        lv_obj_set_style_text_color(lv_label_11, COLOR_ICE_MUTED, 0);

        lv_obj_t * lv_label_12 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_12, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_12, 14);
        lv_obj_set_y(lv_label_12, 96);
        lv_subject_add_observer_obj(subject_lon, screen_hud_lv_label_12_text_binding_cb, lv_label_12, NULL);
        lv_obj_set_style_text_font(lv_label_12, font_num_28, 0);
        lv_obj_set_style_text_color(lv_label_12, COLOR_ICE_INK, 0);

        lv_obj_t * lv_label_13 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_13, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_13, 14);
        lv_obj_set_y(lv_label_13, 130);
        lv_label_set_text(lv_label_13, "卫星");
        lv_obj_set_style_text_font(lv_label_13, font_cjk_16, 0);
        lv_obj_set_style_text_letter_space(lv_label_13, 1, 0);
        lv_obj_set_style_text_color(lv_label_13, COLOR_ICE_MUTED, 0);

        lv_obj_t * lv_label_14 = lv_label_create(pane_pos);
        lv_obj_set_align(lv_label_14, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_14, 54);
        lv_obj_set_y(lv_label_14, 130);
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_label_14_text_binding_cb, lv_label_14, NULL);
        lv_obj_set_style_text_color(lv_label_14, COLOR_ICE_INK, 0);
        lv_obj_add_style(lv_label_14, &style_ice_warn_text, 0);
        screen_hud_style_ice_warn_text_ref = &style_ice_warn_text;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_label_14_style_enabled_cb, lv_label_14, NULL);

        lv_obj_t * lv_obj_28 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_28, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_28, 116);
        lv_obj_set_y(lv_obj_28, 142);
        lv_obj_set_width(lv_obj_28, 6);
        lv_obj_set_height(lv_obj_28, 6);
        lv_obj_add_style(lv_obj_28, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_28, &style_ice_block_hi, 0);
        screen_hud_style_ice_block_hi_ref = &style_ice_block_hi;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_28_style_enabled_cb, lv_obj_28, NULL);

        lv_obj_t * lv_obj_29 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_29, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_29, 125);
        lv_obj_set_y(lv_obj_29, 139);
        lv_obj_set_width(lv_obj_29, 6);
        lv_obj_set_height(lv_obj_29, 9);
        lv_obj_add_style(lv_obj_29, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_29, &style_ice_block_hi, 0);
        screen_hud_style_ice_block_hi_ref = &style_ice_block_hi;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_29_style_enabled_cb, lv_obj_29, NULL);

        lv_obj_t * lv_obj_30 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_30, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_30, 134);
        lv_obj_set_y(lv_obj_30, 136);
        lv_obj_set_width(lv_obj_30, 6);
        lv_obj_set_height(lv_obj_30, 12);
        lv_obj_add_style(lv_obj_30, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_30, &style_ice_block_hi, 0);
        screen_hud_style_ice_block_hi_ref = &style_ice_block_hi;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_30_style_enabled_cb, lv_obj_30, NULL);

        lv_obj_t * lv_obj_31 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_31, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_31, 143);
        lv_obj_set_y(lv_obj_31, 132);
        lv_obj_set_width(lv_obj_31, 6);
        lv_obj_set_height(lv_obj_31, 16);
        lv_obj_add_style(lv_obj_31, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_31, &style_ice_block_hi, 0);
        screen_hud_style_ice_block_hi_ref = &style_ice_block_hi;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_31_style_enabled_cb, lv_obj_31, NULL);

        lv_obj_t * lv_obj_32 = lv_obj_create(pane_pos);
        lv_obj_set_align(lv_obj_32, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_32, 152);
        lv_obj_set_y(lv_obj_32, 128);
        lv_obj_set_width(lv_obj_32, 6);
        lv_obj_set_height(lv_obj_32, 20);
        lv_obj_add_style(lv_obj_32, &style_ice_block_off, 0);
        lv_obj_add_style(lv_obj_32, &style_ice_block_hi, 0);
        screen_hud_style_ice_block_hi_ref = &style_ice_block_hi;
        lv_subject_add_observer_obj(subject_sat, screen_hud_lv_obj_32_style_enabled_cb, lv_obj_32, NULL);

        lv_obj_t * lv_obj_33 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_33, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_33, 14);
        lv_obj_set_y(lv_obj_33, 254);
        lv_obj_set_width(lv_obj_33, 144);
        lv_obj_set_height(lv_obj_33, 1);
        lv_obj_add_style(lv_obj_33, &style_ice_hair, 0);

        lv_obj_t * lv_obj_34 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_34, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_34, 32);
        lv_obj_set_y(lv_obj_34, 253);
        lv_obj_set_width(lv_obj_34, 36);
        lv_obj_set_height(lv_obj_34, 3);
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_obj_34_hidden_binding_cb, lv_obj_34, NULL);
        lv_obj_set_style_bg_color(lv_obj_34, COLOR_ICE_ACCENT, 0);
        lv_obj_set_style_bg_opa(lv_obj_34, (255 * 100 / 100), 0);
        lv_obj_set_style_radius(lv_obj_34, 0, 0);
        lv_obj_set_style_border_width(lv_obj_34, 0, 0);

        lv_obj_t * lv_obj_35 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_35, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_35, 96);
        lv_obj_set_y(lv_obj_35, 253);
        lv_obj_set_width(lv_obj_35, 51);
        lv_obj_set_height(lv_obj_35, 3);
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_obj_35_hidden_binding_cb, lv_obj_35, NULL);
        lv_obj_set_style_bg_color(lv_obj_35, COLOR_ICE_ACCENT, 0);
        lv_obj_set_style_bg_opa(lv_obj_35, (255 * 100 / 100), 0);
        lv_obj_set_style_radius(lv_obj_35, 0, 0);
        lv_obj_set_style_border_width(lv_obj_35, 0, 0);

        lv_obj_t * lv_obj_36 = lv_obj_create(lv_obj_0);
        lv_obj_set_align(lv_obj_36, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_obj_36, 86);
        lv_obj_set_y(lv_obj_36, 254);
        lv_obj_set_width(lv_obj_36, 1);
        lv_obj_set_height(lv_obj_36, 32);
        lv_obj_add_style(lv_obj_36, &style_ice_hair, 0);

        lv_obj_t * lv_label_15 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_15, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_15, 32);
        lv_obj_set_y(lv_label_15, 264);
        lv_label_set_text(lv_label_15, "速度");
        lv_obj_set_style_text_font(lv_label_15, font_cjk_16, 0);
        lv_obj_set_style_text_letter_space(lv_label_15, 2, 0);
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_label_15_style_text_color_binding_cb, lv_label_15, NULL);

        lv_obj_t * lv_label_16 = lv_label_create(lv_obj_0);
        lv_obj_set_align(lv_label_16, LV_ALIGN_TOP_LEFT);
        lv_obj_set_x(lv_label_16, 96);
        lv_obj_set_y(lv_label_16, 264);
        lv_label_set_text(lv_label_16, "经纬度");
        lv_obj_set_style_text_font(lv_label_16, font_cjk_16, 0);
        lv_obj_set_style_text_letter_space(lv_label_16, 1, 0);
        lv_subject_add_observer_obj(subject_mode, screen_hud_lv_label_16_style_text_color_binding_cb, lv_label_16, NULL);

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

static void screen_hud_lv_obj_9_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_on_ref, 0, lv_subject_get_int(subject_batt) > 0);
}

static void screen_hud_lv_obj_10_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_on_ref, 0, lv_subject_get_int(subject_batt) > 20);
}

static void screen_hud_lv_obj_11_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_on_ref, 0, lv_subject_get_int(subject_batt) > 40);
}

static void screen_hud_lv_obj_12_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_on_ref, 0, lv_subject_get_int(subject_batt) > 60);
}

static void screen_hud_lv_obj_13_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_on_ref, 0, lv_subject_get_int(subject_batt) > 80);
}

static void screen_hud_pane_speed_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_hidden(obj, lv_subject_get_int(subject_mode) == 1);
}

static void screen_hud_lv_label_3_text_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_label_set_text(obj, lv_subject_get_string(subject_speed_txt));
}

static void screen_hud_pane_pos_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_hidden(obj, lv_subject_get_int(subject_mode) == 0);
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
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_warn_text_ref, 0, lv_subject_get_int(subject_sat) < 4);
}

static void screen_hud_lv_obj_28_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_hi_ref, 0, lv_subject_get_int(subject_sat) >= 2);
}

static void screen_hud_lv_obj_29_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_hi_ref, 0, lv_subject_get_int(subject_sat) >= 4);
}

static void screen_hud_lv_obj_30_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_hi_ref, 0, lv_subject_get_int(subject_sat) >= 6);
}

static void screen_hud_lv_obj_31_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_hi_ref, 0, lv_subject_get_int(subject_sat) >= 8);
}

static void screen_hud_lv_obj_32_style_enabled_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_enabled(obj, screen_hud_style_ice_block_hi_ref, 0, lv_subject_get_int(subject_sat) >= 10);
}

static void screen_hud_lv_obj_34_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_hidden(obj, lv_subject_get_int(subject_mode) == 1);
}

static void screen_hud_lv_obj_35_hidden_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_hidden(obj, lv_subject_get_int(subject_mode) == 0);
}

static void screen_hud_lv_label_15_style_text_color_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_text_color(obj, lv_subject_get_int(subject_mode) == 0 ? lv_color_hex(0x0A1B28) : lv_color_hex(0x5A7386), 0);
}

static void screen_hud_lv_label_16_style_text_color_binding_cb(lv_observer_t * observer, lv_subject_t * subject)
{
    LV_UNUSED(subject);
    lv_obj_t * obj = lv_observer_get_target_obj(observer);
    lv_obj_set_style_text_color(obj, lv_subject_get_int(subject_mode) == 1 ? lv_color_hex(0x0A1B28) : lv_color_hex(0x5A7386), 0);
}

