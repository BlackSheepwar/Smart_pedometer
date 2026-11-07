/**
 * @file app_screen.h
 * @brief 应用层：两个 LVGL 界面的创建与切换
 * @author Ricardo
 * @date 2026-11-07
 *
 * @note 本模块管"有几屏、当前是哪一屏、切到下一屏"，不管"什么时候切"
 *       （那是按键业务，在 APP_Key_Cmd），也不管"怎么画到面板上"
 *       （那是 Service 的 Svc_LVGL 显示服务）。
 *
 *       对外提供两种"切屏"入口，别混用：
 *         - APP_Screen_Request_Next()  任意线程可调，只发一次请求；
 *         - APP_Screen_Next()          只能在 LVGL 线程里调，真的换屏。
 *       前者是**切屏接口**（本层拥有屏幕语义），后者是显示线程的执行动作。
 *
 *       头文件不出现 lv_obj_t：screen 指针留在本模块的 static 里，
 *       上层只能调 Init / Next / Index。LVGL 因此是 PRIVATE 依赖。
 *
 *       线程约束：除 Request_Next 外，三个函数都必须在 LVGL 线程里调用
 *       （Task_LVGL），不能从按键线程直接调——LVGL 不是线程安全的。
 *       Request_Next 之所以安全，是因为它内部只调 Service 的信号量释放，
 *       一个 lv_ 函数都不碰。
 */

#ifndef __APP_Screen_H__
#define __APP_Screen_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 创建两个界面并加载第 0 屏
 * @retval 无
 * @note 必须在 Svc_LVGL_Init() 成功之后调用：screen 要挂到 display 上。
 *       两屏都常驻内存，切换只是 lv_screen_load() 换指针，没有重建开销；
 *       代价是多占一屏的对象堆（约几百字节），对 24 KB 的 LVGL 堆可接受。
 *       重复调用会重复建屏，本工程只在上电时调一次。
 */
void APP_Screen_Init(void);

/**
 * @brief 请求切到下一屏（任意线程可调，不阻塞）
 * @retval 0: 请求已发出
 * @retval -1: 上一次请求还没被显示线程消费（丢弃本次，不排队）
 * @note **切屏接口在本层**，不在显示服务：屏幕有几张、当前第几张是本模块
 *       的知识，Service 只提供"叫醒显示线程"的机制。
 *       内部只调 Svc_LVGL_Request_Update()，一个 lv_ 函数都不碰，所以可以
 *       从按键线程安全调用；真正的换屏由 Task_LVGL 收到通知后调
 *       APP_Screen_Next() 完成。
 */
int APP_Screen_Request_Next(void);

/**
 * @brief 切到下一屏（0 → 1 → 0 循环）
 * @retval 无
 * @note **只能在 LVGL 线程里调用**（Task_LVGL），按键线程请用
 *       APP_Screen_Request_Next() 发请求。
 *       内部只是换 screen 并标记重绘，真正的像素输出发生在下一次
 *       Svc_LVGL_Step()（lv_timer_handler）里。
 */
void APP_Screen_Next(void);

/**
 * @brief 查询当前界面索引
 * @retval 0: 当前是 SCREEN 1
 * @retval 1: 当前是 SCREEN 2
 * @note 供按键侧或日志判断"现在显示的是哪一屏"，不用于切屏决策本身。
 */
uint8_t APP_Screen_Index(void);

#endif /* __APP_Screen_H__ */
