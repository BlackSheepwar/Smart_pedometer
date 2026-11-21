/**
 * @file bsp_iic.c
 * @brief IIC 主机驱动层：启动 DMA 并登记完成回调，不等待、不持锁
 * @author Ricardo
 * @date 2026-10-27
 *
 * @note 设计模型：
 *       1. 每路总线一个常量句柄（hal 指针 + 运行时块地址），运行期状态
 *          （是否就绪、忙、当前时钟、完成回调）全在运行时块里；
 *       2. 本层只"启动"：BSP_IIC_StartTransfer() 把命令交给 HAL 的 DMA
 *          接口，登记 done 后立刻返回，不持锁、不睡等；
 *       3. 完成/错误中断只做三件事：摘掉登记、清忙标志、调用 done；
 *       4. 超时判定与复位由调用者发起：BSP_IIC_Abort() 先把 HAL 状态机
 *          强制拉回 READY，再 DeInit/Init，否则外设停在 BUSY 时 DeInit
 *          会直接返回 HAL_BUSY、MspDeInit 不执行，越复位越坏。
 *
 *       外设中断必须开：DMA 只搬字节，一次传输的 ADDR / BTF / STOPF 收尾
 *       仍由 I2Cx 的 EV 中断推进，出错走 ER 中断。只开 DMA 流中断、不开
 *       外设中断的话，每笔传输都会永远等不到完成。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "bsp_iic.h"
// 功能包含
#include "i2c.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 从机地址上限：7 bit 地址，未左移 */
#define BSP_IIC_ADDR_MAX            0x7FU

/** @brief 寄存器子地址的最大字节数 */
#define BSP_IIC_MEM_ADDR_SIZE_MAX   2U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief IIC 总线运行时块
 * @note 每次初始化和每笔搬运都会读写，所以不能放进 const 句柄。
 */
typedef struct {
    bsp_iic_done_t volatile done;      /* 本笔完成回调，空闲为 NULL */
    void *volatile          ctx;       /* 回调上下文，与 done 同生共死 */
    volatile uint8_t        busy;      /* 1: 有搬运未收尾 */
    volatile uint8_t        initialized; /* 1: 已完成初始化 */
    uint32_t                clk_now;   /* 当前生效的总线时钟（Hz） */
    uint32_t                clk_init;  /* 初始化时登记的总线时钟（Hz） */
} bsp_iic_rt_t;

/**
 * @brief IIC 总线对象
 * @note  hal 是 void *，避免公共头文件引入 HAL 类型；rt 指向运行期状态。
 */
struct bsp_iic {
    void            *hal; /* HAL I2C 句柄（I2C_HandleTypeDef *） */
    bsp_iic_rt_t    *rt;  /* 运行期状态 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/* ---- 新增一路 IIC：照抄一行，改 HAL 句柄；bsp_iic.h 同步加一行 extern ---- */
static bsp_iic_rt_t bsp_iic1_rt;

const bsp_iic_t bsp_iic1 = {
    .hal = &hi2c1,
    .rt  = &bsp_iic1_rt,
};

/** @brief 总线表：本层管理的全部总线，初始化与中断反查都按它轮询
 *  @note 表长由行数算出，不需要终止符。 */
static const bsp_iic_t *const s_buses[] = {
    &bsp_iic1,
};

/** @brief 总线表长度：由表的行数算出 */
static const size_t s_bus_count = sizeof(s_buses) / sizeof(s_buses[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 按 HAL 句柄查找对应的 BSP 总线对象
 * @param hi2c HAL IIC 句柄
 * @retval 非 NULL: 对应的总线对象
 * @retval NULL: 不是本驱动管理的句柄
 */
static const bsp_iic_t *BSP_IIC_FindBus(I2C_HandleTypeDef *hi2c)
{
    size_t i;

    if (hi2c == NULL) {
        return NULL;
    }

    for (i = 0U; i < s_bus_count; i++) {
        if (s_buses[i]->hal == (void *)hi2c) {
            return s_buses[i];
        }
    }
    return NULL;
}

/**
 * @brief 按命令模式启动一次 HAL DMA 传输
 * @param bus 总线对象
 * @param msg 传输命令
 * @retval HAL_OK: 启动成功
 * @retval 其他: HAL 启动失败
 * @note 7 bit 地址在这里左移成 HAL 需要的 8 bit 形式；MEM 模式用
 *       HAL_I2C_Mem_*_DMA，一次调用完成子地址写 + repeated START。
 */
static HAL_StatusTypeDef BSP_IIC_StartHal(const bsp_iic_t *bus,
                                          const bsp_iic_msg_t *msg)
{
    I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)bus->hal;
    uint16_t dev_addr = (uint16_t)(msg->dev_addr << 1);
    uint16_t mem_size;

    if (msg->mode == (uint8_t)BSP_IIC_MODE_MEM) {
        mem_size = (msg->mem_addr_size == 2U) ?
                   I2C_MEMADD_SIZE_16BIT : I2C_MEMADD_SIZE_8BIT;
        if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
            return HAL_I2C_Mem_Write_DMA(hi2c, dev_addr, msg->mem_addr,
                                         mem_size, msg->data, msg->len);
        }
        return HAL_I2C_Mem_Read_DMA(hi2c, dev_addr, msg->mem_addr,
                                    mem_size, msg->data, msg->len);
    }

    if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
        return HAL_I2C_Master_Transmit_DMA(hi2c, dev_addr,
                                           msg->data, msg->len);
    }
    return HAL_I2C_Master_Receive_DMA(hi2c, dev_addr, msg->data, msg->len);
}

