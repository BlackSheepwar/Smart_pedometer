/**
 * @file app_screen.c
 * @brief 应用层：界面生命周期、模式切换与数据同步实现（单屏）
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 本模块看起来"薄"，但它承担的是界面语义的所有权：
 *       谁建屏、当前显示哪块面板、什么时候刷、刷什么值。
 *       把这些散到 Task_LVGL 里会让线程体同时知道 UI 和业务；
 *       散到 app_ui_port 里又会让"适配层"变成"业务层"。
 *
 *       线程模型（本模块唯一的复杂点）：
 *         s_*         写入者 = 各数据源线程（每项单写者）
 *         s_*         读取者 = Task_LVGL（APP_Screen_Step）
 *         s_shown_*   只被 Task_LVGL 读写，记录"上次真正推给界面的值"
 *         s_pending_mode  初始化与按键线程写请求（全部走 __atomic_*），
 *                     显示线程在 Step 里落地到 s_active_mode 并推给界面
 *       所有量都是 8/16/32 位对齐整数，Cortex-M4 上单条 STR/LDR 不可分割，
 *       所以不加锁也不会有"半个值"。volatile 只是防止编译器把数据源写入的
 *       变量提到寄存器里缓存，导致显示线程永远读不到新值。
 *       唯一的例外是 s_pending_mode：写入点分散在初始化与按键线程，
 *       异或与写入都必须原子，免得"某处普通写、某处原子写"两种语义混用。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include "app_screen.h"
// 功能包含
#include "app_ui_port.h"         /* 界面适配层：Init / Set_* / 模式常量 */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief "未设置"哨兵：8 位量（电量、卫星数）用 0xFF */
#define APP_SCREEN_UNSET_U8     0xFFu
/** @brief "未设置"哨兵：16 位量（速度、打包后的时间）用 0xFFFF */
#define APP_SCREEN_UNSET_U16    0xFFFFu
/**
 * @brief "未设置"哨兵：32 位量（打包后的日期、经纬度）用 0x7FFFFFFF
 * @note 不能用 0xFFFFFFFF：转成 int32 是 -1，而 -1 微度是赤道上完全合法的
 *       纬度。取 INT32_MAX 才真正不可能出现——它折合 2147.48°，
 *       而端口层把 |lat| 限在 90°、|lon| 限在 180°。
 */
#define APP_SCREEN_UNSET_U32    0x7FFFFFFFu

/*==============================================================================
 * 编译期检查
 *============================================================================*/
/* 本层的 APP_SCREEN_MODE_* 与适配层的 APP_UI_MODE_* 是同一件事的两种说法：
 * 前者是"界面策略"，后者是"界面实现"。数值必须一致，写死断言而不是靠注释，
 * 否则改了一边、另一边还在用旧值，症状是"点了经纬度却停在速度面板"。
 */
_Static_assert((uint8_t)APP_UI_MODE_SPEED == APP_SCREEN_MODE_SPEED,
               "APP_SCREEN_MODE_SPEED 与 APP_UI_MODE_SPEED 必须同值");
_Static_assert((uint8_t)APP_UI_MODE_POS == APP_SCREEN_MODE_POS,
               "APP_SCREEN_MODE_POS 与 APP_UI_MODE_POS 必须同值");
/* Toggle 用"异或 1"翻转，前提是两个模式恰好是相邻的 0 与 1。
   将来加第三块面板时这里会先编译失败，逼着把 Toggle 改成显式取值。 */
_Static_assert(APP_SCREEN_MODE_POS == (APP_SCREEN_MODE_SPEED + 1u),
               "Toggle 依赖 SPEED/POS 相邻（0/1），改取值必须重写翻转逻辑");

/*==============================================================================
 * 静态变量：数据源写入区（每项单写者、显示线程只读）
 *============================================================================*/
