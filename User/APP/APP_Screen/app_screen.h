/**
 * @file app_screen.h
 * @brief 应用层：界面生命周期、切屏与数据同步
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 本模块管三件事，且只这三件：
 *         1. 什么时候把界面建起来（Init）；
 *         2. 当前该显示哪一屏（Next 提请求，Step 落地）；
 *         3. 每个显示节拍把业务数据送到界面上（Step）。
 *       不管"界面长什么样"（那是 UI_Export/ 下的导出代码，
 *       由 APP_UI_Port 适配），
 *       也不管"计数什么时候变"（那是 APP_Step 与按键命令表）。
 *
 *       ★切屏为什么拆成 Next + Step 两个函数★
 *       LVGL 不是线程安全的（本工程 LV_USE_OS = LV_OS_NONE，内核里没有锁），
 *       而"切屏"这个动作来自按键线程。所以：
 *         APP_Screen_Next()  只改一个 uint8 索引，跑在 Task_Key_Cmd；
 *         APP_Screen_Step()  发现索引变了才真正 lv_screen_load()，
 *                            跑在 Task_LVGL。
 *       两个线程各写各的变量、单写者单读者，8 位对齐量在 Cortex-M4 上
 *       读写不可分割，因此**不加锁**——与 APP_Step 同一套模型。
 *       代价是切屏最多滞后一个显示节拍（10 ms），按下即切换的体感无差别。
 *       不要在 Next() 里直接调 LVGL：那会把 SPI 传输与 LVGL 的内部状态
 *       拉进按键线程，是这套分层里唯一不能破的线。
 *
 *       三屏的序号由 APP_UI_SCREEN_COUNT 决定（当前 3，循环切换）。
 *
 *       头文件不出现 lv_obj_t，也不出现任何 lv_ 类型：
 *       导出代码的符号被 app_ui_port.h 挡在下面，本模块对外只有三个名字。
 */

#ifndef __APP_SCREEN_H__
#define __APP_SCREEN_H__

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 建立界面并显示首帧
 * @retval 无
 * @note 必须在 Svc_LVGL_Init() 成功之后调用：界面要挂到 display 上。
 *       内部顺序：初始化导出界面（三屏全建、量程统一）→ 加载第 0 屏 →
 *       立刻同步一次计数，让上电首帧就是真实值而不是导出时的演示值。
 *       重复调用会重建界面并泄漏上一份，本工程只在上电时调一次。
 */
void APP_Screen_Init(void);

/**
 * @brief 请求切到下一屏（循环）
 * @retval 无
 * @note **只能由 Task_Key_Cmd 线程调用**（单写者约定，见文件头）。
 *       只做一次加法与一次取模，不碰 LVGL、不碰 SPI、不阻塞，
 *       可以在命令表的 handler 里直接调，不会拖慢 8 格深的事件队列。
 *       真正的切屏发生在下一个 APP_Screen_Step()。
 */
void APP_Screen_Next(void);

/**
 * @brief 把一个显示节拍的数据同步到界面
 * @retval 无
 * @note 由 Task_LVGL 每 10 ms 调用一次（与 Svc_LVGL_Step 同一循环）。
 *       先看有没有待处理的切屏请求，再读 APP_Step_Total() 同步数值；
 *       值没变时整条路径只有两次整数比较，不产生任何 LVGL 调用。
 *       这是**轮询**而不是事件驱动：按键线程只改整数、不唤醒显示线程，
 *       换来的是按键线程与界面线程之间零耦合。
 */
void APP_Screen_Step(void);

#endif /* __APP_SCREEN_H__ */