/**
 * @brief 中断收尾：摘掉登记、清忙标志、调用完成回调
 * @param bus    总线对象
 * @param status 本次结果
 * @retval 无
 * @note 只由 HAL 完成/错误回调调用。先取局部量再清登记，最后才调 done：
 *       回调里若立刻启动下一笔，新的登记不会被这一笔的清理踩掉。
 */
static void BSP_IIC_CompleteFromISR(const bsp_iic_t *bus,
                                    bsp_iic_status_t status)
{
    bsp_iic_rt_t  *rt = bus->rt;
    bsp_iic_done_t done = rt->done;
    void          *ctx = rt->ctx;

    rt->done = NULL;
    rt->ctx  = NULL;
    rt->busy = 0U;

    if (done != NULL) {
        done(ctx, status);
    }
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化 IIC 驱动：逐路登记 HAL 句柄、外设状态与初始时钟
 * @retval BSP_IIC_OK: 全部实例就绪（已就绪的实例会被跳过）
 * @retval BSP_IIC_ERR_STATE: 某一路缺 HAL 句柄，或外设不在 READY
 * @note 必须在 MX_I2Cx_Init() 之后调用；不碰任何 OS 对象。
 */
bsp_iic_status_t BSP_IIC_Init(void)
{
    size_t i;

    for (i = 0U; i < s_bus_count; i++) {
        const bsp_iic_t *bus = s_buses[i];
        I2C_HandleTypeDef *hi2c;
        bsp_iic_rt_t *rt;

        if (bus->rt->initialized != 0U) {
            continue;
        }
        if (bus->hal == NULL) {
            return BSP_IIC_ERR_STATE;
        }

        hi2c = (I2C_HandleTypeDef *)bus->hal;
        if (HAL_I2C_GetState(hi2c) != HAL_I2C_STATE_READY) {
            return BSP_IIC_ERR_STATE;
        }

        rt = bus->rt;
        rt->done       = NULL;
        rt->ctx        = NULL;
        rt->busy       = 0U;
        rt->clk_now    = hi2c->Init.ClockSpeed;
        rt->clk_init   = hi2c->Init.ClockSpeed;
        rt->initialized = 1U;
    }
    return BSP_IIC_OK;
}

/**
 * @brief 设置总线时钟
 * @param bus 总线句柄，不能为 NULL
 * @param hz  目标时钟（Hz）；传 0 表示回到初始化时登记的值
 * @retval BSP_IIC_OK: 已生效（或本来就等于目标值）
 * @retval BSP_IIC_ERR_PARAM: 句柄为空，或目标值为 0 且没有可回落的初值
 * @retval BSP_IIC_ERR_STATE: 未初始化，或上一笔搬运还没收尾
 * @retval BSP_IIC_ERR_HAL: HAL 拒绝重新初始化
 * @note 时钟是整条总线共用的，所以只能在空闲时改；失败时把 Init 里的
 *       时钟值改回原值并重试一次初始化，避免留下一份半改的配置。
 */
bsp_iic_status_t BSP_IIC_SetSpeed(const bsp_iic_t *bus, uint32_t hz)
{
    I2C_HandleTypeDef *hi2c;
    bsp_iic_rt_t *rt;
    uint32_t target;

    if (bus == NULL || bus->hal == NULL) {
        return BSP_IIC_ERR_PARAM;
    }

    rt = bus->rt;
    if (rt->initialized == 0U) {
        return BSP_IIC_ERR_STATE;
    }
    if (rt->busy != 0U) {
        return BSP_IIC_ERR_STATE;
    }

    target = (hz != 0U) ? hz : rt->clk_init;
    if (target == 0U) {
        return BSP_IIC_ERR_PARAM;
    }
    if (target == rt->clk_now) {
        return BSP_IIC_OK;
    }

    hi2c = (I2C_HandleTypeDef *)bus->hal;
    hi2c->Init.ClockSpeed = target;
    if (HAL_I2C_Init(hi2c) != HAL_OK) {
        hi2c->Init.ClockSpeed = rt->clk_now;
        (void)HAL_I2C_Init(hi2c);
        return BSP_IIC_ERR_HAL;
    }

    rt->clk_now = target;
    return BSP_IIC_OK;
}

/**
 * @brief 启动一次搬运并立即返回
 * @param bus  总线句柄，不能为 NULL
 * @param msg  传输命令，不能为 NULL；data 在 done 之前必须保持有效
 * @param done 完成回调，可以为 NULL（只能靠 IsBusy 轮询）
 * @param ctx  回调上下文，原样回传
 * @retval BSP_IIC_OK: 已交给外设，完成点只在 done
 * @retval BSP_IIC_ERR_PARAM: 地址、方向、模式、长度或缓冲区非法
 * @retval BSP_IIC_ERR_STATE: 未初始化，或上一笔还没收尾
 * @retval BSP_IIC_ERR_HAL: HAL 拒绝启动（启动失败时已复位外设）
 * @note 启动失败会顺手复位外设：多半是上一笔的 STOP 没走完或 DMA 通道
 *       还挂着，不复位的话下一笔同样起不来。
 */
bsp_iic_status_t BSP_IIC_StartTransfer(const bsp_iic_t *bus,
                                       const bsp_iic_msg_t *msg,
                                       bsp_iic_done_t done, void *ctx)
{
    bsp_iic_rt_t *rt;

    if (bus == NULL || msg == NULL) {
        return BSP_IIC_ERR_PARAM;
    }
    if (bus->hal == NULL) {
        return BSP_IIC_ERR_PARAM;
    }

    rt = bus->rt;
    if (rt->initialized == 0U) {
        return BSP_IIC_ERR_STATE;
    }
    if (rt->busy != 0U) {
        return BSP_IIC_ERR_STATE;
    }

    if (msg->data == NULL || msg->len == 0U) {
        return BSP_IIC_ERR_PARAM;
    }
    if (msg->dev_addr > BSP_IIC_ADDR_MAX) {
        return BSP_IIC_ERR_PARAM;
    }
    if (msg->dir > (uint8_t)BSP_IIC_DIR_RX ||
        msg->mode > (uint8_t)BSP_IIC_MODE_MEM) {
        return BSP_IIC_ERR_PARAM;
    }
    if (msg->mode == (uint8_t)BSP_IIC_MODE_MEM &&
        (msg->mem_addr_size == 0U ||
         msg->mem_addr_size > BSP_IIC_MEM_ADDR_SIZE_MAX)) {
        return BSP_IIC_ERR_PARAM;
    }

    rt->done = done;
    rt->ctx  = ctx;
    rt->busy = 1U;

    if (BSP_IIC_StartHal(bus, msg) != HAL_OK) {
        rt->done = NULL;
        rt->ctx  = NULL;
        rt->busy = 0U;
        (void)BSP_IIC_Abort(bus);
        return BSP_IIC_ERR_HAL;
    }

    return BSP_IIC_OK;
}

/**
 * @brief 查询总线是否还有搬运未收尾
 * @param bus 总线句柄
 * @retval 1: 忙
 * @retval 0: 空闲（句柄为空也返回 0）
 */
uint8_t BSP_IIC_IsBusy(const bsp_iic_t *bus)
{
    if (bus == NULL || bus->rt == NULL) {
        return 0U;
    }
    return (bus->rt->busy != 0U) ? 1U : 0U;
}

/**
 * @brief 中止当前搬运并把外设复位到可用状态
 * @param bus 总线句柄
 * @retval BSP_IIC_OK: 已复位
 * @retval BSP_IIC_ERR_PARAM: 句柄为空
 * @retval BSP_IIC_ERR_STATE: 未初始化
 * @retval BSP_IIC_ERR_HAL: 复位后 HAL 重新初始化失败
 * @note 先摘登记再动外设：迟到的完成中断不会再回调；这一笔的调用者要
 *       自己判失败。
 */
bsp_iic_status_t BSP_IIC_Abort(const bsp_iic_t *bus)
{
    I2C_HandleTypeDef *hi2c;
    bsp_iic_rt_t *rt;

    if (bus == NULL || bus->hal == NULL) {
        return BSP_IIC_ERR_PARAM;
    }

    rt = bus->rt;
    if (rt->initialized == 0U) {
        return BSP_IIC_ERR_STATE;
    }

    rt->done = NULL;
    rt->ctx  = NULL;
    rt->busy = 0U;

    hi2c = (I2C_HandleTypeDef *)bus->hal;
    hi2c->State = HAL_I2C_STATE_READY;
    hi2c->Lock  = HAL_UNLOCKED;

    (void)HAL_I2C_DeInit(hi2c);
    if (HAL_I2C_Init(hi2c) != HAL_OK) {
        return BSP_IIC_ERR_HAL;
    }
    return BSP_IIC_OK;
}

/*==============================================================================
 * HAL 回调覆盖
 *
 * 一次传输的完成点有两个来源：I2Cx 的 EV 中断（ADDR 阶段、BTF、STOPF 等
 * 事件推进状态机）与 DMA 流的中断（字节搬完）。HAL 把两者收在同一组
 * 完成回调里，所以下面四个回调既可能从外设中断进，也可能从 DMA 中断进。
 * 错误回调的来源是 I2Cx 的 ER 中断，或 DMA 流的错误中断。
 * 这里覆盖的是 HAL 的 __weak 回调，不是 IRQ 入口本身。
 *============================================================================*/
/**
 * @brief 主机发送完成回调
 * @param hi2c HAL IIC 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；RAW 发送路径的完成点。
 */
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    const bsp_iic_t *bus = BSP_IIC_FindBus(hi2c);

    if (bus != NULL) {
        BSP_IIC_CompleteFromISR(bus, BSP_IIC_OK);
    }
}

