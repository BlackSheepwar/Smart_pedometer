/**
 * @file dev_iic.c
 * @brief IIC 虚拟外设：总线独占、速率绑定、等待与超时、错误码翻译
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 设计模型：
 *       1. 每路总线一个总线对象（bsp 句柄 + 互斥量 + 运行时块），总线对象
 *          不导出，上层只看得到设备句柄；
 *       2. 一次读写 = 抢总线互斥量 → 按设备句柄换总线时钟 → 启动一笔 DMA
 *          → 睡在任务通知上等完成 → 超时就中止并复位；
 *       3. bsp_iic 的完成回调在中断里只做三件事：写结果、置完成标志、
 *          唤醒还在这笔传输上等着的任务；
 *       4. 超时后置放弃标志再中止，迟到的完成中断不会再唤醒任何人。
 *
 *       等待、超时、片内地址宽度这些"多器件共用一条总线"的规则全在本层；
 *       bsp_iic 只负责把一笔搬运交给外设并在中断里回调。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "dev_iic.h"
// 功能包含
#include "bsp_iic.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

/*==============================================================================
 * 外部变量声明
 *============================================================================*/
/* ---- CubeMX 生成的互斥量：名字与 .ioc 里 FREERTOS.Mutexes01 一致 ---- */
extern osMutexId_t IIC_LOCKHandle;

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单笔传输默认超时（ms），等总线与搬运共用这一份预算 */
#define DEV_IIC_DEFAULT_TIMEOUT_MS  50U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 总线运行时块
 * @note 记着谁在等、结果是什么、这一笔是否已被放弃。
 */
typedef struct {
    TaskHandle_t volatile     owner;     /* 正在等这笔完成的任务 */
    volatile dev_iic_status_t result;    /* 本笔结果，由中断写入 */
    volatile uint8_t          done;      /* 1: 本笔已完成（中断置位） */
    volatile uint8_t          abandoned; /* 1: 调用者已放弃，不再唤醒 */
    volatile uint8_t          initialized; /* 1: 已完成初始化 */
} dev_iic_rt_t;

/**
 * @brief IIC 总线对象
 */
typedef struct {
    const bsp_iic_t *bsp;  /* 芯片侧总线句柄 */
    osMutexId_t     *lock; /* 指向 CubeMX 生成的互斥量句柄 */
    dev_iic_rt_t    *rt;   /* 运行期状态 */
} dev_iic_t;

/**
 * @brief 从机设备句柄
 */
