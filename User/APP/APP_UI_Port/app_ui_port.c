/**
 * @file app_ui_port.c
 * @brief 应用层：LVGL Pro 导出界面的本地适配层实现（单屏天光仪表）
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 本文件是本工程唯一直接引用生成代码符号的地方（screen_hud、
 *       subject_*、lvgl_ui_project_init）。生成代码在 ../UI_Export/，
 *       两者是并列目录：UI_Export 只被编辑器整体覆盖，本文件从不被覆盖。
 *       其余文件一律只见 app_ui_port.h，所以重新导出 UI 时受影响的
 *       就只剩这里，而这里本身不需要改——只需重做头文件顶部列出的那两处
 *       （lv_conf 开关、删翻译段）。
 *
 *       所有函数都必须在 LVGL 线程（Task_LVGL）里调用，理由见头文件。
 *
 *       格式化为什么用 lv_snprintf 而不是 snprintf：
 *       lv_conf.h 里 LV_USE_STDLIB_SPRINTF = LV_STDLIB_BUILTIN，LVGL 自带
 *       一份 printf（map 里的 _sprintf_builtin / _vsnprintf_inner），
 *       导出代码的 lv_snprintf 已经在用它——本文件复用它，既不新增
 *       newlib 的 printf 依赖，也不用赌 newlib-nano 对 "%02u" 的裁剪行为。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdbool.h>
#include <stddef.h>
#include "app_ui_port.h"
// 功能包含
#include "lvgl_ui_project.h"     /* 生成代码的聚合头：init + screen_hud + subjects */

/*==============================================================================
 * 静态变量
 *============================================================================*/
/**
 * @brief 界面是否已初始化完成
 * @note 用来挡住"Init 之前就写 subject"的调用。subject 在
 *       lvgl_ui_project_init() 里才被创建，提前写入是空指针解引用。
 */
static bool s_ready = false;

/**
 * @brief 屏幕点击的处理函数，由 APP 层注册
 * @note 只在 LVGL 线程里被读、被调用：写入发生在初始化阶段（同一线程），
 *       所以不需要保护。为 NULL 时点击被静默丢弃。
 */
static app_ui_tap_cb_t s_tap_cb = NULL;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 全屏透明点击层的事件处理：转交给 APP 注册的回调
 * @param event LVGL 事件对象，本函数不使用
 * @retval 无
 * @note 挂的是 LV_EVENT_SHORT_CLICKED，不是 LV_EVENT_CLICKED：
 *       后者只要"按下与抬起在同一对象、且没触发滚动"就发，
 *       **长按之后松手也会发**——手指搭在屏上超过 400 ms 再抬起同样会切面板；
 *       前者额外要求按压时长短于 long_press_time（默认 400 ms），
 *       正好挡住这类误动作。
 */
static void App_UI_Tap_Event(lv_event_t *event)
{
    (void)event;

    if (s_tap_cb != NULL) {
        s_tap_cb();
    }
}

/**
 * @brief 把一个带符号的微度值格式化成 "39.9042N" 这种形式
 * @param buf    输出缓冲，至少 12 字节
 * @param size   缓冲长度
 * @param value_e6 微度值（1e-6 度）
 * @param pos    非负时用的半球字母（纬度 N / 经度 E）
 * @param neg    负值时用的半球字母（纬度 S / 经度 W）
 * @retval 无
 * @note 先取绝对值再拆整数与小数：负数直接做除法会在 -0.5 这种值上
 *       得到"0.5000S"以外的奇怪结果，取绝对值最省事。
 *       小数只保留 4 位——1e-4 度约 11 m，第 5 位对显示没有意义，
 *       而多一位就会超出 9 字符的宽度预算（144px 内容宽 / 15px 字宽）。
 */
