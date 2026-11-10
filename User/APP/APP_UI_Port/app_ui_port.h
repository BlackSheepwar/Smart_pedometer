/**
 * @file app_ui_port.h
 * @brief 应用层：LVGL Pro 导出界面的本地适配层（三屏）
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
 *       ★本工程量程：0~APP_UI_STEPS_MAX（100）★
 *       导出代码是按"计步器演示"生成的：screen1 的 subject_steps 上限 200000、
 *       初值 8432，screen2 / screen3 各带一套独立演示值（12480 / 62）。
 *       本工程只有一路计数，三屏显示同一个值，量程统一成 100。
 *       这些覆盖**全部在 App_UI_Init() 里用 subject API 完成**，不写进生成
 *       代码——重新导出后会自动重新生效，不需要人工补改。
 *
 *       重新导出后必须重做的手改（都在 UI_Export/ 内，共四处）：
 *         1. lv_conf.h 的 LV_USE_OBJ_NAME 必须为 1（已改在 lv_conf.h，不会丢）
 *         2. lvgl_ui_project_gen.c 的两段 Translations 要删掉（示例翻译表 +
 *            lv_translation_add_static 调用），否则必须打开 LV_USE_TRANSLATION
 *         3. screens/screen2_gen.c 里进度条的 max 与 "/ 20000" 文案改成 100
 *            （控件属性没有 subject 绑定，只能在生成代码里改）
 *         4. 三个 screen*_gen.c 底部的按键提示 "B10  NEXT" 改成 "B1  NEXT"
 *            （演示工程里切屏键是 B10，本工程是 B1）
 *       量程与初值不在此列：它们在 App_UI_Init() 里用 subject API 覆盖，
 *       重新导出后自动生效。
 *
 *       线程约束：三个函数都只能在本工程的 LVGL 线程（Task_LVGL）里调用。
 *       LV_USE_OS 是 LV_OS_NONE，LVGL 内部没有任何锁；本层不做线程安全
 *       包装，跨线程的数据传递在 APP_Step / APP_Screen 里完成（只碰整数）。
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
 * @brief 导出代码里的界面数量（screen1 / screen2 / screen3）
 * @note 与 UI_Export/screens/ 下的三个 .xml 一一对应，增删界面时同步。
 *       APP_Screen 的循环切屏按这个数取模，不另设第二个常量。
 */
#define APP_UI_SCREEN_COUNT     3

/**
 * @brief 界面可显示的最大计数值
 * @note 与 APP_STEP_MAX 保持一致（业务上限在 APP_Step 里，本常量只负责
 *       "界面能显示多少"这一侧）。导出代码里 subject 的 200000 会在
 *       App_UI_Init() 里被覆盖成这个值。
 */
#define APP_UI_STEPS_MAX        100

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 建三块屏、统一量程、加载首屏
 * @retval 无
 * @note 必须在本工程的 LVGL 初始化之后调用（Svc_LVGL_Init 成功）。
 *       内部顺序：
 *         1. lvgl_ui_project_init(NULL) —— 注册 4 个字体、建全局样式、
 *            建 7 个 subject、创建并填充常驻屏 screen1；
 *         2. screen2_create() / screen3_create() —— 各建一次并自持指针；
 *         3. 覆盖 subject 的上下限与初值（量程 0~100，初值 0）；
 *         4. lv_screen_load(screen1)。
 *       参数传 NULL 是安全的：导出代码里 asset_path 只在签名上存在，
 *       没有任何一处读它（全部资产都是烧进 FLASH 的 C 数组）。
 *       重复调用会重建 screen2 / screen3 并泄漏上一份，本工程只在上电调一次。
 */
void App_UI_Init(void);

/**
 * @brief 切换到第 index 块屏
 * @param index 屏序号，0 = screen1，1 = screen2，2 = screen3
 * @retval 无
 * @note index 越界或界面未初始化时直接返回，不做静默取模——越界是调用方
 *       的 bug，悄悄绕回去只会让问题更难查。循环语义属于 APP_Screen。
 */
void App_UI_Show(uint8_t index);

/**
 * @brief 把计数值写进三块屏
 * @param steps 要显示的计数，超出 [0, APP_UI_STEPS_MAX] 会被夹取
 * @retval 无
 * @note 一次写三个 subject：screen1 的大号数字与圆弧、screen2 的数字与
 *       进度条、screen3 的数字与 5 段点阵。三屏只有一屏可见，但隐藏屏的
 *       observer 照样会被触发——这是有意的：切屏瞬间画面已经是新值，
 *       不会先闪一帧旧数据。
 *       值没变化时本函数直接返回：lv_subject_set_int() 即使值相同也会
 *       遍历 observer，在一次 10 ms 节拍里白跑三遍没有意义。
 */
void App_UI_Set_Steps(int32_t steps);

#endif /* __APP_UI_PORT_H__ */
