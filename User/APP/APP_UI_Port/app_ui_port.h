/**
 * @file app_ui_port.h
 * @brief 应用层：LVGL Pro 导出界面的本地适配层
 * @author Ricardo
 * @date 2026-11-09
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
 *       UI 在 Editor 里改一次就要重新导出并整体覆盖 UI_Export/，
 *       凡是生成文件里的本地改动都会丢。把"适配"集中在手写的 port 层，
 *       重新导出后只需要重做下面这几处：
 *
 *         1. lv_conf.h 的 LV_USE_OBJ_NAME 必须为 1（已改，不会丢）
 *         2. UI_Export/lvgl_ui_project_gen.c 里两处 Translations 段落要删掉
 *            （示例翻译表 + lv_translation_add_static 调用），
 *            否则必须打开 LV_USE_TRANSLATION
 *         3. 本文件与 app_ui_port.c 不用动，UI_Export/ 下其余文件不用动
 *
 *       线程约束：两个函数都只能在本工程的 LVGL 线程（Task_LVGL）里调用。
 *       LV_USE_OS 是 LV_OS_NONE，LVGL 内部没有任何锁；本层不做线程安全
 *       包装，跨线程的数据传递在 APP_Step 里完成（那边只碰整数）。
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
 * @brief 界面可显示的最大步数
 * @note 与 globals.xml 里 subject_steps 的 max_value 保持一致（200000）。
 *       UI 侧的 subject 也有同样的上下限，这里再夹一次是为了让"界面能显示
 *       多少"这件事在 C 侧也有明确出处，不必去翻 XML。
 */
#define APP_UI_STEPS_MAX        200000

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化导出界面并把它挂到当前 display 上
 * @retval 无
 * @note 必须在 Svc_LVGL_Init() 成功之后调用：screen 要挂到 display 上，
 *       subject 的绑定回调也要在 display 存在时才会被触发。
 *       内部做两件事：
 *         1. lvgl_ui_project_init(NULL) —— 注册 4 个字体、建全局样式、
 *            建 5 个 subject、创建并填充 screen1（13 个控件）；
 *         2. lv_screen_load(screen1) —— 真正让它上屏。
 *       参数传 NULL 是安全的：导出代码里 asset_path 只在签名上存在，
 *       没有任何一处读它（全部资产都是烧进 FLASH 的 C 数组，不走文件系统）。
 *       本函数会从 LVGL 堆里分配控件与 subject，堆不足时 LVGL 自身会打
 *       断言或返回 NULL，本层不做额外检查。
 */
void App_UI_Init(void);

/**
 * @brief 把步数写进界面
 * @param steps 要显示的步数，超出 [0, APP_UI_STEPS_MAX] 会被夹取
 * @retval 无
 * @note 写的是 subject_steps，LVGL 会通过 observer 自动刷新两处控件：
 *          - 屏幕中央 36px 大号数字；
 *          - 圆弧进度（value = steps，max = subject_goal = 10000）。
 *       所以调用方不需要持有任何控件指针，也不该去改控件。
 *       值没变化时本函数直接返回：lv_subject_set_int() 即使值相同也会
 *       遍历 observer，在一次 10 ms 节拍里白跑一遍没有意义。
 */
void App_UI_Set_Steps(int32_t steps);

#endif /* __APP_UI_PORT_H__ */
