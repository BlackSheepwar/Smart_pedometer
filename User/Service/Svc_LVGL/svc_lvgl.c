/**
 * @file svc_lvgl.c
 * @brief 服务层实现：把 LVGL 的显示与触摸绑定到 ST7789 + TSC2046
 * @author Ricardo
 * @date 2026-11-07
 *
 * @note 本文件是 LVGL 这个第三方库在本工程里的唯一适配器，放在 Service：
 *         - 时基与延时：都由调用方注入（svc_lvgl_tick_ms_t /
 *           svc_lvgl_delay_ms_t），本层不读时钟、不提供延时手段，
 *           换 OS 只改 TASK 一处。本文件因此不含任何 OS 调用；
 *         - 刷屏：flush 回调 → Dev_ST7789_SetWindow() + WritePixels()。
 *           BSP 的 SPI 传输是"抢互斥量 + DMA + 任务通知"的阻塞式完成，
 *           像素写完即可直接 lv_display_flush_ready()，不需要额外的
 *           DMA 完成中断回调，也不需要第二块"传输中"缓冲；
 *         - 输入：两路并存。按键仍走既有"消抖服务 → 事件队列 → APP"链路，
 *           不经过 LVGL；触摸走标准 LVGL indev——read_cb 每帧被
 *           lv_timer_handler() 调一次，采到点就填进 data，LVGL 自己负责
 *           点击/长按的时序判定。所以"点一下切面板"不需要本层写任何
 *           消抖或计时逻辑，也不需要额外的中断；"左右滑翻页"的位移判定
 *           同样不在本层（在 APP_UI_Port 的全屏手势层里），本层只负责
 *           把按下点如实喂进去。
 *
 *       缓冲策略：部分缓冲单块 240×40 行（19 200 B）。整屏 240×320×2 =
 *       153 600 B，128 KB RAM 放不下，LVGL 按行分片渲染并逐片 flush，
 *       代价是同一区域可能被刷多次，换来 RAM 占用下降一个数量级。
 *       行数取 40 是折中：再少则整屏被切成更多片、SPI 开窗次数上升；
 *       再多则 .bss 里的缓冲与 LV_MEM_SIZE（32 KB）合计逼近 128 KB。
 *
 *       单实例：display / indev 指针与 draw buffer 都是模块级 static，只服务
 *       dev_st7789_0 与 dev_tsc2046_0，与两个设备层的单实例约定一致。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include "svc_lvgl.h"
// 功能包含
#include "dev_st7789.h"
#include "dev_tsc2046.h"
#include "lvgl.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 面板逻辑宽度（像素），与 dev_st7789_0 的 width 一致 */
#define SVC_LVGL_HOR_RES            240u
/** @brief 面板逻辑高度（像素），与 dev_st7789_0 的 height 一致 */
#define SVC_LVGL_VER_RES            320u
/** @brief 部分缓冲的行数：40 行 = 19 200 B，是 RAM 与重绘次数的折中 */
#define SVC_LVGL_BUF_LINES          40u
/** @brief 缓冲字节数：RGB565 每像素 2 字节 */
#define SVC_LVGL_BUF_BYTES          (SVC_LVGL_HOR_RES * SVC_LVGL_BUF_LINES * 2u)

/*==============================================================================
 * 内部变量
 *============================================================================*/
/**
 * @brief LVGL 渲染缓冲（部分模式）
 * @note 4 字节对齐：软件渲染按 uint32_t 访问，未对齐会在 Cortex-M4 上
 *       触发 UNALIGNED 用法故障。缓冲放 .bss 不占 LVGL 堆，也不占任务栈。
 */
static uint8_t s_draw_buf[SVC_LVGL_BUF_BYTES] __attribute__((aligned(4)));

/** @brief 本端口的 display 对象，Init 成功后非 NULL */
static lv_display_t *s_disp;

