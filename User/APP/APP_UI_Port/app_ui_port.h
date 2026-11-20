/**
 * @file app_ui_port.h
 * @brief 应用层：LVGL Pro 导出界面的本地适配层（240×320 双面板仪表）
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 目录分工（这是本模块存在的理由）：
 *         User/APP/UI_Export/      LVGL Pro 的导出物，纯生成，勿手改；
 *                                  里面全是 lv_obj_t / lv_subject_t / lv_font_t
 *                                  这些类型，没有一行属于本工程。
 *         User/APP/APP_UI_Port/    ★本文件★ 手写适配层，全工程唯一
 *                                  include lvgl_ui_project.h 的地方。
 *
 *       为什么要有这条边界：
 *       若让 APP_Screen 直接 include 生成代码的头，那些 lv_ 类型就会顺着
 *       app_screen.h 传给上层，TASK 层也就间接拿到了 LVGL，
 *       "lvgl 是 PRIVATE 依赖"这条约定随即失效（改一个 UI 就要重编上层）。
 *       本头文件里一个 lv_ 都没有，app_screen.c 因此连 lvgl.h 都不用包含。
 *
 *       ★界面是什么★
 *       240×320 单屏，竖向四段：时钟/日期/5 段电量常驻在上（y 8..65），
 *       中间 240×170 的舞台里并排两块面板（速度 / 经纬度），底部是页签与
 *       朱砂下划线。两块面板各自就位（x=0 / x=240），切换靠 style 的
 *       translate_x 过渡自己滑过去，所以"换页"只有一个 subject 可写：
 *       subject_mode（0 = 速度，1 = 经纬度）。
 *
 *       ★换页的触发源★
 *         1. PC13 按键 → APP_Screen_Toggle_Mode()（翻到另一页）
 *       页数是界面语义，归 APP_Screen 管；本层只负责把数据写进 subject，
 *       不解析任何输入事件。
 *
 *       ★数据接口全是"只写"★
 *       界面不提供任何读回或查询：谁有数据谁调用 Set_*，本层格式化后写
 *       subject，剩下的由 LVGL 的 observer 完成。Set 的入参都用整数，
 *       理由是不引入浮点：0.1 km/h 与 1e-6 度都是整型，换算在数据源侧做，
 *       界面侧只做定点格式化（lv_snprintf），全程不碰 soft-float。
 *
 *       ★上电默认值★
 *       导出代码里 subject 带的是设计稿的演示值（20:44 / 26-11-10 / 78% /
 *       24.8 / 39.9042N / 9 颗卫星）。本层不清零：数据源还没接的时候，
 *       屏幕上留着这组值正好用来核对排版；接上真实数据后，对应的 Set_*
 *       一调用就会覆盖。APP_Screen 里没有 setter 被调用过的数据项会被跳过
 *       （见那边的"未设置"哨兵），所以这里不需要额外判断。
 *
 *       重新导出后必须重做的手改（只剩两处）：
 *         1. lv_conf.h 的 LV_USE_OBJ_NAME 必须为 1（已写在 lv_conf.h，不会丢）
 *         2. lvgl_ui_project_gen.c 的两段 Translations 要删掉（示例翻译表 +
 *            lv_translation_add_static 调用），否则必须打开 LV_USE_TRANSLATION
 *       字模是重新导出的产物：gen.c 会给"项目里定义过的每一套字体"生成
 *       extern 与赋值，所以 UI_Export/fonts/ 下的 .c 必须与导出一致地全量
 *       放进 CMake（见 User/APP/CMakeLists.txt 的说明）。
 *
 *       尺寸一致性：UI 导出尺寸、Dev_ST7789 的 width/x_offset、
 *       SVC_LVGL_HOR_RES 必须相同。
 *       对不上时 Svc_LVGL_Init() 返回 -3 停在故障态，不会给出"看起来能跑
 *       但错位"的界面。
 *
 *       线程约束：全部函数都只能在本工程的 LVGL 线程（Task_LVGL）里调用。
 *       LV_USE_OS 是 LV_OS_NONE，LVGL 内部没有任何锁；本层不做线程安全
 *       包装，跨线程的数据传递在 APP_Screen 里完成（只碰整数）。
 *       唯一的例外是 App_UI_Init()：它在 Task_LVGL 的初始化段里跑。
 */

#ifndef __APP_UI_PORT_H__
#define __APP_UI_PORT_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/**
 * @brief 模式面板：速度
 * @note 与 subject_mode 的取值一一对应（0 = 速度，1 = 经纬度）。
 *       这两个常量是"界面语义"，所以定义在本层；APP_Screen 用它，
 *       但 APP_Screen 对外只暴露"切换/选页"这类动作，不把数字漏给 TASK。
 */
