/**
 * @file app_level.c
 * @brief 应用层：档位业务与显示
 * @author Ricardo
 * @date 2026-10-23
 *
 * @note 这一层只管"档位是几、灯怎么亮"，不做三件事：
 *       - 不建线程：线程在 CubeMX 的 .ioc 里配，线程体在 User/TASK 下；
 *       - 不查表：按键事件到动作的映射在 app_key_cmd.c；
 *       - 不驱动按键服务：扫描节拍属于 TASK，Task_Key_Scan 直接调
 *         Svc_Key_Logic_Tick()，本层不转包。
 *
 *       档位范围 0~8：0 全灭，8 全亮，加满回绕到 0、减到 0 回绕到 8。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include "app_level.h"
// 功能包含
#include "dev_led_bar.h"

/*==============================================================================
 * 静态变量
 *============================================================================*/
static uint8_t s_level = 0;                 /* 当前档位 */

/**
 * @brief 将连续点亮档位转换为灯带位图
 * @param level 连续点亮灯数，范围为 0~8
 * @retval 对应位图，低位先点亮
 */
static uint8_t APP_Level_To_Pattern(uint8_t level)
{
    if (level >= 8u) {
        return 0xFFu;
    }

    return (uint8_t)((1u << level) - 1u);
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 一次性初始化：档位归零，灯带全灭
 * @retval 无
 * @note 由 Src/freertos.c 的 MX_FREERTOS_Init() 在调度器启动前调用。
 *       只写 GPIO 与清零静态量，不碰内核，所以放在 osKernelStart() 之前安全。
 *       移植说明：frame 里这个函数只有声明没有定义（没人调用所以不报错），
 *       本工程补上定义并在启动链里调用，让"上电档位为 0"由代码保证，
 *       而不是依赖 MX_GPIO_Init() 恰好把 PA0~PA7 复位成低电平。
 */
void APP_Level_Init(void)
{
    s_level = 0u;
    Dev_LED_Bar_Set(&dev_led_bar_0, APP_Level_To_Pattern(s_level));
}

/**
 * @brief 档位步进（带回绕）
 * @param delta +1 或 -1
 * @note 由按键命令表的 handler 调用：0 → 1 → … → 8 → 0，反向同理。
 */
void APP_Level_Step(int8_t delta)
{
    int16_t span = (int16_t)(Dev_LED_Bar_Count(&dev_led_bar_0) + 1u);
    int16_t next = (int16_t)((int16_t)s_level + (int16_t)delta);

    if (next < 0) {
        next = (int16_t)(next + span);
    } else if (next >= span) {
        next = (int16_t)(next - span);
    }

    s_level = (uint8_t)next;
    Dev_LED_Bar_Set(&dev_led_bar_0, APP_Level_To_Pattern(s_level));
}