static void App_UI_Format_Coord(char *buf, size_t size, int32_t value_e6,
                                char pos, char neg)
{
    uint32_t mag = (value_e6 < 0) ? (uint32_t)(-value_e6) : (uint32_t)value_e6;
    uint32_t deg = mag / 1000000u;
    uint32_t frac = (mag % 1000000u) / 100u;
    char hemi = (value_e6 < 0) ? neg : pos;

    (void)lv_snprintf(buf, size, "%u.%04u%c", (unsigned)deg, (unsigned)frac, hemi);
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 建界面并上屏
 * @retval 无
 * @note 顺序不能换：先 init（建 subject、建常驻屏 screen_hud），再 load。
 *       反过来会加载一个 NULL 屏幕。
 *       最后盖一层全屏透明点击层，它是触摸唯一的落点，见文件头的说明。
 *       这里不清空 subject：导出代码带的是设计稿演示值，留着它正好让人
 *       在数据源接上之前核对排版，第一个 Set_* 调用就会覆盖对应字段。
 */
void App_UI_Init(void)
{
    lv_obj_t *tap;

    lvgl_ui_project_init(NULL);

    lv_screen_load(screen_hud);

    /* ---------- 全屏透明点击层：盖在最上层，只负责接"点了一下" ---------- */
    /* 不绑到具体控件上：导出代码会把控件整体重建，绑过的事件全丢；
       这层只依赖"屏幕被点了"，重新导出后依然成立。
       样式全部清零是为了让它真正看不见——LVGL 默认的 obj 自带底色与圆角。 */
    tap = lv_obj_create(lv_screen_active());
    if (tap != NULL) {
        lv_obj_set_pos(tap, 0, 0);
        lv_obj_set_size(tap, (int32_t)LV_PCT(100), (int32_t)LV_PCT(100));
        lv_obj_set_style_bg_opa(tap, (lv_opa_t)LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tap, 0, 0);
        lv_obj_set_style_outline_width(tap, 0, 0);
        lv_obj_set_style_shadow_width(tap, 0, 0);
        lv_obj_set_style_pad_all(tap, 0, 0);
        lv_obj_set_clickable(tap, true);   /* LVGL 9.6：add_flag 已弃用 */
        lv_obj_add_event_cb(tap, App_UI_Tap_Event, LV_EVENT_SHORT_CLICKED, NULL);
    }

    s_ready = true;
}

/**
 * @brief 注册"屏幕被点了一下"的处理函数
 * @param cb 回调，NULL 表示不处理
 * @retval 无
 * @note 只存指针，不立刻调用；真正的调用点在 App_UI_Tap_Event()。
 */
void App_UI_Set_Tap_Cb(app_ui_tap_cb_t cb)
{
    s_tap_cb = cb;
}

/**
 * @brief 切换模式面板
 * @param mode APP_UI_MODE_SPEED / APP_UI_MODE_POS
 * @retval 无
 * @note subject_mode 是 0/1 的整数，任何非 0 值都归到 1——避免将来
 *       有人传了 2 反而两块面板都隐藏（那样界面上就只剩常驻层）。
 */
void App_UI_Set_Mode(uint8_t mode)
{
    if (!s_ready) {
        return;
    }

    lv_subject_set_int(subject_mode, (mode == APP_UI_MODE_POS) ? 1 : 0);
}

/**
 * @brief 写电量
 * @param percent 0~100
 * @retval 无
 * @note 写越界值不会报错：subject 的 min/max 是 0/100，LVGL 自己夹取。
 */
void App_UI_Set_Battery(uint8_t percent)
{
    if (!s_ready) {
        return;
    }

    lv_subject_set_int(subject_batt, (int32_t)percent);
}

/**
 * @brief 写时间
 * @param hour   0~23
 * @param minute 0~59
 * @retval 无
 * @note 越界直接返回而不是夹取：23:99 这种值的成因通常是 RTC 读失败，
 *       把它显示出来会让人误以为时间是对的。
 */
void App_UI_Set_Time(uint8_t hour, uint8_t minute)
{
    char buf[8];

    if (!s_ready || (hour > 23u) || (minute > 59u)) {
        return;
    }

    (void)lv_snprintf(buf, sizeof(buf), "%02u:%02u",
                      (unsigned)hour, (unsigned)minute);
    lv_subject_set_string(subject_time, buf);
}

/**
 * @brief 写日期
 * @param year  完整年份（显示后两位）
 * @param month 1~12
 * @param day   1~31
 * @retval 无
 * @note 只校验"月/日不为 0"这一级，不校验大小月与闰年：界面不是日历，
 *       日期的合法性由 RTC 侧保证。
 */
void App_UI_Set_Date(uint16_t year, uint8_t month, uint8_t day)
{
    char buf[10];

    if (!s_ready || (month == 0u) || (month > 12u) || (day == 0u) || (day > 31u)) {
        return;
    }

    (void)lv_snprintf(buf, sizeof(buf), "%02u-%02u-%02u",
                      (unsigned)(year % 100u), (unsigned)month, (unsigned)day);
    lv_subject_set_string(subject_date, buf);
}

/**
 * @brief 写速度
 * @param speed_d10 速度 × 10（0.1 km/h 为单位）
 * @retval 无
 * @note 两个 subject 一次写完，中间不会插进一次渲染，所以不会出现
 *       "大数字已经是 24.8、刻度条还是旧值"的一帧。
 *       刻度条的量程是 0~120（导出时写死的），超出的速度会被夹住，
 *       但主读数照实显示——120 以上刻度条满格本来也是正确表现。
 */
void App_UI_Set_Speed(uint16_t speed_d10)
{
    char buf[8];

    if (!s_ready) {
        return;
    }

    lv_subject_set_int(subject_speed, (int32_t)(speed_d10 / 10u));

    (void)lv_snprintf(buf, sizeof(buf), "%u.%u",
                      (unsigned)(speed_d10 / 10u), (unsigned)(speed_d10 % 10u));
    lv_subject_set_string(subject_speed_txt, buf);
}

/**
 * @brief 写经纬度
 * @param lat_e6 纬度 × 1e6，北正南负
 * @param lon_e6 经度 × 1e6，东正西负
 * @retval 无
 * @note 越界直接返回：|lat| <= 90_000_000、|lon| <= 180_000_000。
 *       这条校验同时保证了 App_UI_Format_Coord() 里 -value_e6 不会溢出
 *       （-INT32_MIN 在 32 位上是未定义行为）。
 */
void App_UI_Set_Position(int32_t lat_e6, int32_t lon_e6)
{
    char buf[12];

    if (!s_ready) {
        return;
    }

    if ((lat_e6 < -90000000) || (lat_e6 > 90000000) ||
        (lon_e6 < -180000000) || (lon_e6 > 180000000)) {
        return;
    }

    App_UI_Format_Coord(buf, sizeof(buf), lat_e6, 'N', 'S');
    lv_subject_set_string(subject_lat, buf);

    App_UI_Format_Coord(buf, sizeof(buf), lon_e6, 'E', 'W');
    lv_subject_set_string(subject_lon, buf);
}

/**
 * @brief 写卫星颗数
 * @param count 0~24
 * @retval 无
 * @note 5 根信号柱的点亮阈值（2/4/6/8/10）与"不足 4 颗转警告色"
 *       都在导出代码的样式绑定里，本层只写一个整数。
 */
void App_UI_Set_Satellites(uint8_t count)
{
    if (!s_ready) {
        return;
    }

    lv_subject_set_int(subject_sat, (int32_t)count);
}
