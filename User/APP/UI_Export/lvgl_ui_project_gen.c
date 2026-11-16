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

/* 本地改动（每次重新导出后都要重做，清单见 app_ui_port.h 顶部）：
 * 原处是一份 Editor 空模板残留的示例翻译表（languages = en/de，
 * tags = dog/cat/house），本工程的界面一处都没有引用。
 * 保留它就必须在 lv_conf.h 打开 LV_USE_TRANSLATION，代价是多编译一个
 * 模块、多一份 pack 的 RAM/FLASH 开销。本工程无多语言需求，整段删除。
 *
 * 同时删掉的还有 lvgl_ui_project_init_gen() 里
 * lv_translation_add_static() / lv_translation_set_language() 两行调用。 */

/**********************
 *  GLOBAL VARIABLES
 **********************/

/*--------------------
 *  Permanent screens
 *-------------------*/

lv_obj_t * screen_hud = NULL;

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
lv_font_t * font_num_44;
extern lv_font_t font_num_44_data;
lv_font_t * font_num_28;
extern lv_font_t font_num_28_data;
lv_font_t * font_cjk_16;
extern lv_font_t font_cjk_16_data;
lv_font_t * font_num_68;
extern lv_font_t font_num_68_data;
lv_font_t * font_num_30;
extern lv_font_t font_num_30_data;
lv_font_t * font_num_20;
extern lv_font_t font_num_20_data;
lv_font_t * font_cjk_22;
extern lv_font_t font_cjk_22_data;
lv_font_t * font_cjk_18;
extern lv_font_t font_cjk_18_data;

/*----------------
 * Images
 *----------------*/



/*----------------
 * Global styles
 *----------------*/

lv_style_t style_screen;
lv_style_t style_screen_paper;
lv_style_t style_screen_hud;
lv_style_t style_seg_off;
lv_style_t style_seg_lit;
lv_style_t style_screen_ice;
lv_style_t style_ice_hair;
lv_style_t style_ice_frame;
lv_style_t style_ice_tick;
lv_style_t style_ice_block_off;
lv_style_t style_ice_block_on;
lv_style_t style_ice_block_hi;
lv_style_t style_ice_warn_text;
lv_style_t style_ice_pane;
lv_style_t style_meter_screen;
lv_style_t style_meter_rule;
lv_style_t style_meter_pane;
lv_style_t style_meter_pane_next;
lv_style_t style_meter_tab_idle;
lv_style_t style_meter_tab_on;
lv_style_t style_meter_underline;
lv_style_t style_meter_seg_off;
lv_style_t style_meter_seg_on;
lv_style_t style_meter_seg_low;
lv_style_t style_meter_warn;

/*----------------
 * Subjects
 *----------------*/

