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
#define ARC_THICKNESS (10)
#define COLOR_BG (lv_color_hex(0x0E1420))
#define COLOR_TEXT (lv_color_hex(0xFFFFFF))
#define COLOR_MUTED (lv_color_hex(0x8A97AD))
#define COLOR_ACCENT (lv_color_hex(0x34D399))
#define COLOR_TRACK (lv_color_hex(0x243044))


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

extern lv_obj_t * screen1;

/*----------------
 * Global styles
 *----------------*/

extern lv_style_t style_screen;

/*----------------
 * Fonts
 *----------------*/

/* Targets: any */
extern lv_font_t * font_12;
extern lv_font_t * font_14;
extern lv_font_t * font_16;
extern lv_font_t * font_num_36;


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
#include "screens/screen1_gen.h"

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LVGL_PRO_LVGL_UI_PROJECT_GEN_H*/