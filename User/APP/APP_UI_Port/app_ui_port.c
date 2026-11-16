/**
 * @file app_ui_port.c
 * @brief 应用层：LVGL Pro 导出界面的本地适配层实现（240×320 双面板仪表）
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
 *
 *       ★手势为什么不做"跟手拖动"★
 *       这一版的面板切换是状态过渡：subject_mode 一变，两块面板按
 *       style_meter_pane 里声明的 translate_x 过渡自己补间 320 ms。
 *       要做到"手指拖到哪面板走到哪"，就得在 PRESSING 里直接改写
 *       translate_x 并与那 320 ms 的过渡抢同一个属性；而这块电阻屏
 *       20 ms 才采一次样、单点、无压力轴，跟手效果只会又抖又滞后。
 *       所以这里只判"划够了没有、往哪边划"，松手后交给状态过渡播完
 *       那 320 ms——动作干净，也不依赖 LVGL 的版本细节。
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
 * 宏定义与常量
 *============================================================================*/
/**
 * @brief 判定为滑动所需的最小水平位移（像素）
 * @note 240 宽的 13%。再小则轻点时的抖动会误判；再大则要划掉半个屏幕才
 *       有反应，而电阻屏 20 ms 采一次样，快速划动本来就吃不满位移。
 */
#define APP_UI_SWIPE_MIN_DX     32

/**
 * @brief 判定为点按允许的最大位移（像素）
 * @note 与 LVGL 的 LV_INDEV_DEF_SCROLL_LIMIT 同值：手指按下后在这个圈内
 *       移动，在 LVGL 眼里"没有拖动"，在本层眼里也应当算"点了一下"。
 */
#define APP_UI_TAP_MAX_MOVE     10

/**
 * @brief 判定为点按允许的最长按压时长（毫秒）
 * @note 与 LVGL 的 LV_INDEV_DEF_LONG_PRESS_TIME 同值。超过它的静止按压
 *       多半是"手搭在屏上"，不该当成一次换页。
 */
#define APP_UI_TAP_MAX_MS       400u

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 一次按压的中间状态
 * @note 只有 PRESSED → RELEASED 之间才有效；tracking 为 false 时其余字段
 *       是上一次的残值，不可读。
 */
typedef struct {
    lv_point_t start;       /**< 按下点，逻辑坐标 */
    uint32_t   start_tick;  /**< 按下时刻，用于判点按的时长 */
    bool       tracking;    /**< true 表示这一次按压还没结束 */
} app_ui_press_t;

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
 * @brief 手势的处理函数，由 APP 层注册
 * @note 只在 LVGL 线程里被读、被调用：写入发生在初始化阶段（同一线程），
 *       所以不需要保护。为 NULL 时手势被静默丢弃。
 */
static app_ui_gesture_cb_t s_gesture_cb = NULL;

/** @brief 当前按压状态，见 app_ui_press_t */
static app_ui_press_t s_press;

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

/**
 * @brief 按位移与时长判定手势，并转交给 APP 层
 * @param dx      水平总位移，向右为正
 * @param dy      竖直总位移，向下为正
 * @param held_ms 按压时长（毫秒）
 * @retval 无
 * @note 判定顺序不能换：先判滑动再判点按。反过来的话，一次"划得不够远
 *       但确实不是在点"的操作会落进点按分支；而这里要求点按的位移
 *       <= 10 px，两者本来也不重叠，先判谁只是让意图更清楚。
 *       竖直位移大于水平位移时一律丢弃：本界面没有上下翻页，
 *       把斜着划当成翻页会让人误以为触摸在乱跳。
 */
static void App_UI_Report_Gesture(int32_t dx, int32_t dy, uint32_t held_ms)
{
    int32_t adx;
    int32_t ady;
    app_ui_gesture_t gesture;

    if (s_gesture_cb == NULL) {
        return;
    }

    /* 自己取绝对值而不是用 LV_ABS：那个宏对参数求值两次，传表达式时会重复计算 */
    adx = (dx < 0) ? -dx : dx;
    ady = (dy < 0) ? -dy : dy;

    if ((adx >= APP_UI_SWIPE_MIN_DX) && (adx > ady)) {
        gesture = (dx < 0) ? APP_UI_GESTURE_SWIPE_LEFT : APP_UI_GESTURE_SWIPE_RIGHT;
    } else if ((adx <= APP_UI_TAP_MAX_MOVE) && (ady <= APP_UI_TAP_MAX_MOVE) &&
               (held_ms < APP_UI_TAP_MAX_MS)) {
        gesture = APP_UI_GESTURE_TAP;
    } else {
        /* 竖直拖动、位移介于两者之间、长按不动：都不解释 */
        return;
    }

    s_gesture_cb(gesture);
}