lv_subject_t * subject_steps;
lv_subject_t * subject_goal;
lv_subject_t * subject_kcal;
lv_subject_t * subject_dist_m;
lv_subject_t * subject_active_min;
lv_subject_t * subject_cnt2;
lv_subject_t * subject_cnt3;
lv_subject_t * subject_time;
lv_subject_t * subject_date;
lv_subject_t * subject_batt;
lv_subject_t * subject_mode;
lv_subject_t * subject_speed_txt;
lv_subject_t * subject_speed;
lv_subject_t * subject_lat;
lv_subject_t * subject_lon;
lv_subject_t * subject_sat;

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
        if (!font_num_44) {
            /* font_num_44 */
            /* get font 'font_num_44' from a C array */
            font_num_44 = &font_num_44_data;

        }
        if (!font_num_28) {
            /* font_num_28 */
            /* get font 'font_num_28' from a C array */
            font_num_28 = &font_num_28_data;

        }
        if (!font_cjk_16) {
            /* font_cjk_16 */
            /* get font 'font_cjk_16' from a C array */
            font_cjk_16 = &font_cjk_16_data;

        }
        if (!font_num_68) {
            /* font_num_68 */
            /* get font 'font_num_68' from a C array */
            font_num_68 = &font_num_68_data;

        }
        if (!font_num_30) {
            /* font_num_30 */
            /* get font 'font_num_30' from a C array */
            font_num_30 = &font_num_30_data;

        }
        if (!font_num_20) {
            /* font_num_20 */
            /* get font 'font_num_20' from a C array */
            font_num_20 = &font_num_20_data;

        }
        if (!font_cjk_22) {
            /* font_cjk_22 */
            /* get font 'font_cjk_22' from a C array */
            font_cjk_22 = &font_cjk_22_data;

        }
        if (!font_cjk_18) {
            /* font_cjk_18 */
            /* get font 'font_cjk_18' from a C array */
            font_cjk_18 = &font_cjk_18_data;

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
        static const lv_style_prop_t style_meter_pane_trans_props[] = {LV_STYLE_TRANSLATE_X, 0};
        static lv_style_transition_dsc_t style_meter_pane_trans_dsc;


        /*Init all styles*/
        lv_style_init(&style_screen);
        lv_style_init(&style_screen_paper);
        lv_style_init(&style_screen_hud);
        lv_style_init(&style_seg_off);
        lv_style_init(&style_seg_lit);
        lv_style_init(&style_screen_ice);
        lv_style_init(&style_ice_hair);
        lv_style_init(&style_ice_frame);
        lv_style_init(&style_ice_tick);
        lv_style_init(&style_ice_block_off);
        lv_style_init(&style_ice_block_on);
        lv_style_init(&style_ice_block_hi);
        lv_style_init(&style_ice_warn_text);
        lv_style_init(&style_ice_pane);
        lv_style_init(&style_meter_screen);
        lv_style_init(&style_meter_rule);
        lv_style_init(&style_meter_pane);
        lv_style_init(&style_meter_pane_next);
        lv_style_init(&style_meter_tab_idle);
        lv_style_init(&style_meter_tab_on);
        lv_style_init(&style_meter_underline);
        lv_style_init(&style_meter_seg_off);
        lv_style_init(&style_meter_seg_on);
        lv_style_init(&style_meter_seg_low);
        lv_style_init(&style_meter_warn);

        lv_style_set_bg_color(&style_screen, COLOR_BG);
        lv_style_set_bg_opa(&style_screen, (255 * 100 / 100));
        lv_style_set_text_color(&style_screen, COLOR_TEXT);
        lv_style_set_text_font(&style_screen, font_16);
        lv_style_set_bg_color(&style_screen_paper, COLOR_PAPER_BG);
        lv_style_set_bg_opa(&style_screen_paper, (255 * 100 / 100));
        lv_style_set_text_color(&style_screen_paper, COLOR_PAPER_INK);
        lv_style_set_text_font(&style_screen_paper, font_16);
        lv_style_set_bg_color(&style_screen_hud, COLOR_HUD_BG);
        lv_style_set_bg_opa(&style_screen_hud, (255 * 100 / 100));
        lv_style_set_text_color(&style_screen_hud, COLOR_HUD_TEXT);
        lv_style_set_text_font(&style_screen_hud, font_16);
        lv_style_set_bg_color(&style_seg_off, COLOR_HUD_OFF);
        lv_style_set_bg_opa(&style_seg_off, (255 * 100 / 100));
        lv_style_set_radius(&style_seg_off, 3);
        lv_style_set_bg_color(&style_seg_lit, COLOR_HUD_LIT);
        lv_style_set_bg_opa(&style_seg_lit, (255 * 100 / 100));
        lv_style_set_radius(&style_seg_lit, 3);
        lv_style_set_bg_color(&style_screen_ice, COLOR_ICE_BG);
        lv_style_set_bg_opa(&style_screen_ice, (255 * 100 / 100));
        lv_style_set_text_color(&style_screen_ice, COLOR_ICE_INK);
        lv_style_set_text_font(&style_screen_ice, font_16);
        lv_style_set_bg_color(&style_ice_hair, COLOR_ICE_HAIR);
        lv_style_set_bg_opa(&style_ice_hair, (255 * 100 / 100));
        lv_style_set_radius(&style_ice_hair, 0);
        lv_style_set_border_width(&style_ice_hair, 0);
        lv_style_set_bg_color(&style_ice_frame, COLOR_ICE_FRAME);
        lv_style_set_bg_opa(&style_ice_frame, (255 * 100 / 100));
        lv_style_set_radius(&style_ice_frame, 0);
        lv_style_set_border_width(&style_ice_frame, 0);
        lv_style_set_bg_color(&style_ice_tick, COLOR_ICE_TICK);
        lv_style_set_bg_opa(&style_ice_tick, (255 * 100 / 100));
        lv_style_set_radius(&style_ice_tick, 0);
        lv_style_set_border_width(&style_ice_tick, 0);
        lv_style_set_bg_color(&style_ice_block_off, COLOR_ICE_TRACK);
        lv_style_set_bg_opa(&style_ice_block_off, (255 * 100 / 100));
        lv_style_set_radius(&style_ice_block_off, 0);
        lv_style_set_border_width(&style_ice_block_off, 0);
        lv_style_set_bg_color(&style_ice_block_on, COLOR_ICE_INK);
        lv_style_set_bg_opa(&style_ice_block_on, (255 * 100 / 100));
        lv_style_set_radius(&style_ice_block_on, 0);
        lv_style_set_border_width(&style_ice_block_on, 0);
        lv_style_set_bg_color(&style_ice_block_hi, COLOR_ICE_HI);
        lv_style_set_bg_opa(&style_ice_block_hi, (255 * 100 / 100));
        lv_style_set_radius(&style_ice_block_hi, 0);
        lv_style_set_border_width(&style_ice_block_hi, 0);
        lv_style_set_text_color(&style_ice_warn_text, COLOR_ICE_WARN);
        lv_style_set_bg_opa(&style_ice_pane, 0);
        lv_style_set_border_width(&style_ice_pane, 0);
        lv_style_set_radius(&style_ice_pane, 0);
        lv_style_set_pad_all(&style_ice_pane, 0);
        lv_style_set_bg_color(&style_meter_screen, COLOR_METER_BG);
        lv_style_set_bg_opa(&style_meter_screen, (255 * 100 / 100));
        lv_style_set_text_color(&style_meter_screen, COLOR_METER_INK);
        lv_style_set_text_font(&style_meter_screen, font_16);
        lv_style_set_pad_all(&style_meter_screen, 0);
        lv_style_set_border_width(&style_meter_screen, 0);
        lv_style_set_radius(&style_meter_screen, 0);
        lv_style_set_bg_color(&style_meter_rule, COLOR_METER_HAIR);
        lv_style_set_bg_opa(&style_meter_rule, (255 * 100 / 100));
        lv_style_set_radius(&style_meter_rule, 0);
        lv_style_set_border_width(&style_meter_rule, 0);
        lv_style_set_bg_opa(&style_meter_pane, 0);
        lv_style_set_border_width(&style_meter_pane, 0);
        lv_style_set_radius(&style_meter_pane, 0);
        lv_style_set_pad_all(&style_meter_pane, 0);
        lv_style_set_translate_x(&style_meter_pane, 0);
        lv_style_transition_dsc_init(&style_meter_pane_trans_dsc, style_meter_pane_trans_props, lv_anim_path_ease_out, 320, 0, NULL);
        lv_style_set_transition(&style_meter_pane, &style_meter_pane_trans_dsc);
        lv_style_set_translate_x(&style_meter_pane_next, -240);
        lv_style_set_text_color(&style_meter_tab_idle, COLOR_METER_MUTED);
        lv_style_set_text_color(&style_meter_tab_on, COLOR_METER_INK);
        lv_style_set_bg_color(&style_meter_underline, COLOR_METER_SIG);
        lv_style_set_bg_opa(&style_meter_underline, (255 * 100 / 100));
        lv_style_set_radius(&style_meter_underline, 0);
        lv_style_set_border_width(&style_meter_underline, 0);
        lv_style_set_bg_color(&style_meter_seg_off, COLOR_METER_TRACK);
        lv_style_set_bg_opa(&style_meter_seg_off, (255 * 100 / 100));
        lv_style_set_radius(&style_meter_seg_off, 2);
        lv_style_set_border_width(&style_meter_seg_off, 0);
        lv_style_set_bg_color(&style_meter_seg_on, COLOR_METER_INK);
        lv_style_set_bg_opa(&style_meter_seg_on, (255 * 100 / 100));
        lv_style_set_radius(&style_meter_seg_on, 2);
        lv_style_set_border_width(&style_meter_seg_on, 0);
        lv_style_set_bg_color(&style_meter_seg_low, COLOR_METER_SIG);
        lv_style_set_bg_opa(&style_meter_seg_low, (255 * 100 / 100));
        lv_style_set_text_color(&style_meter_warn, COLOR_METER_SIG);

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
    subject_cnt2 = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_cnt2, 0);
    lv_subject_set_max_value_int(subject_cnt2, 20000);
    lv_subject_set_int(subject_cnt2, 12480);
    subject_cnt3 = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_cnt3, 0);
    lv_subject_set_max_value_int(subject_cnt3, 100);
    lv_subject_set_int(subject_cnt3, 62);
    static char subject_time_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_time_prev_buf[UI_SUBJECT_STRING_LENGTH];
    subject_time = lv_subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject_time, subject_time_buf, subject_time_prev_buf, UI_SUBJECT_STRING_LENGTH);
    lv_subject_set_string(subject_time, "20:44");
    static char subject_date_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_date_prev_buf[UI_SUBJECT_STRING_LENGTH];
    subject_date = lv_subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject_date, subject_date_buf, subject_date_prev_buf, UI_SUBJECT_STRING_LENGTH);
    lv_subject_set_string(subject_date, "26-11-10");
    subject_batt = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_batt, 0);
    lv_subject_set_max_value_int(subject_batt, 100);
    lv_subject_set_int(subject_batt, 78);
    subject_mode = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_mode, 0);
    lv_subject_set_max_value_int(subject_mode, 1);
    lv_subject_set_int(subject_mode, 0);
    static char subject_speed_txt_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_speed_txt_prev_buf[UI_SUBJECT_STRING_LENGTH];
    subject_speed_txt = lv_subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject_speed_txt, subject_speed_txt_buf, subject_speed_txt_prev_buf, UI_SUBJECT_STRING_LENGTH);
    lv_subject_set_string(subject_speed_txt, "24.8");
    subject_speed = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_speed, 0);
    lv_subject_set_max_value_int(subject_speed, 120);
    lv_subject_set_int(subject_speed, 25);
    static char subject_lat_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_lat_prev_buf[UI_SUBJECT_STRING_LENGTH];
    subject_lat = lv_subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject_lat, subject_lat_buf, subject_lat_prev_buf, UI_SUBJECT_STRING_LENGTH);
    lv_subject_set_string(subject_lat, "39.9042N");
    static char subject_lon_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_lon_prev_buf[UI_SUBJECT_STRING_LENGTH];
    subject_lon = lv_subject_create(LV_SUBJECT_TYPE_STRING);
    lv_subject_set_string_buffer_static(subject_lon, subject_lon_buf, subject_lon_prev_buf, UI_SUBJECT_STRING_LENGTH);
    lv_subject_set_string(subject_lon, "116.4074E");
    subject_sat = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_min_value_int(subject_sat, 0);
    lv_subject_set_max_value_int(subject_sat, 24);
    lv_subject_set_int(subject_sat, 9);

    /*----------------
     * Translations
     *----------------*/

    /* 本地改动：两行 lv_translation_* 调用已删除，理由见文件上方的说明。 */

