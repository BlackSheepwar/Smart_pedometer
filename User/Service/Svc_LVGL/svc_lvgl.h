/**
 * @file svc_lvgl.h
 * @brief 服务层：把 LVGL 的显示服务绑定到 ST7789 设备
 * @author Ricardo
 * @date 2026-11-07
 *
 * @note 第三方库的适配器要放在"需要它的层"，不能塞进 Device：
 *       Device 只回答"这块屏能做什么"（开窗、写像素），不该知道世界上
 *       有没有 LVGL。本服务负责的是跨设备组合——LVGL 内核 ↔ dev_st7789_0，
 *       这正是 Service 的职责。
 *
 *       头文件不出现 lv_ 类型，也不出现 dev_st7789_t：display 指针、渲染
 *       缓冲与面板句柄都留在 .c 里，上层只能调 Init / Step，因此依赖全部
 *       写成 PRIVATE，别人的类型不会顺着头文件传给 APP / TASK。
 *
 *       毫秒时基与毫秒延时都由调用方注入：Service 不读时钟、不提供延时
 *       手段，换 OS 只改 TASK 一处。本服务因此不含任何 OS 调用
 *       （既没有信号量，也没有互斥量）。
 *
 *       本服务只提供两个动作：Init（把面板与 LVGL 内核接起来）与
 *       Step（推进一次定时器与重绘）。**函数名与参数里没有任何界面语义**
 *       （无 screen / switch 字样）：谁有资格换界面、换成哪一张，全部在
 *       APP 层决定，机制与策略分开。
 */

#ifndef __Svc_LVGL_H__
#define __Svc_LVGL_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 毫秒时基来源：返回自系统启动起的毫秒数
 * @note 由 TASK 层用 OS 节拍实现并注入。Service 不关心它来自 SysTick、
 *       HAL_GetTick 还是 osKernelGetTickCount；换 OS 时只改 TASK 一处。
 */
typedef uint32_t (*svc_lvgl_tick_ms_t)(void);

/**
 * @brief 毫秒延时来源
 * @param ms 需要等待的毫秒数
 * @note 由 TASK 层实现并注入，本服务只把它转交给 dev_st7789_0 的上电
 *       时序，不决定用什么手段等待（osDelay 会让出 CPU，忙等则独占）。
 *       做成注入而不是本层自己调 osDelay，是为了让本层保留"换 OS 只改
 *       TASK"的性质。
 */
typedef void (*svc_lvgl_delay_ms_t)(uint32_t ms);

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化 LVGL 显示服务：面板 → 内核 → 时基 → display → 缓冲 → flush
 * @param tick_ms  毫秒时基来源，不可为 NULL；转交给 lv_tick_set_cb()
 * @param delay_ms 毫秒延时来源，不可为 NULL；转交给面板的复位与上电时序
 * @retval 0: 初始化成功
 * @retval -1: ST7789 面板初始化失败（SPI 时序、几何配置或调度器未运行）
 * @retval -2: lv_display_create() 返回 NULL（LVGL 堆不足）
 * @retval -3: 面板当前逻辑宽高与本服务的缓冲/显示常量不一致
 * @retval -4: tick_ms 或 delay_ms 为 NULL
 * @note 必须在**调度器启动之后的线程里**调用：面板初始化的复位时序靠
 *       delay_ms 等待，调度器没跑时延时会全部落空；毫秒时基也依赖内核
 *       已启动才能取到有效计数。
 *       本函数之后才能创建任何 LVGL 对象（APP_Screen_Init() 依赖它）。
 */
int Svc_LVGL_Init(svc_lvgl_tick_ms_t tick_ms, svc_lvgl_delay_ms_t delay_ms);

/**
 * @brief 推进 LVGL 的定时器与重绘队列
 * @retval 无
 * @note 必须在创建本端口的那一个线程里周期调用（建议 5~10 ms 一次）：
 *       LVGL 不是线程安全的，所有 LVGL 调用（含界面切换）都必须与
 *       本函数处于同一线程上下文。
 */
void Svc_LVGL_Step(void);

#endif /* __Svc_LVGL_H__ */