/**
 * @brief 时间，打包成 hour×100 + minute
 * @note 打包而不是两个字节：一次 16 位写入，读侧不会拿到"新小时 + 旧分钟"。
 *       初值 0xFFFF 表示还没人提交过，Step() 直接跳过。
 */
static volatile uint16_t s_hhmm = APP_SCREEN_UNSET_U16;

/**
 * @brief 日期，打包成 yy×10000 + month×100 + day
 * @note 同 s_hhmm，一次 32 位写入。0x7FFFFFFF 表示未设置（见 UNSET_U32 的说明）。
 */
static volatile uint32_t s_date = APP_SCREEN_UNSET_U32;

/** @brief 电量百分比 0~100，0xFF 表示未设置 */
static volatile uint8_t s_batt = APP_SCREEN_UNSET_U8;

/** @brief 速度 × 10（0.1 km/h），0xFFFF 表示未设置 */
static volatile uint16_t s_speed_d10 = APP_SCREEN_UNSET_U16;

/** @brief 纬度 × 1e6，0x7FFFFFFF 表示未设置 */
static volatile int32_t s_lat_e6 = (int32_t)APP_SCREEN_UNSET_U32;

/** @brief 经度 × 1e6，与 s_lat_e6 成对写入 */
static volatile int32_t s_lon_e6 = (int32_t)APP_SCREEN_UNSET_U32;

/** @brief 卫星颗数 0~24，0xFF 表示未设置 */
static volatile uint8_t s_sat = APP_SCREEN_UNSET_U8;

/**
 * @brief 待显示的模式（初始化与按键线程都会写）
 * @note 初值 = 速度面板。volatile 理由见文件头。
 *       写入点分散在初始化、Toggle 与 Select_Mode，翻转走 __atomic_fetch_xor、
 *       选页走 __atomic_store_n，不能再写成读-改-写或普通赋值。
 */
static volatile uint8_t s_pending_mode = APP_SCREEN_MODE_SPEED;

/*==============================================================================
 * 静态变量：显示线程独占区（记录已推送的值）
 *============================================================================*/
/** @brief 已经真正加载的模式（显示线程独占） */
static uint8_t s_active_mode = APP_SCREEN_MODE_SPEED;

/** @brief 上次推给界面的时间（打包格式），初值 = 哨兵，保证第一次提交必生效 */
static uint16_t s_shown_hhmm = APP_SCREEN_UNSET_U16;

/** @brief 上次推给界面的日期（打包格式） */
static uint32_t s_shown_date = APP_SCREEN_UNSET_U32;

/** @brief 上次推给界面的电量 */
static uint8_t s_shown_batt = APP_SCREEN_UNSET_U8;

/** @brief 上次推给界面的速度 */
static uint16_t s_shown_speed_d10 = APP_SCREEN_UNSET_U16;

/** @brief 上次推给界面的纬度 */
static int32_t s_shown_lat_e6 = (int32_t)APP_SCREEN_UNSET_U32;

/** @brief 上次推给界面的经度 */
static int32_t s_shown_lon_e6 = (int32_t)APP_SCREEN_UNSET_U32;

/** @brief 上次推给界面的卫星颗数 */
static uint8_t s_shown_sat = APP_SCREEN_UNSET_U8;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 建立界面并显示首帧
 * @retval 无
 * @note 不清空数据区：留成哨兵，所以第一次 Step 不会把界面刷成 00:00。
 *       App_UI_Init() 里已经 load 了 screen_hud，这里再把模式显式写一次，
 *       是为了让"上电一定是速度面板"这条不依赖导出代码里的 subject 初值。
 */
void APP_Screen_Init(void)
{
    /* 这个变量有三个写入点（初始化、Toggle、Select_Mode），都走原子存储，
       免得"某处普通写、某处原子写"两种语义混在同一个变量上。 */
    (void)__atomic_store_n(&s_pending_mode, (uint8_t)APP_SCREEN_MODE_SPEED,
                           __ATOMIC_RELAXED);
    s_active_mode = APP_SCREEN_MODE_SPEED;

    App_UI_Init();
    App_UI_Set_Mode(APP_SCREEN_MODE_SPEED);
}

