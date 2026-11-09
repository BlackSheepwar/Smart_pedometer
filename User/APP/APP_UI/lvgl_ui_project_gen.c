/**
 * @file lvgl_ui_project_gen.c
 */

/*********************
 *      INCLUDES
 *********************/

#include "lvgl_ui_project.h"

#if defined(LV_USE_XML) && LV_USE_XML
#endif /* LV_USE_XML */

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void check_font(lv_font_t ** font, const char * name);

/**********************
 *  STATIC VARIABLES
 **********************/

static uint32_t lvgl_ui_project_target = LVGL_UI_PROJECT_COMPILE_TARGET;

/*----------------
 * Translations
 *----------------*/

#ifndef LV_EDITOR_PREVIEW
    static const char * translation_languages[] = {"en", "de", NULL};
    static const char * translation_tags[] = {"dog", "cat", "house", NULL};
    static const char * translation_texts[] = {
        "This is a dog", "Das ist ein Hund", /* dog */
        "A curious little cat", "Eine neugierige kleine Katze", /* cat */
        "The house is cozy and warm", "Das Haus ist gemütlich und warm", /* house */
    };
#endif

/**********************
 *  GLOBAL VARIABLES
 **********************/

/*--------------------
 *  Permanent screens
 *-------------------*/

lv_obj_t * screen1 = NULL;

/*----------------
 * Fonts
 *----------------*/

lv_font_t * font_12;
extern lv_font_t font_12_data;
lv_font_t * font_14;
extern lv_font_t font_14_data;
lv_font_t * font_16;
extern lv_font_t font_16_data;
lv_font_t * font_num_36;
extern lv_font_t font_num_36_data;

/*----------------
 * Images
 *----------------*/



/*----------------
 * Global styles
 *----------------*/

lv_style_t style_screen;

/*----------------
 * Subjects
 *----------------*/

lv_subject_t * subject_steps;
lv_subject_t * subject_goal;
lv_subject_t * subject_kcal;
lv_subject_t * subject_dist_m;
lv_subject_t * subject_active_min;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lvgl_ui_project_init_gen(const char * asset_path)
{

    /*----------------
     * Fonts
     *----------------*/

    /* Targets: any */

    #if LVGL_UI_PROJECT_CHECK_COMPILE_TARGET(LVGL_UI_PROJECT_TARGET_ALL)
    if (lvgl_ui_project_check_target(LVGL_UI_PROJECT_TARGET_ALL)) {
        if (!font_12) {
            /* font_12 */
            /* get font 'font_12' from a C array */
            font_12 = &font_12_data;

        }
        if (!font_14) {
            /* font_14 */
            /* get font 'font_14' from a C array */
            font_14 = &font_14_data;

        }
        if (!font_16) {
            /* font_16 */
            /* get font 'font_16' from a C array */
            font_16 = &font_16_data;

        }
        if (!font_num_36) {
            /* font_num_36 */
            /* get font 'font_num_36' from a C array */
            font_num_36 = &font_num_36_data;

        }
    }
    #endif

    /*----------------
     * Images
     *----------------*/



    /*----------------
     * Global styles
     *----------------*/

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_screen);

        lv_style_set_bg_color(&style_screen, COLOR_BG);
        lv_style_set_bg_opa(&style_screen, (255 * 100 / 100));
        lv_style_set_text_color(&style_screen, COLOR_TEXT);
        lv_style_set_text_font(&style_screen, font_14);

        style_inited = true;
    }

    /*----------------
     * Subjects
     *----------------*/
    subject_steps = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_steps, 0);
    lv_subject_set_max_value_int(subject_steps, 200000);
    lv_subject_set_int(subject_steps, 8432);
    subject_goal = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_goal, 1);
    lv_subject_set_max_value_int(subject_goal, 200000);
    lv_subject_set_int(subject_goal, 10000);
    subject_kcal = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_kcal, 0);
    lv_subject_set_max_value_int(subject_kcal, 9999);
    lv_subject_set_int(subject_kcal, 326);
    subject_dist_m = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_dist_m, 0);
    lv_subject_set_max_value_int(subject_dist_m, 999999);
    lv_subject_set_int(subject_dist_m, 6120);
    subject_active_min = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_active_min, 0);
    lv_subject_set_max_value_int(subject_active_min, 1440);
    lv_subject_set_int(subject_active_min, 47);

    /*----------------
     * Translations
     *----------------*/

    #ifndef LV_EDITOR_PREVIEW
        lv_translation_add_static(translation_languages, translation_tags, translation_texts);
        lv_translation_set_language(translation_languages[0]);
    #endif

#if defined(LV_USE_XML) && LV_USE_XML
    /* Register widgets */

    /* Check all fonts / default if needed. This prevents fonts that are used in one target but
       defined in another from causing assertion failures during rendering of the Preview. */
    check_font(&font_12, "font_12");
    check_font(&font_14, "font_14");
    check_font(&font_16, "font_16");
    check_font(&font_num_36, "font_num_36");

    /* Register fonts */
    lv_xml_register_font(NULL, "font_12", font_12);
    lv_xml_register_font(NULL, "font_14", font_14);
    lv_xml_register_font(NULL, "font_16", font_16);
    lv_xml_register_font(NULL, "font_num_36", font_num_36);

    /* Register subjects */
    lv_xml_register_subject(NULL, "subject_steps", subject_steps);
    lv_xml_register_subject(NULL, "subject_goal", subject_goal);
    lv_xml_register_subject(NULL, "subject_kcal", subject_kcal);
    lv_xml_register_subject(NULL, "subject_dist_m", subject_dist_m);
    lv_xml_register_subject(NULL, "subject_active_min", subject_active_min);

    /* Register callbacks */

    /* Register easing callbacks */
#endif

    /* Register all the global assets so that they won't be created again when globals.xml is parsed.
     * While running in the editor skip this step to update the preview when the XML changes */
#if defined(LV_USE_XML) && LV_USE_XML && !defined(LV_EDITOR_PREVIEW)
    /* Register images */
#endif

#if !defined(LV_USE_XML) || LV_USE_XML == 0
    /*--------------------
     *  Permanent screens
     *-------------------*/
    /* If XML is enabled it's assumed that the permanent screens are created
     * manually from XML using lv_xml_create() */
    /* Create all the permanent screens first so that they can reference each other */
    screen1 = lv_obj_create(NULL);

    screen1_create();
#endif
}

void lvgl_ui_project_set_target(uint32_t target)
{
    lvgl_ui_project_target = target;
}

uint32_t lvgl_ui_project_get_target(void)
{
    return lvgl_ui_project_target;
}

bool lvgl_ui_project_check_target(uint32_t target)
{
    return (lvgl_ui_project_target & target) ? true : false;
}

/* Callbacks */

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void check_font(lv_font_t ** font, const char * name)
{
    if (!(*font)) {
        *font = (lv_font_t *)LV_FONT_DEFAULT;
        LV_LOG_WARN("font `%s` was not set. Using `LV_FONT_DEFAULT` instead", name);
    }
}