/** @brief 本端口的触摸 indev 对象，Init 成功后非 NULL */
static lv_indev_t *s_indev;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把一块渲染好的区域写到面板
 * @param disp   display 对象，由 LVGL 回传
 * @param area   待刷区域（闭区间，坐标已落在屏幕内）
 * @param px_map 像素数据，大端 RGB565
 * @retval 无
 * @note 无论写屏成败都必须调 lv_display_flush_ready()：漏掉一次，LVGL 的
 *       flushing 标志就清不掉，后续所有重绘都会被永久挂起。
 *       LVGL 的类型只在本函数与 s_disp 上出现，头文件一个字都不带。
 */
static void Svc_LVGL_Flush(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map)
{
    uint32_t width;
    uint32_t height;
    int32_t  x0;
    int32_t  y0;
    int32_t  x1;
    int32_t  y1;

    if ((area == NULL) || (px_map == NULL)) {
        lv_display_flush_ready(disp);
        return;
    }

    /* ---------- 1. 坐标夹紧到面板范围内 ---------- */
    x0 = (area->x1 < 0) ? 0 : area->x1;
    y0 = (area->y1 < 0) ? 0 : area->y1;
    x1 = (area->x2 >= (int32_t)SVC_LVGL_HOR_RES) ?
         ((int32_t)SVC_LVGL_HOR_RES - 1) : area->x2;
    y1 = (area->y2 >= (int32_t)SVC_LVGL_VER_RES) ?
         ((int32_t)SVC_LVGL_VER_RES - 1) : area->y2;

    /* ---------- 2. 开窗并整块送像素；区域非法时跳过传输 ---------- */
    if ((x1 >= x0) && (y1 >= y0)) {
        width  = (uint32_t)(x1 - x0 + 1);
        height = (uint32_t)(y1 - y0 + 1);

        if (Dev_ST7789_SetWindow(&dev_st7789_0, (uint16_t)x0, (uint16_t)y0,
                                 (uint16_t)x1, (uint16_t)y1) == 0) {
            (void)Dev_ST7789_WritePixels(&dev_st7789_0, px_map,
                                         width * height * 2u);
        }
    }

    /* ---------- 3. 通知 LVGL 这片已送完，可以接着渲染下一片 ---------- */
    lv_display_flush_ready(disp);
}

/**
 * @brief 读一次触摸状态喂给 LVGL
 * @param indev 由 LVGL 回传的输入设备对象，本函数不使用
 * @param data  输出：按下状态与坐标
 * @retval 无
 * @note 每个渲染周期被调用一次（当前 10 ms），不能在里面做重活：
 *       按下判定只读一个 GPIO，取坐标才动一次 SPI。
 *       读失败一律报"未按下"——报按下却带着上一次的旧坐标，会让界面
 *       在手指已经离开之后误触发一次点击。
 */