/**
 * @brief 请求在"速度 / 经纬度"之间切换
 * @retval 无
 * @note 触发源是按键线程（PC13）。变量另有初始化的写入点，所以翻转用
 *       __atomic_fetch_xor：取值只有 0/1，异或 1 就是翻转，Cortex-M4 上是
 *       LDREXB/STREXB 循环，无锁、不关中断，APP 层也不因此需要引入 FreeRTOS.h。
 */
void APP_Screen_ToggleMode(void)
{
    (void)__atomic_fetch_xor(&s_pending_mode, 1u, __ATOMIC_RELAXED);
}

/**
 * @brief 直接选定某块模式面板
 * @param mode APP_SCREEN_MODE_SPEED / APP_SCREEN_MODE_POS
 * @retval 无
 * @note 任意线程可调：命令表的 handler 若有"直达"某一页的按键就会走这里。
 *       写入用 __atomic_store_n——与 Toggle 的异或、Init 的存储同属一个
 *       原子变量，混用普通赋值会让"读到半新半旧"这件事重新有可能。
 *       在 Cortex-M4 上对齐的单字节存储本来就是一条 STR，relaxed 版本
 *       编译出来与普通赋值同价，所以这不是为安全付性能。
 *
 *       只认这两个值：传别的进来直接丢掉，而不是"非 0 就当经纬度"——
 *       那样一个笔误就会把界面锁在经纬度面板上，还查不出原因。
 *       与 Toggle 的区别只在语义：Toggle 是"不管当前是哪块，翻到另一块"，
 *       本函数是"就选这块"。
 */
void APP_Screen_SelectMode(uint8_t mode)
{
    if ((mode != APP_SCREEN_MODE_SPEED) && (mode != APP_SCREEN_MODE_POS)) {
        return;
    }

    (void)__atomic_store_n(&s_pending_mode, mode, __ATOMIC_RELAXED);
}

/**
 * @brief 提交时间
 * @param hour   0~23
 * @param minute 0~59
 * @retval 无
 * @note 打包成 hh×100+mm 后一次写入。越界值不做夹取也不在此拦——
 *       先写进去，由 App_UI_Set_Time() 在显示线程里丢掉，这样即使
 *       RTC 读失败（返回 0xFF）也不会把非法时间画到屏上。
 */
void APP_Screen_SetTime(uint8_t hour, uint8_t minute)
{
    s_hhmm = (uint16_t)(((uint16_t)hour * 100u) + (uint16_t)minute);
}

/**
 * @brief 提交日期
 * @param year  完整年份，只保留后两位
 * @param month 1~12
 * @param day   1~31
 * @retval 无
 * @note 年份取模放在这里而不是显示层：数据区只存"要显示的东西"，
 *       免得读的人还要再想一次 2026 与 26 的关系。
 */
void APP_Screen_SetDate(uint16_t year, uint8_t month, uint8_t day)
{
    s_date = ((uint32_t)(year % 100u) * 10000u)
             + ((uint32_t)month * 100u)
             + (uint32_t)day;
}

/**
 * @brief 提交电量百分比
 * @param percent 0~100
 * @retval 无
 */
void APP_Screen_SetBattery(uint8_t percent)
{
    s_batt = percent;
}

/**
 * @brief 提交速度
 * @param speed_d10 速度 × 10（0.1 km/h）
 * @retval 无
 */
void APP_Screen_SetSpeed(uint16_t speed_d10)
{
    s_speed_d10 = speed_d10;
}

