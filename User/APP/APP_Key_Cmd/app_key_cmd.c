/**
 * @file app_key_cmd.c
 * @brief 按键命令分发（配置表驱动）
 * @author Ricardo
 * @date 2026-10-23
 *
 * @note 参照 RC_Dog 的 app_key_cmd.c：命令不写在 if 里，全部集中到
 *       APP_KEY_CMD_TABLE 一张表，表里一行 = 一个"键 × 事件 → 动作"。
 *       新增按键或新增事件，只需写一个 handler 再在表里加一行。
 *
 *       当前的分工：
 *         单击   → 档位 ±1
 *         长按   → 满 300 ms 后开始连发 REPEAT，每次 ±1（灯带跟着一格一格动）
 *
 *       处理函数一律无参、只做短动作：它们跑在 Task_Key_Cmd 线程里，
 *       一旦阻塞，8 格事件队列就会撑满、新事件被丢弃。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
// 功能包含
#include "app_key_cmd.h"
#include "app_level.h"
#include "dev_led.h"
#include "dev_st7789.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/**
 * @brief 测试用图案缓冲大小（字节）
 * @note 必须是偶数。512 B = 256 像素，放 .bss 不占任务栈；图案越大、
 *       帧内 DMA 启动次数越少（512 B 刷整屏 215 次，4096 B 只要 27 次），
 *       这里取 512 是拿时间换 RAM。
 */
#define APP_TEST_PAT_BYTES      512u
/** @brief 颜色键循环的档位数 */
#define APP_TEST_COLOR_COUNT    3u

/*==============================================================================
 * 内部类型
 *============================================================================*/
/**
 * @brief 命令表条目：哪个键的哪个事件调哪个处理函数
 */
typedef struct {
    const dev_key_t *key;
    key_event_t      event;
    void            (*handler)(void);
} app_key_cmd_entry_t;

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief 测试用纯色图案，由 APP_Key_Cmd_TestFillScreen() 按当前颜色重填 */
static uint8_t s_test_pat[APP_TEST_PAT_BYTES];

/*==============================================================================
 * 按键事件处理函数
 *============================================================================*/
static void Key_Inc(void)
{
    APP_Level_Step(1);
}

static void Key_Dec(void)
{
    APP_Level_Step(-1);
}

static void Key_ON(void)
{
    Dev_LED_Toggle(&dev_led_1);
}

static void Key_OFF(void)
{
    Dev_LED_Write(&dev_led_1, LED_OFF);
}

/**
 * @brief 颜色键：红 → 绿 → 蓝 → 红 循环
 * @retval 无
 * @note 这是**测试**动作，整屏重绘会阻塞约 85 ms，超出"handler 必须短"
 *       的约定；量产界面应改成只发一次重绘请求，由显示任务去画。
 *       设备层不提供 FillScreen，这里用"构造图案 + 重复写"两步实现，
 *       走的都是 SetWindow / WritePattern 这些基础显示函数。
 */
static void Key_Color(void)
{
    static const uint16_t colors[APP_TEST_COLOR_COUNT] = {
        0xF800u,   /* 红 */
        0x07E0u,   /* 绿 */
        0x001Fu,   /* 蓝 */
    };
    static uint8_t s_color_index = 0xFFu;

    uint8_t index = (uint8_t)(((uint32_t)s_color_index + 1u) % APP_TEST_COLOR_COUNT);

    if (APP_Key_Cmd_TestFillScreen(colors[index]) == 0) {
        s_color_index = index;
    }
}

/*==============================================================================
 * 命令表
 *============================================================================*/
/**
 * @brief 键 × 事件 → 处理函数
 * @note 只列有意义的组合；没列的组合查表查不到，直接丢弃。
 *       档位在按下（DOWN）当拍就动，长按由 LONG 与 REPEAT 负责连发，
 *       CLICK / LONG_UP 不产生动作。
 */
static const app_key_cmd_entry_t APP_KEY_CMD_TABLE[] = {
    { &dev_key_inc, KEY_EVENT_DOWN,    Key_Inc    },
    { &dev_key_inc, KEY_EVENT_LONG,    Key_Inc    },
    { &dev_key_inc, KEY_EVENT_REPEAT,  Key_Inc    },

    { &dev_key_dec, KEY_EVENT_DOWN,    Key_Dec    },
    { &dev_key_dec, KEY_EVENT_LONG,    Key_Dec    },
    { &dev_key_dec, KEY_EVENT_REPEAT,  Key_Dec    },

    { &dev_key_led, KEY_EVENT_DOWN,    Key_ON     },
    { &dev_key_led, KEY_EVENT_LONG_UP, Key_OFF    },

    { &dev_key_color, KEY_EVENT_DOWN,  Key_Color  },
};

/**
 * @brief 命令表行数
 */
#define APP_KEY_CMD_COUNT  (sizeof(APP_KEY_CMD_TABLE) / sizeof(APP_KEY_CMD_TABLE[0]))

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 按键命令分发入口
 * @param key   按键设备句柄（与表里存的是同一个对象地址，按地址比较）
 * @param event 按键事件（DOWN / CLICK / LONG / REPEAT / LONG_UP）
 * @note 由 Task_Key_Cmd 每取到一条事件调用一次；查不到组合就静默丢弃。
 *       句柄是 const 全局对象，地址唯一，所以"是不是这个键"等价于
 *       "是不是这个地址"；运行期拷贝句柄会让这条判断失效。
 */
void APP_Key_Cmd_Packet(const dev_key_t *key, key_event_t event)
{
    uint8_t index;

    for (index = 0u; index < (uint8_t)APP_KEY_CMD_COUNT; ++index) {
        if ((APP_KEY_CMD_TABLE[index].key == key) &&
            (APP_KEY_CMD_TABLE[index].event == event)) {
            APP_KEY_CMD_TABLE[index].handler();
            return;
        }
    }
}

/**
 * @brief 用纯色重绘整屏（测试用）
 * @param color 颜色，RGB565
 * @retval 0: 成功
 * @retval -1: 面板尚未就绪，或尺寸为 0
 * @retval -2: 开窗或传输失败
 * @note 三步都是基础显示操作，没有一处依赖"填色"这种业务接口：
 *       读尺寸 → 开全屏窗口 → 把颜色铺满图案缓冲 → 重复写图案。
 *       颜色铺进缓冲时高字节在前，这是 ST7789 的字节序要求。
 */
int APP_Key_Cmd_TestFillScreen(uint16_t color)
{
    uint32_t index;
    uint32_t w = (uint32_t)Dev_ST7789_Width(&dev_st7789_0);
    uint32_t h = (uint32_t)Dev_ST7789_Height(&dev_st7789_0);

    if ((w == 0u) || (h == 0u)) {
        return -1;
    }

    if (Dev_ST7789_SetWindow(&dev_st7789_0, 0u, 0u,
                             (uint16_t)(w - 1u), (uint16_t)(h - 1u)) != 0) {
        return -2;
    }

    for (index = 0u; index < APP_TEST_PAT_BYTES; index += 2u) {
        s_test_pat[index]      = (uint8_t)(color >> 8);
        s_test_pat[index + 1u] = (uint8_t)(color & 0xFFu);
    }

    return Dev_ST7789_WritePattern(&dev_st7789_0, s_test_pat,
                                   APP_TEST_PAT_BYTES, w * h * 2u);
}