/**
 * @brief 全屏透明手势层的事件处理：判定手势并转交给 APP 注册的回调
 * @param event LVGL 事件对象
 * @retval 无
 * @note 只挂 PRESSED / RELEASED / PRESS_LOST，一个 click 事件都不挂：
 *       理由见头文件"为什么不用 LV_EVENT_SHORT_CLICKED"。
 *
 *       抬起点为什么可信：indev_read_core() 在调 read_cb 之前先用
 *       last_raw_point 预填了 data->point，而本工程的触摸 read_cb 在未按下时
 *       只改 state、不碰坐标，所以 RELEASED 这一刻 lv_indev_get_point()
 *       拿到的就是"最后一次按下的坐标"。若哪天 read_cb 改成在未按下时
 *       清零坐标，这里会得到 (0,0)，表现为"每次松手都判成向左滑"。
 *
 *       PRESS_LOST 必须接：手指划出本层（例如将来界面缩小了）或 LVGL
 *       判定按压被别处接管时，它代替 RELEASED 到达；不接就会带着上一次的
 *       按下点等到下一次 PRESSED，把两次无关的触摸拼成一次长距离滑动。
 */
static void App_UI_Gesture_Event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev;
    lv_point_t point;

    switch (code) {
    case LV_EVENT_PRESSED:
        indev = lv_indev_active();
        if (indev == NULL) {
            return;
        }
        lv_indev_get_point(indev, &s_press.start);
        s_press.start_tick = lv_tick_get();
        s_press.tracking = true;
        break;

    case LV_EVENT_RELEASED:
        if (!s_press.tracking) {
            return;
        }
        s_press.tracking = false;

        indev = lv_indev_active();
        if (indev == NULL) {
            return;
        }
        lv_indev_get_point(indev, &point);
        App_UI_Report_Gesture(point.x - s_press.start.x,
                              point.y - s_press.start.y,
                              lv_tick_elaps(s_press.start_tick));
        break;

    case LV_EVENT_PRESS_LOST:
        s_press.tracking = false;
        break;

    default:
        break;
    }
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 建界面并上屏
 * @retval 无
 * @note 顺序不能换：先 init（建 subject、建常驻屏 screen_hud），再 load。
 *       反过来会加载一个 NULL 屏幕。
 *       最后盖一层全屏透明手势层，它是触摸唯一的落点，见文件头的说明。
 *       这里不清空 subject：导出代码带的是设计稿演示值，留着它正好让人
 *       在数据源接上之前核对排版，第一个 Set_* 调用就会覆盖对应字段。
 */
void App_UI_Init(void)
{
    lv_obj_t *screen;
    lv_obj_t *layer;

    lvgl_ui_project_init(NULL);

    lv_screen_load(screen_hud);

    screen = lv_screen_active();
    if (screen == NULL) {
        /* 正常情况下不可能：Svc_LVGL_Init 已经建好 display 且 screen_hud
           就是它的屏幕。真为空时宁可不建手势层——界面能看但点不动，
           也好过拿 NULL 去 lv_obj_create()（那会新建一个屏幕）把界面顶掉。 */
        s_ready = true;
        return;
    }

    /* ---------- 屏幕自身禁止滚动：手势层只想要"点/划"，不想要"滚" ---------- */
    /* 舞台里的 pane_pos 平时停在 x=240（屏幕外），若屏幕还带滚动能力，
       它的可滚动区域会被算成 480 宽，一次竖直拖动就可能让 LVGL 认定
       "发生了滚动"，随后给手势层发 PRESS_LOST——表现为斜着划一下没反应。
       关掉之后，屏幕上任何拖动都不会被解释成滚动。 */
    lv_obj_set_scrollable(screen, false);

    /* ---------- 全屏透明手势层：盖在最上层，只负责接"点了一下 / 划了一下" ---------- */
    /* 不绑到具体控件上：导出代码会把控件整体重建，绑过的事件全丢；
       这层只依赖"屏幕被触摸了"，重新导出后依然成立。
       样式全部清零是为了让它真正看不见——LVGL 默认的 obj 自带底色与圆角。 */
    layer = lv_obj_create(screen);
    if (layer != NULL) {
        lv_obj_set_pos(layer, 0, 0);
        lv_obj_set_size(layer, (int32_t)LV_PCT(100), (int32_t)LV_PCT(100));
        lv_obj_set_style_bg_opa(layer, (lv_opa_t)LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(layer, 0, 0);
        lv_obj_set_style_outline_width(layer, 0, 0);
        lv_obj_set_style_shadow_width(layer, 0, 0);
        lv_obj_set_style_pad_all(layer, 0, 0);
        lv_obj_set_style_radius(layer, 0, 0);
        lv_obj_set_scrollable(layer, false);  /* 本层不吃滚动，只吃按下与抬起 */
        lv_obj_set_clickable(layer, true);    /* LVGL 9.6：add_flag 已弃用 */

        lv_obj_add_event_cb(layer, App_UI_Gesture_Event, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(layer, App_UI_Gesture_Event, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(layer, App_UI_Gesture_Event, LV_EVENT_PRESS_LOST, NULL);
    }

    s_press.tracking = false;
    s_ready = true;
}

/**
 * @brief 注册触摸手势的处理函数
 * @param cb 回调，NULL 表示不处理
 * @retval 无
 * @note 只存指针，不立刻调用；真正的调用点在 App_UI_Report_Gesture()。
 */
void App_UI_Set_Gesture_Cb(app_ui_gesture_cb_t cb)
{
    s_gesture_cb = cb;
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