#define APP_UI_MODE_SPEED       0u

/**
 * @brief 模式面板：经纬度
 * @note 见 APP_UI_MODE_SPEED。
 */
#define APP_UI_MODE_POS         1u

/**
 * @brief 电量 5 段指示的段数
 * @note 只用于注释与调用方理解：界面按 >0/20/40/60/80% 逐段点亮，
 *       本层不做换算，直接写 0~100 的百分比。
 */
#define APP_UI_BATT_SEGMENTS    5u

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 建界面并上屏
 * @retval 无
 * @note 必须在本工程的 LVGL 初始化之后调用（Svc_LVGL_Init 成功）。
 *       内部顺序：lvgl_ui_project_init(NULL) —— 注册 12 套字体、建全局样式、
 *       建 16 个 subject、创建并填充常驻屏 screen_hud；随后 lv_screen_load()。
 *       参数传 NULL 是安全的：导出代码里 asset_path 只在签名上存在，
 *       没有任何一处读它（全部资产都是烧进 FLASH 的 C 数组）。
 *       重复调用会重建一遍控件并泄漏上一份，本工程只在上电调一次。
 */
void App_UI_Init(void);

/**
 * @brief 切换到指定模式面板
 * @param mode APP_UI_MODE_SPEED 或 APP_UI_MODE_POS，其它值按"速度"处理
 * @retval 无
 * @note 只写一个 subject；两块面板是互斥的 checked 绑定，LVGL 在同一帧里
 *       决定显隐并补间 320 ms，不会出现两块都在或都不在的中间态。
 *       从速度切经纬度时面板整体左移 240 px，反向则右移——方向由状态变化
 *       本身决定，调用方不需要（也没法）指定动画方向。
 */
void App_UI_Set_Mode(uint8_t mode);

/**
 * @brief 写电量
 * @param percent 0~100，超出被 subject 的量程夹取
 * @retval 无
 * @note 界面消费的是"百分比"而不是电压：换算（分压 / 电量曲线）属于
 *       数据源侧的业务，本层不做。
 */
void App_UI_Set_Battery(uint8_t percent);

/**
 * @brief 写时间（24 小时制）
 * @param hour   0~23，越界直接返回（不显示非法时间）
 * @param minute 0~59，越界直接返回
 * @retval 无
 * @note 格式化成 "20:44"。补零在格式化里做，调用方不必自己拼字符串。
 */
void App_UI_Set_Time(uint8_t hour, uint8_t minute);

/**
 * @brief 写日期
 * @param year  完整年份（如 2026），只取后两位显示；越界直接返回
 * @param month 1~12，越界直接返回
 * @param day   1~31，越界直接返回
 * @retval 无
 * @note 格式化成 "26-11-10"（yy-mm-dd，8 字符）。
 *       用完整年份而不是两位年：调用方拿 RTC 的年份不用再减 2000。
 */
void App_UI_Set_Date(uint16_t year, uint8_t month, uint8_t day);

/**
 * @brief 写速度
 * @param speed_d10 速度 × 10（即 0.1 km/h 为单位），如 248 = 24.8 km/h
 * @retval 无
 * @note 一次写两个 subject：
 *         subject_speed     整数 km/h，只喂 12 格刻度条（量程 0~120）；
 *         subject_speed_txt "24.8"，喂 68px 主读数。
 *       之所以要两份：界面里不做除法取余，小数由本层格式化。
 */
void App_UI_Set_Speed(uint16_t speed_d10);

/**
 * @brief 写经纬度
 * @param lat_e6 纬度 × 1e6（微度），北正南负，如 39904200 = 39.9042°N
 * @param lon_e6 经度 × 1e6（微度），东正西负，如 116407400 = 116.4074°E
 * @retval 无
 * @note 格式化成 "39.9042N" / "116.4074E"：3 位整数 + 小数点 + 4 位小数 +
 *       半球字母，共 9 字符（30px 等宽下约 148 px，正好放进 178 px 内容宽）。
 *       4 位小数 ≈ 11 m，够显示；第 5 位起直接舍掉，不做四舍五入。
 *       越界（|lat| > 90°、|lon| > 180°）直接返回。
 */
void App_UI_Set_Position(int32_t lat_e6, int32_t lon_e6);

/**
 * @brief 写卫星颗数
 * @param count 0~24，超出被 subject 的量程夹取
 * @retval 无
 * @note 界面做两件事：显示颗数，以及按 2/4/6/8/10 颗逐根点亮 5 根信号柱；
 *       不足 4 颗时颗数自动转朱砂（这是导出的 subject 绑定，本层不管）。
 */
void App_UI_Set_Satellites(uint8_t count);

#endif /* __APP_UI_PORT_H__ */
