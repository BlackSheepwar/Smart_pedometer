/**
 * @file svc_key_cmd.c
 * @brief 服务层：按键命令表分发实现
 * @author Ricardo
 * @date 2026-11-09
 *
 * @note 实现只有一次线性扫描：表通常不到 20 行，查表开销可以忽略；
 *       改成二分或哈希需要表有序/建索引，反而把"加一行就能用"的
 *       配置表便利性弄丢了。
 *
 *       本模块不持有任何状态：没有 static 变量，没有 Init，因此天然
 *       可重入（多个线程各传各的表互不影响），也能脱离硬件在 PC 上单测。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
// 功能包含
#include "svc_key_cmd.h"

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 按键命令分发：在给定命令表中查 { key, event } 并执行对应 handler
 * @param table 命令表首地址，不可为 NULL
 * @param count 表行数，必须与 table 的实际长度一致（本函数不做越界校验）
 * @param key   按键设备句柄，按对象地址比较；不可为 NULL
 * @param event 按键事件（KEY_EVENT_*）
 * @retval 1: 命中且已执行 handler
 * @retval 0: 未命中或参数非法，未执行任何 handler
 * @note 句柄是 const 全局对象，地址唯一，所以"是不是这个键"等价于
 *       "是不是这个地址"；运行期拷贝句柄会让这条判断失效。
 *       命中但 handler 为 NULL 时按未绑定处理，直接返回 0，不再继续扫表。
 */
uint8_t Svc_Key_Cmd_Dispatch(const svc_key_cmd_entry_t *table, uint8_t count,
                             const dev_key_t *key, key_event_t event)
{
    uint8_t index;

    /* ---------- 参数校验：空表或空句柄直接判未命中 ---------- */
    if ((table == NULL) || (key == NULL)) {
        return 0u;
    }

    /* ---------- 线性查表：{ 键, 事件 } 同时相等才算命中 ---------- */
    for (index = 0u; index < count; ++index) {
        if ((table[index].key != key) || (table[index].event != event)) {
            continue;
        }

        if (table[index].handler == NULL) {
            return 0u;          /* 动作未绑定：与"表里没这一行"同义 */
        }

        table[index].handler();
        return 1u;
    }

    return 0u;
}
