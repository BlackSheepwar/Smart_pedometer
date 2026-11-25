/**
 * @file app_ui_port.c
 * @brief 应用层：LVGL Pro 导出界面的本地适配层实现（240×320 双面板仪表）
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 本文件是本工程唯一直接引用生成代码符号的地方（screen_hud、
 *       subject_*、lvgl_ui_project_init）。生成代码在 APP_UI_Package 根下，
 *       本文件在 APP_UI_Package/APP_UI_Port/ 子目录：导出物只被编辑器整体覆盖，
 *       本文件从不被覆盖，两者装在一个目录里整体交付。
 *       其余文件一律只见 app_ui_port.h，所以重新导出 UI 时受影响的
 *       就只剩这里，而这里本身不需要改——只需核对 lv_conf 的
 *       LV_USE_OBJ_NAME，翻译文件已从工程侧移除。
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
#include "lvgl_ui_project.h"

/*==============================================================================
 * 静态变量
 *============================================================================*/
/**
 * @brief 界面是否已初始化完成
 * @note 用来挡住"Init 之前就写 subject"的调用。subject 在
 *       lvgl_ui_project_init() 里才被创建，提前写入是空指针解引用。
 */
static bool s_ready = false;

/*==============================================================================
 * 内部函数
 *============================================================================*/
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
 *       而多一位就会超出 9 字符的宽度预算（178px 内容宽 / 15px 字宽）。
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
 *       这里不清空 subject：导出代码带的是设计稿演示值，留着它正好让人
 *       在数据源接上之前核对排版，第一个 Set_* 调用就会覆盖对应字段。
 */
void App_UI_Init(void)
{
    lvgl_ui_project_init(NULL);

    lv_screen_load(screen_hud);

    s_ready = true;
}

/**
 * @brief 切换模式面板
 * @param mode APP_UI_MODE_SPEED / APP_UI_MODE_POS
 * @retval 无
 * @note subject_mode 是 0/1 的整数，任何非 0 值都归到 1——避免将来
 *       有人传了 2 反而两块面板都不隐藏（那样界面上就只剩常驻层）。
 *       写入同一个值时 subject 不会通知 observer，也就不会重播那 320 ms
 *       过渡，所以重复调用是安全的。
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
 *       先夹到 APP_UI_SPEED_MAX_D10（120.0 km/h）：刻度条量程是 0~120，
 *       主读数 68px 也只放得下 3 位整数，两者用同一个夹取后的值。
 */
void App_UI_Set_Speed(uint16_t speed_d10)
{
    char buf[8];

    if (!s_ready) {
        return;
    }

    if (speed_d10 > APP_UI_SPEED_MAX_D10) {
        speed_d10 = APP_UI_SPEED_MAX_D10;
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