/**
 * @brief 提交经纬度
 * @param lat_e6 纬度 × 1e6，北正南负
 * @param lon_e6 经度 × 1e6，东正西负
 * @retval 无
 * @note 顺序是"纬度先置未设置 → 写经度 → 写纬度"：Step() 只在纬度有效时
 *       提交这一对，所以写者中途被抢占时读侧会跳过这一拍。
 *       残留窗口是 Step 读完纬度之后写者才更新经度，此时会推一帧
 *       「旧纬度 + 新经度」——只影响一帧显示，不会产生非法坐标。
 */
void APP_Screen_SetPosition(int32_t lat_e6, int32_t lon_e6)
{
    s_lat_e6 = (int32_t)APP_SCREEN_UNSET_U32;   /* 先标记为未完成 */
    s_lon_e6 = lon_e6;
    s_lat_e6 = lat_e6;                          /* 再落地纬度，这一帧才有效 */
}

/**
 * @brief 提交卫星颗数
 * @param count 0~24
 * @retval 无
 */
void APP_Screen_SetSatellites(uint8_t count)
{
    s_sat = count;
}

/**
 * @brief 把一个显示节拍的数据同步到界面
 * @retval 无
 * @note 先落地模式请求，再逐项比对数据。顺序有意义：先换面板，
 *       新面板的第一帧就带着当前数据，不会出现"旧面板已刷新、新面板
 *       还是旧值"的中间态（虽然只有一帧，但在 30 fps 下看得见）。
 *       每一项都先读进局部变量再比较：读两次的话，数据源可能在两次读之间
 *       改了值，导致"比较用旧值、推送用新值"这种看起来对、实际漏一帧的行为。
 */
void APP_Screen_Step(void)
{
    /* ---------- 1. 模式 ---------- */
    uint8_t mode = __atomic_load_n(&s_pending_mode, __ATOMIC_RELAXED);

    if (mode != s_active_mode) {
        s_active_mode = mode;
        App_UI_Set_Mode(mode);
    }

    /* ---------- 2. 时间 ---------- */
    uint16_t hhmm = s_hhmm;

    if ((hhmm != APP_SCREEN_UNSET_U16) && (hhmm != s_shown_hhmm)) {
        s_shown_hhmm = hhmm;
        App_UI_Set_Time((uint8_t)(hhmm / 100u), (uint8_t)(hhmm % 100u));
    }

    /* ---------- 3. 日期 ---------- */
    uint32_t date = s_date;

    if ((date != APP_SCREEN_UNSET_U32) && (date != s_shown_date)) {
        s_shown_date = date;
        App_UI_Set_Date((uint16_t)(date / 10000u),
                        (uint8_t)((date / 100u) % 100u),
                        (uint8_t)(date % 100u));
    }

    /* ---------- 4. 电量 ---------- */
    uint8_t batt = s_batt;

    if ((batt != APP_SCREEN_UNSET_U8) && (batt != s_shown_batt)) {
        s_shown_batt = batt;
        App_UI_Set_Battery(batt);
    }

    /* ---------- 5. 速度 ---------- */
    uint16_t speed = s_speed_d10;

    if ((speed != APP_SCREEN_UNSET_U16) && (speed != s_shown_speed_d10)) {
        s_shown_speed_d10 = speed;
        App_UI_Set_Speed(speed);
    }

    /* ---------- 6. 经纬度 ---------- */
    int32_t lat = s_lat_e6;
    int32_t lon = s_lon_e6;

    if ((lat != (int32_t)APP_SCREEN_UNSET_U32) &&
        ((lat != s_shown_lat_e6) || (lon != s_shown_lon_e6))) {
        s_shown_lat_e6 = lat;
        s_shown_lon_e6 = lon;
        App_UI_Set_Position(lat, lon);
    }

    /* ---------- 7. 卫星颗数 ---------- */
    uint8_t sat = s_sat;

    if ((sat != APP_SCREEN_UNSET_U8) && (sat != s_shown_sat)) {
        s_shown_sat = sat;
        App_UI_Set_Satellites(sat);
    }
}