static void Svc_LVGL_Touch_Read(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x;
    uint16_t y;

    (void)indev;

    if ((Dev_TSC2046_Is_Pressed(&dev_tsc2046_0) != 0u) &&
        (Dev_TSC2046_Read_Pixel(&dev_tsc2046_0, &x, &y) == 0)) {
        data->point.x = (int32_t)x;
        data->point.y = (int32_t)y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state   = LV_INDEV_STATE_RELEASED;
    }
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化 LVGL 显示与输入服务：面板 → 内核 → 时基 → display → 缓冲
 *        → flush → 触摸 indev
 * @param tick_ms  毫秒时基来源，不可为 NULL；转交给 lv_tick_set_cb()
 * @param delay_ms 毫秒延时来源，不可为 NULL；转交给面板的复位与上电时序
 * @retval 0: 初始化成功
 * @retval -1: ST7789 面板初始化失败（SPI 时序、几何配置或调度器未运行）
 * @retval -2: lv_display_create() 返回 NULL（LVGL 堆不足）
 * @retval -3: 面板当前逻辑宽高与本服务的缓冲/显示常量不一致
 * @retval -4: tick_ms 或 delay_ms 为 NULL
 * @retval -5: 触摸控制器初始化失败（句柄为空，或几何/量程参数矛盾）
 * @retval -6: lv_indev_create() 返回 NULL（LVGL 堆不足）
 * @note 必须在**调度器启动之后的线程里**调用：面板初始化的复位时序靠
 *       delay_ms 等待（TASK 注入的通常是 osDelay），调度器没跑时延时会
 *       全部落空；毫秒时基也依赖内核已启动才能取到有效计数。
 *       本函数之后才能创建任何 LVGL 对象（APP_Screen_Init() 依赖它）。
 *
 *       顺序不能颠倒：display 的颜色格式必须在 set_buffers 之前设置，
 *       因为缓冲能容纳多少行是按"每像素字节数"折算的；RGB565 与
 *       RGB565_SWAPPED 同为 2 字节，这里顺序只是保持语义正确。
 *
 *       尺寸校验不是多余：面板旋转由 Dev_ST7789_SetRotation() 动态改变，
 *       而这里的分辨率与缓冲行宽是编译期常量。一旦有人把方向调到 90°，
 *       不校验就会按 240 宽去开窗、drv 却按 320 宽做越界判断，表现为
 *       "整屏只刷出左半边"，从现象很难反推到这一行。
 */
int Svc_LVGL_Init(svc_lvgl_tick_ms_t tick_ms, svc_lvgl_delay_ms_t delay_ms)
{
    if ((tick_ms == NULL) || (delay_ms == NULL)) {
        return -4;
    }

    /* ---------- 1. 面板先于 LVGL：失败时不留半个 LVGL 实例 ---------- */
    if (Dev_ST7789_Init(&dev_st7789_0, delay_ms) != 0) {
        return -1;
    }

    /* ---------- 2. 逻辑尺寸必须与编译期常量一致（含旋转检查） ---------- */
    if ((Dev_ST7789_Width(&dev_st7789_0)  != SVC_LVGL_HOR_RES) ||
        (Dev_ST7789_Height(&dev_st7789_0) != SVC_LVGL_VER_RES)) {
        return -3;
    }

    /* ---------- 3. LVGL 内核与时基：时钟由 TASK 注入，本层不读 ---------- */
    lv_init();
    lv_tick_set_cb(tick_ms);

    /* ---------- 4. display、颜色格式、部分缓冲与 flush 回调 ---------- */
    s_disp = lv_display_create((int32_t)SVC_LVGL_HOR_RES,
                               (int32_t)SVC_LVGL_VER_RES);
    if (s_disp == NULL) {
        return -2;
    }

    /* ST7789 要大端（高字节在前），SWAPPED 让 LVGL 直接产出大端像素，
       flush 里零拷贝送屏；模板已开 LV_DRAW_SW_SUPPORT_RGB565_SWAPPED */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, Svc_LVGL_Flush);

    /* ---------- 5. 触摸：设备自检在前，再由 LVGL 建指针型 indev ---------- */
    /* 触摸是当前唯一的交互入口，所以初始化失败按整体失败处理：
       与其给出一块"能画但点不动"的界面让人去猜是驱动坏了还是界面没响应，
       不如让 TASK 直接停住并把 -5 暴露出来。 */
    if (Dev_TSC2046_Init(&dev_tsc2046_0) != 0) {
        return -5;
    }

    s_indev = lv_indev_create();
    if (s_indev == NULL) {
        return -6;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, Svc_LVGL_Touch_Read);

    return 0;
}

/**
 * @brief 推进 LVGL 的定时器与重绘队列
 * @retval 无
 * @note 必须在创建本端口的那一个线程里周期调用（建议 5~10 ms 一次）：
 *       LVGL 不是线程安全的，所有 LVGL 调用（含界面切换）都必须与
 *       本函数处于同一线程上下文。
 *       lv_timer_handler() 返回"下次该被调用的间隔"，本工程用固定周期
 *       轮询，返回值丢弃；需要省电时由 TASK 层拿它改成动态延时。
 */
void Svc_LVGL_Step(void)
{
    (void)lv_timer_handler();
}