/**
 * @brief 主机接收完成回调
 * @param hi2c HAL IIC 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；RAW 接收路径的完成点。
 */
void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    const bsp_iic_t *bus = BSP_IIC_FindBus(hi2c);

    if (bus != NULL) {
        BSP_IIC_CompleteFromISR(bus, BSP_IIC_OK);
    }
}

/**
 * @brief 寄存器写完成回调
 * @param hi2c HAL IIC 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；MEM 写路径的完成点。RAW 与 MEM
 *       两条路径的回调不能互相替代，HAL 按传输类型分别调用。
 */
void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    const bsp_iic_t *bus = BSP_IIC_FindBus(hi2c);

    if (bus != NULL) {
        BSP_IIC_CompleteFromISR(bus, BSP_IIC_OK);
    }
}

/**
 * @brief 寄存器读完成回调
 * @param hi2c HAL IIC 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；MEM 读路径的完成点。
 */
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    const bsp_iic_t *bus = BSP_IIC_FindBus(hi2c);

    if (bus != NULL) {
        BSP_IIC_CompleteFromISR(bus, BSP_IIC_OK);
    }
}

/**
 * @brief 错误回调：把 HAL ErrorCode 翻译成 BSP 返回码
 * @param hi2c HAL IIC 句柄
 * @note AF -> NACK；BERR/ARLO/OVR -> BUS；TIMEOUT -> TIMEOUT；其余 HAL。
 *       只有登记着回调时才收结果：Abort 之后迟到的错误不会惊动调用者。
 */
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    const bsp_iic_t *bus = BSP_IIC_FindBus(hi2c);
    bsp_iic_status_t result;

    if (bus == NULL) {
        return;
    }

    if ((hi2c->ErrorCode & HAL_I2C_ERROR_AF) != 0U) {
        result = BSP_IIC_ERR_NACK;
    } else if ((hi2c->ErrorCode &
                (HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_ARLO |
                 HAL_I2C_ERROR_OVR)) != 0U) {
        result = BSP_IIC_ERR_BUS;
    } else if ((hi2c->ErrorCode & HAL_I2C_ERROR_TIMEOUT) != 0U) {
        result = BSP_IIC_ERR_TIMEOUT;
    } else {
        result = BSP_IIC_ERR_HAL;
    }

    BSP_IIC_CompleteFromISR(bus, result);
}