#if defined(LV_USE_XML) && LV_USE_XML
    /* Register widgets */

    /* Check all fonts / default if needed. This prevents fonts that are used in one target but
       defined in another from causing assertion failures during rendering of the Preview. */
    check_font(&font_12, "font_12");
    check_font(&font_14, "font_14");
    check_font(&font_16, "font_16");
    check_font(&font_num_36, "font_num_36");
    check_font(&font_num_44, "font_num_44");
    check_font(&font_num_28, "font_num_28");
    check_font(&font_cjk_16, "font_cjk_16");
    check_font(&font_num_68, "font_num_68");
    check_font(&font_num_30, "font_num_30");
    check_font(&font_num_20, "font_num_20");
    check_font(&font_cjk_22, "font_cjk_22");
    check_font(&font_cjk_18, "font_cjk_18");

    /* Register fonts */
    lv_xml_register_font(NULL, "font_12", font_12);
    lv_xml_register_font(NULL, "font_14", font_14);
    lv_xml_register_font(NULL, "font_16", font_16);
    lv_xml_register_font(NULL, "font_num_36", font_num_36);
    lv_xml_register_font(NULL, "font_num_44", font_num_44);
    lv_xml_register_font(NULL, "font_num_28", font_num_28);
    lv_xml_register_font(NULL, "font_cjk_16", font_cjk_16);
    lv_xml_register_font(NULL, "font_num_68", font_num_68);
    lv_xml_register_font(NULL, "font_num_30", font_num_30);
    lv_xml_register_font(NULL, "font_num_20", font_num_20);
    lv_xml_register_font(NULL, "font_cjk_22", font_cjk_22);
    lv_xml_register_font(NULL, "font_cjk_18", font_cjk_18);

    /* Register subjects */
    lv_xml_register_subject(NULL, "subject_steps", subject_steps);
    lv_xml_register_subject(NULL, "subject_goal", subject_goal);
    lv_xml_register_subject(NULL, "subject_kcal", subject_kcal);
    lv_xml_register_subject(NULL, "subject_dist_m", subject_dist_m);
    lv_xml_register_subject(NULL, "subject_active_min", subject_active_min);
    lv_xml_register_subject(NULL, "subject_cnt2", subject_cnt2);
    lv_xml_register_subject(NULL, "subject_cnt3", subject_cnt3);
    lv_xml_register_subject(NULL, "subject_time", subject_time);
    lv_xml_register_subject(NULL, "subject_date", subject_date);
    lv_xml_register_subject(NULL, "subject_batt", subject_batt);
    lv_xml_register_subject(NULL, "subject_mode", subject_mode);
    lv_xml_register_subject(NULL, "subject_speed_txt", subject_speed_txt);
    lv_xml_register_subject(NULL, "subject_speed", subject_speed);
    lv_xml_register_subject(NULL, "subject_lat", subject_lat);
    lv_xml_register_subject(NULL, "subject_lon", subject_lon);
    lv_xml_register_subject(NULL, "subject_sat", subject_sat);

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
    screen_hud = lv_obj_create(NULL);

    screen_hud_create();
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