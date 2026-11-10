/**
 * @file lvgl_ui_project_gen.h
 */

#ifndef LVGL_PRO_LVGL_UI_PROJECT_GEN_H
#define LVGL_PRO_LVGL_UI_PROJECT_GEN_H

#ifndef UI_SUBJECT_STRING_LENGTH
#define UI_SUBJECT_STRING_LENGTH 256
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#if defined(LV_USE_XML) && LV_USE_XML
    #include "lv_xml/lv_xml.h"
#endif



/* Prototypes for target functions, needed by responsive const definitions */

void lvgl_ui_project_set_target(uint32_t target);
uint32_t lvgl_ui_project_get_target(void);
bool lvgl_ui_project_check_target(uint32_t target);

/*********************
 *      DEFINES
 *********************/

#define LVGL_UI_PROJECT_TARGET_UNDEFINED  (0 << 1)
#define LVGL_UI_PROJECT_TARGET_TARGET1    (1 << 1)
#define LVGL_UI_PROJECT_TARGET_ALL        0x0FFFFFFF

/* By default compile for all targets, allowing to switch to any targets at runtime */
#ifndef LVGL_UI_PROJECT_COMPILE_TARGET
#define LVGL_UI_PROJECT_COMPILE_TARGET LVGL_UI_PROJECT_TARGET_ALL
#endif

#define LVGL_UI_PROJECT_CHECK_COMPILE_TARGET(target) (LVGL_UI_PROJECT_COMPILE_TARGET & (target) ? 1 : 0)

#define SPACE_XS (2)
#define SPACE_SM (4)
#define SPACE_MD (8)
#define SPACE_LG (12)
#define RADIUS_MD (10)
#define ARC_THICKNESS (14)
#define COLOR_BG (lv_color_hex(0x0E1420))
#define COLOR_TEXT (lv_color_hex(0xFFFFFF))
#define COLOR_SOFT (lv_color_hex(0xD8E2EE))
#define COLOR_MUTED (lv_color_hex(0xA8B8CC))
#define COLOR_ACCENT (lv_color_hex(0x3EE0A6))
#define COLOR_TRACK (lv_color_hex(0x22304A))
#define COLOR_PAPER_BG (lv_color_hex(0xF7F5EF))
#define COLOR_PAPER_INK (lv_color_hex(0x14120E))
#define COLOR_PAPER_MUTED (lv_color_hex(0x6B6459))
#define COLOR_PAPER_ACCENT (lv_color_hex(0xC8452D))
#define COLOR_PAPER_TRACK (lv_color_hex(0xDCD6C8))
#define COLOR_HUD_BG (lv_color_hex(0x0B0E11))
#define COLOR_HUD_LIT (lv_color_hex(0xFFB020))
#define COLOR_HUD_OFF (lv_color_hex(0x232B34))
#define COLOR_HUD_SOFT (lv_color_hex(0x9FB0C0))
#define COLOR_HUD_TEXT (lv_color_hex(0xE8EDF2))
#define COLOR_ICE_BG (lv_color_hex(0xF2F6FA))
#define COLOR_ICE_INK (lv_color_hex(0x0A1B28))
#define COLOR_ICE_MUTED (lv_color_hex(0x5A7386))
#define COLOR_ICE_HAIR (lv_color_hex(0xC9DAE6))
#define COLOR_ICE_TICK (lv_color_hex(0x8FA9BC))
#define COLOR_ICE_TRACK (lv_color_hex(0xD3E1EC))
#define COLOR_ICE_ACCENT (lv_color_hex(0x0B7285))
#define COLOR_ICE_HI (lv_color_hex(0x14B8CC))
#define COLOR_ICE_WARN (lv_color_hex(0xD9611F))
#define COLOR_ICE_FRAME (lv_color_hex(0xA8C2D0))


#ifndef LV_XML_EVAL_STRING_BUF_SIZE
    #define LV_XML_EVAL_STRING_BUF_SIZE 256
#endif

/**
 * Animation path of a style transition with a `bezier(…)` easing. The control
 * points come from the transition descriptor's `user_data`, as a transition
 * descriptor cannot store animation parameters itself.
 */
static inline int32_t lvgl_ui_project_style_transition_bezier_path_helper(const lv_anim_t * a)
{
    const lv_anim_bezier3_para_t * para = (const lv_anim_bezier3_para_t *)lv_anim_get_user_data(a);
    if(para == NULL) return lv_anim_path_linear(a);

    lv_anim_t a_tmp = *a;
    lv_anim_set_bezier3_param(&a_tmp, para->x1, para->y1, para->x2, para->y2);
    return lv_anim_path_custom_bezier3(&a_tmp);
}

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL VARIABLES
 **********************/

/*-------------------
 * Permanent screens
 *------------------*/

extern lv_obj_t * screen_hud;

/*----------------
 * Global styles
 *----------------*/

extern lv_style_t style_screen;
extern lv_style_t style_screen_paper;
extern lv_style_t style_screen_hud;
extern lv_style_t style_seg_off;
extern lv_style_t style_seg_lit;
extern lv_style_t style_screen_ice;
extern lv_style_t style_ice_hair;
extern lv_style_t style_ice_frame;
extern lv_style_t style_ice_tick;
extern lv_style_t style_ice_block_off;
extern lv_style_t style_ice_block_on;
extern lv_style_t style_ice_block_hi;
extern lv_style_t style_ice_warn_text;
extern lv_style_t style_ice_pane;

/*----------------
 * Fonts
 *----------------*/

/* Targets: any */
extern lv_font_t * font_12;
extern lv_font_t * font_14;
extern lv_font_t * font_16;
extern lv_font_t * font_num_36;
extern lv_font_t * font_num_44;
extern lv_font_t * font_num_28;
extern lv_font_t * font_cjk_16;


/*----------------
 * Images
 *----------------*/



/*----------------
 * Subjects
 *----------------*/

extern lv_subject_t * subject_steps;
extern lv_subject_t * subject_goal;
extern lv_subject_t * subject_kcal;
extern lv_subject_t * subject_dist_m;
extern lv_subject_t * subject_active_min;
extern lv_subject_t * subject_cnt2;
extern lv_subject_t * subject_cnt3;
extern lv_subject_t * subject_time;
extern lv_subject_t * subject_date;
extern lv_subject_t * subject_batt;
extern lv_subject_t * subject_mode;
extern lv_subject_t * subject_speed_txt;
extern lv_subject_t * subject_speed;
extern lv_subject_t * subject_lat;
extern lv_subject_t * subject_lon;
extern lv_subject_t * subject_sat;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/*----------------
 * Event Callbacks
 *----------------*/

/*----------------
 * Easing Callbacks
 *----------------*/

/**
 * Initialize the component library
 */

void lvgl_ui_project_init_gen(const char * asset_path);

/**********************
 *      MACROS
 **********************/

/**********************
 *   POST INCLUDES
 **********************/

/*Include all the widgets, components and screens of this library*/
#include "screens/screen_hud_gen.h"

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LVGL_PRO_LVGL_UI_PROJECT_GEN_H*/