struct dev_iic_dev {
    const dev_iic_t *bus;          /* 挂在哪条总线上 */
    uint16_t         addr;         /* 7 bit 从机地址，未左移 */
    uint32_t         speed_hz;     /* 这颗器件用的总线时钟（Hz） */
    uint8_t          mem_addr_size; /* 寄存器子地址宽度，1 或 2 字节 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief I2C1 的运行时块 */
static dev_iic_rt_t dev_iic1_rt;

/** @brief I2C1 总线对象；不导出，只经设备句柄间接使用 */
const dev_iic_t dev_iic1 = {
    .bsp  = &bsp_iic1,
    .lock = &IIC_LOCKHandle,
    .rt   = &dev_iic1_rt,
};

/** @brief 总线表：初始化按它轮询
 *  @note 表长由行数算出，不需要终止符。 */
static const dev_iic_t *const s_buses[] = {
    &dev_iic1,
};

/** @brief 总线表长度：由表的行数算出 */
static const size_t s_bus_count = sizeof(s_buses) / sizeof(s_buses[0]);

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把 BSP 返回码翻译成本层返回码
 * @param status BSP 返回码
 * @retval 对应的 dev_iic_status_t；未知值一律按 HAL 错误处理
 */
static dev_iic_status_t Dev_IIC_Map(bsp_iic_status_t status)
{
    switch (status) {
    case BSP_IIC_OK:          return DEV_IIC_OK;
    case BSP_IIC_ERR_PARAM:   return DEV_IIC_ERR_PARAM;
    case BSP_IIC_ERR_STATE:   return DEV_IIC_ERR_STATE;
    case BSP_IIC_ERR_TIMEOUT: return DEV_IIC_ERR_TIMEOUT;
    case BSP_IIC_ERR_NACK:    return DEV_IIC_ERR_NACK;
    case BSP_IIC_ERR_BUS:     return DEV_IIC_ERR_BUS;
    case BSP_IIC_ERR_HAL:     return DEV_IIC_ERR_HAL;
    default:                  return DEV_IIC_ERR_HAL;
    }
}

/**
 * @brief 搬运完成回调（中断上下文）
 * @param ctx    总线运行时块
 * @param status 本次结果
 * @retval 无
 * @note 先写结果与完成标志、再读 owner 与放弃标志：任务被唤醒后看到的
 *       一定是已经落盘的结果。已放弃的一笔只留结果，不再唤醒任何人。
 */
static void Dev_IIC_OnDone(void *ctx, bsp_iic_status_t status)
{
    dev_iic_rt_t *rt = (dev_iic_rt_t *)ctx;
    TaskHandle_t owner;
    BaseType_t woken = pdFALSE;

    rt->result = Dev_IIC_Map(status);
    rt->done   = 1U;

    owner = rt->owner;
    if (owner != NULL && rt->abandoned == 0U) {
        vTaskNotifyGiveFromISR(owner, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/**
 * @brief 执行一笔传输：抢总线 → 按本设备登记的速度改总线时钟 → 启动 → 等完成
 * @param dev        设备句柄
 * @param msg        传输命令
 * @param timeout_ms 总预算（ms），0 表示用默认值
 * @retval DEV_IIC_OK: 成功
 * @retval 其他: 失败原因
 * @note 进入前已校验句柄；本函数负责加锁与解锁，任何出口都不漏锁。
 *       换速改的是整条总线的 SCL，不是这颗器件自己的时钟：上一笔可能是
 *       另一颗器件、另一个速率，所以每次开口前都要把总线恢复到本设备的
 *       速率；目标值与当前值相同的时候 BSP 直接返回，不重写分频。
 *       阻塞方式在启动函数里已经搬完，此时没有通知可等。
 */
static dev_iic_status_t Dev_IIC_Run(const dev_iic_dev_t *dev,
                                    const bsp_iic_msg_t *msg,
                                    uint32_t timeout_ms)
{
    const dev_iic_t *bus;
    dev_iic_rt_t *rt;
    TickType_t ticks;
    uint32_t budget_ms;
    bsp_iic_status_t st;
    dev_iic_status_t out;

    if (dev == NULL || dev->bus == NULL || msg == NULL) {
        return DEV_IIC_ERR_PARAM;
    }
    if (msg->data == NULL || msg->len == 0U) {
        return DEV_IIC_ERR_PARAM;
    }

    bus = dev->bus;
    rt  = bus->rt;
    if (rt->initialized == 0U) {
        return DEV_IIC_ERR_STATE;
    }
    /* 调度器没跑起来时任务通知与互斥量都不可用，先挡住。 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return DEV_IIC_ERR_STATE;
    }

    budget_ms = (timeout_ms != 0U) ? timeout_ms : DEV_IIC_DEFAULT_TIMEOUT_MS;
    ticks = pdMS_TO_TICKS(budget_ms);

    if (osMutexAcquire(*bus->lock, ticks) != osOK) {
        return DEV_IIC_ERR_TIMEOUT;
    }

    st = BSP_IIC_SetSpeed(bus->bsp, dev->speed_hz);
    if (st != BSP_IIC_OK) {
        (void)osMutexRelease(*bus->lock);
        return Dev_IIC_Map(st);
    }

    rt->result    = DEV_IIC_ERR_HAL;
    rt->done      = 0U;
    rt->abandoned = 0U;
    rt->owner     = xTaskGetCurrentTaskHandle();

    (void)ulTaskNotifyTake(pdTRUE, 0U); /* 清掉上一笔残留的令牌 */

    st = BSP_IIC_StartTransfer(bus->bsp, msg, budget_ms, Dev_IIC_OnDone, rt);
    if (st != BSP_IIC_OK) {
        rt->owner = NULL;
        (void)osMutexRelease(*bus->lock);
        return Dev_IIC_Map(st);
    }

    /* 阻塞方式在启动函数里已经搬完、没有回调可等；异步方式才睡等通知。 */
    if (BSP_IIC_IsBusy(bus->bsp) == 0U) {
        out = (rt->done != 0U) ? rt->result : DEV_IIC_OK;
    } else if (ulTaskNotifyTake(pdTRUE, ticks) == 0U) {
        rt->abandoned = 1U;
        (void)BSP_IIC_Abort(bus->bsp);
        out = DEV_IIC_ERR_TIMEOUT;
    } else {
        out = rt->result;
    }

    rt->owner = NULL;
    (void)osMutexRelease(*bus->lock);
    return out;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化 IIC 虚拟外设：逐路检查总线、互斥量并探测 BSP 就绪
 * @retval DEV_IIC_OK: 全部总线就绪
 * @retval DEV_IIC_ERR_STATE: 缺总线句柄、互斥量尚未创建，或 bsp_iic 未初始化
 * @note 探测走 BSP_IIC_SetSpeed(bus, 0)：目标值 0 表示回落到初始化登记的
 *       时钟，未初始化时直接返回 ERR_STATE，所以这个调用是零副作用的
 *       就绪探针，不需要去读 BSP 句柄的内部字段。
 */
dev_iic_status_t Dev_IIC_Init(void)
{
    size_t i;

    for (i = 0U; i < s_bus_count; i++) {
        const dev_iic_t *bus = s_buses[i];
        bsp_iic_status_t st;
        dev_iic_rt_t *rt;

        rt = bus->rt;
        if (rt->initialized != 0U) {
            continue;
        }
        if (bus->bsp == NULL || bus->lock == NULL || *bus->lock == NULL) {
            return DEV_IIC_ERR_STATE;
        }

        st = BSP_IIC_SetSpeed(bus->bsp, 0U);
        if (st != BSP_IIC_OK) {
            return Dev_IIC_Map(st);
        }

        rt->owner       = NULL;
        rt->result      = DEV_IIC_ERR_HAL;
        rt->done        = 0U;
        rt->abandoned   = 0U;
        rt->initialized = 1U;
    }
    return DEV_IIC_OK;
}

/**
 * @brief 向从机直接写数据（RAW 模式）
 * @param dev        设备句柄，不能为 NULL
 * @param data       发送缓冲区，不能为 NULL
 * @param len        发送字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已发送完成
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 */
dev_iic_status_t Dev_IIC_Write(const dev_iic_dev_t *dev,
                               const uint8_t *data, uint16_t len,
                               uint32_t timeout_ms)
{
    bsp_iic_msg_t msg;

    if (dev == NULL) {
        return DEV_IIC_ERR_PARAM;
    }

    msg.dev_addr      = dev->addr;
    msg.mem_addr      = 0U;
    msg.mem_addr_size = 1U;
    msg.len           = len;
    msg.dir           = (uint8_t)BSP_IIC_DIR_TX;
    msg.mode          = (uint8_t)BSP_IIC_MODE_RAW;
    msg.data          = (uint8_t *)data;

    return Dev_IIC_Run(dev, &msg, timeout_ms);
}

/**
 * @brief 从从机直接读数据（RAW 模式）
 * @param dev        设备句柄，不能为 NULL
 * @param data       接收缓冲区，不能为 NULL
 * @param len        接收字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已读满 len 字节
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 */
dev_iic_status_t Dev_IIC_Read(const dev_iic_dev_t *dev,
                              uint8_t *data, uint16_t len,
                              uint32_t timeout_ms)
{
    bsp_iic_msg_t msg;

    if (dev == NULL) {
        return DEV_IIC_ERR_PARAM;
    }

    msg.dev_addr      = dev->addr;
    msg.mem_addr      = 0U;
    msg.mem_addr_size = 1U;
    msg.len           = len;
    msg.dir           = (uint8_t)BSP_IIC_DIR_RX;
    msg.mode          = (uint8_t)BSP_IIC_MODE_RAW;
    msg.data          = data;

    return Dev_IIC_Run(dev, &msg, timeout_ms);
}

/**
 * @brief 向寄存器子地址写数据（MEM 模式）
 * @param dev        设备句柄，不能为 NULL；子地址宽度取自设备句柄
 * @param reg        寄存器子地址
 * @param data       发送缓冲区，不能为 NULL
 * @param len        发送字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已写入完成
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 */
dev_iic_status_t Dev_IIC_WriteReg(const dev_iic_dev_t *dev, uint16_t reg,
                                  const uint8_t *data, uint16_t len,
                                  uint32_t timeout_ms)
{
    bsp_iic_msg_t msg;

    if (dev == NULL) {
        return DEV_IIC_ERR_PARAM;
    }

    msg.dev_addr      = dev->addr;
    msg.mem_addr      = reg;
    msg.mem_addr_size = (uint16_t)dev->mem_addr_size;
    msg.len           = len;
    msg.dir           = (uint8_t)BSP_IIC_DIR_TX;
    msg.mode          = (uint8_t)BSP_IIC_MODE_MEM;
    msg.data          = (uint8_t *)data;

    return Dev_IIC_Run(dev, &msg, timeout_ms);
}

/**
 * @brief 从寄存器子地址读数据（MEM 模式）
 * @param dev        设备句柄，不能为 NULL；子地址宽度取自设备句柄
 * @param reg        寄存器子地址
 * @param data       接收缓冲区，不能为 NULL
 * @param len        接收字节数，不能为 0
 * @param timeout_ms 等待与搬运的总预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已读满 len 字节
 * @retval DEV_IIC_ERR_PARAM: 句柄、缓冲区或长度非法
 * @retval DEV_IIC_ERR_STATE: 未初始化
 * @retval DEV_IIC_ERR_TIMEOUT: 等总线超时，或搬运超时（已中止并复位）
 * @retval DEV_IIC_ERR_NACK: 从机 NACK
 * @retval DEV_IIC_ERR_BUS: 总线错误
 * @retval DEV_IIC_ERR_HAL: HAL 报错
 */
dev_iic_status_t Dev_IIC_ReadReg(const dev_iic_dev_t *dev, uint16_t reg,
                                 uint8_t *data, uint16_t len,
                                 uint32_t timeout_ms)
{
    bsp_iic_msg_t msg;

    if (dev == NULL) {
        return DEV_IIC_ERR_PARAM;
    }

    msg.dev_addr      = dev->addr;
    msg.mem_addr      = reg;
    msg.mem_addr_size = (uint16_t)dev->mem_addr_size;
    msg.len           = len;
    msg.dir           = (uint8_t)BSP_IIC_DIR_RX;
    msg.mode          = (uint8_t)BSP_IIC_MODE_MEM;
    msg.data          = data;

    return Dev_IIC_Run(dev, &msg, timeout_ms);
}
