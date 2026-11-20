/**
 * @file app_screen.h
 * @brief 应用层：界面生命周期、模式切换与数据同步（单屏）
 * @author Ricardo
 * @date 2026-11-10
 *
 * @note 本模块管三件事，且只这三件：
 *         1. 什么时候把界面建起来（Init）；
 *         2. 当前显示哪一块模式面板（Toggle 提请求，Step 落地）；
 *         3. 每个显示节拍把数据送到界面上（Step）。
 *       不管"界面长什么样"（那是 UI_Export/ 下的导出代码，
 *       由 APP_UI_Port 适配），也不管"数据从哪来"（那是将来的 RTC /
 *       GPS / ADC 任务，它们只调本文件的 Set_*）。
 *
 *       ★为什么数据写入和刷屏要分成两级★
 *       LVGL 不是线程安全的（本工程 LV_USE_OS = LV_OS_NONE，内核里没有锁），
 *       而数据来自别的线程。所以：
 *         APP_Screen_Set_*()  只写一个 volatile 整数，跑在数据源线程；
 *         APP_Screen_Step()   发现值变了才调 lv_subject_set_*()，跑在 Task_LVGL。
 *       每个数据项只有一个写入者、一个读取者，8/16/32 位对齐量在 Cortex-M4 上
 *       单条 STR/LDR 不可分割，因此**不加锁**——单写者单读者就够，不需要临界区。
 *       代价是显示最多滞后一个节拍（10 ms），不可感知。
 *       不要在 Set_* 里直接碰 LVGL：那会把 SPI 传输与 LVGL 的内部状态
 *       拉进数据源线程，是这套分层里唯一不能破的线。
 *
 *       ★时间与日期为什么要打包成一个整数★
 *       20:44 是两个字节，分两次写就会有一个节拍读到"新小时 + 旧分钟"。
 *       打包成 hh*100+mm 之后一次 16 位写入，读侧不可能拿到半新半旧的值；
 *       日期同理打包成 yy*10000+mm*100+dd。这一步只是为了让"无需加锁"
 *       这个结论站得住，代价是一条除法。
 *
 *       ★还没接数据源时的行为★
 *       所有数据项初值是"未设置"哨兵（0xFFFF / 0xFFFFFFFF）。Step() 跳过
 *       未设置的项，于是上电后界面留着导出时的演示值（20:44 / 78% / 24.8 /
 *       39.9042N）。接数据源的动作只有一句 Set_*，调用后该项立刻变真实值，
 *       没接的项继续显示演示值——不需要额外的开关或初始化顺序。
 *
 *       头文件不出现 lv_obj_t，也不出现任何 lv_ 类型：
 *       导出代码的符号被 app_ui_port.h 挡在下面，本模块对外只有十个名字。
 */

#ifndef __APP_SCREEN_H__
#define __APP_SCREEN_H__

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
 * @note 本层对外只暴露"哪一块面板"这个语义，按键换页靠它落地。
 *       适配层还有一份 APP_UI_MODE_*，数值必须一致——app_screen.c 里有
 *       静态断言锁住，改一处忘了另一处会直接编译失败。
 */
#define APP_SCREEN_MODE_SPEED   0u

/**
 * @brief 模式面板：经纬度
 * @note 见 APP_SCREEN_MODE_SPEED。
 */
#define APP_SCREEN_MODE_POS     1u

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 建立界面并显示首帧
 * @retval 无
 * @note 必须在 Svc_LVGL_Init() 成功之后调用：界面要挂到 display 上。
 *       内部顺序：初始化导出界面（建常驻屏并加载）→ 把模式钉在"速度"。
 *       重复调用会重建界面并泄漏上一份，本工程只在上电时调一次。
 */
void APP_Screen_Init(void);

/**
 * @brief 请求在"速度 / 经纬度"之间切换
 * @retval 无
 * @note 触发源是 Task_Key_Cmd（PC13）。翻转用 __atomic 异或实现：这个变量
 *       另有初始化的写入点，原子操作避免"某处普通写、某处原子写"的语义冲突。
 *       不碰 LVGL、不碰 SPI、不阻塞，可以在命令表的 handler 里直接调。
 *       真正的换面板发生在下一个 APP_Screen_Step()。
 */
void APP_Screen_Toggle_Mode(void);

/**
 * @brief 直接选定某块模式面板（不再翻转）
 * @param mode APP_SCREEN_MODE_SPEED 或 APP_SCREEN_MODE_POS，其它值忽略
 * @retval 无
 * @note 任意线程可调：命令表的 handler 若有"直达"某页的按键就走这里，
 *       写入是 __atomic_store_n，与 Toggle 的异或共享同一个原子变量。
 *       与 Toggle 的区别只在语义：Toggle 是"不管当前是哪块，翻到另一块"，
 *       本函数是"就选这块"，所以重复按同一颗键不会来回跳。
 */
void APP_Screen_Select_Mode(uint8_t mode);

/**
 * @brief 提交时间
 * @param hour   0~23
 * @param minute 0~59
 * @retval 无
 * @note 任意线程可调（写者是唯一的那个数据源线程）。合法性由
 *       App_UI_Set_Time() 再校验一次：非法值会被丢掉而不是显示 23:99。
 */
void APP_Screen_Set_Time(uint8_t hour, uint8_t minute);

/**
 * @brief 提交日期
 * @param year  完整年份，只显示后两位
 * @param month 1~12
 * @param day   1~31
 * @retval 无
 */
void APP_Screen_Set_Date(uint16_t year, uint8_t month, uint8_t day);

/**
 * @brief 提交电量百分比
 * @param percent 0~100
 * @retval 无
 * @note 传百分比而不是电压：折算属于数据源侧的业务。
 */
void APP_Screen_Set_Battery(uint8_t percent);

/**
 * @brief 提交速度
 * @param speed_d10 速度 × 10（0.1 km/h 为单位），248 = 24.8 km/h
 * @retval 无
 */
void APP_Screen_Set_Speed(uint16_t speed_d10);

/**
 * @brief 提交经纬度
 * @param lat_e6 纬度 × 1e6，北正南负
 * @param lon_e6 经度 × 1e6，东正西负
 * @retval 无
 * @note 两个参数一起提交：界面上一行纬度一行经度，分开写会出现
 *       "新纬度配旧经度"的一帧。
 */
void APP_Screen_Set_Position(int32_t lat_e6, int32_t lon_e6);

/**
 * @brief 提交卫星颗数
 * @param count 0~24
 * @retval 无
 * @note 与经纬度分开提交：没有定位时也可能有星数（未定位置状态），
 *       两者不是同一条数据。
 */
void APP_Screen_Set_Satellites(uint8_t count);

/**
 * @brief 把一个显示节拍的数据同步到界面
 * @retval 无
 * @note 由 Task_LVGL 每 10 ms 调用一次（与 Svc_LVGL_Step 同一循环）。
 *       先落地模式请求，再逐项比对数据；值没变的项一次 subject 写入都不产生
 *       （写 subject 会遍历 observer 并触发重绘，这个短路是必要的）。
 *       这是**轮询**而不是事件驱动：数据源只改整数、不唤醒显示线程，
 *       换来的是两条线程之间零耦合。
 */
void APP_Screen_Step(void);

#endif /* __APP_SCREEN_H__ */
