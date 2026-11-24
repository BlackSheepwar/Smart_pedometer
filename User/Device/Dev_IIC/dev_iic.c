/**
 * @file dev_iic.c
 * @brief IIC 虚拟外设：总线独占、速率绑定、等待与超时、错误码翻译
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 设计模型：
 *       1. 每路总线一个总线对象（bsp 句柄 + 互斥量 + 运行块），总线对象
 *          不导出，上层只看得到设备句柄；
 *       2. 单笔读写 = 抢总线互斥量 → 按设备句柄换总线时钟 → 启动一笔 DMA
 *          → 睡在任务通知上等完成 → 超时就中止并复位；
 *       3. 一次会话 = Select() 一次 → 任意多笔读写 → Deselect() 一次：
 *          需要连续跟同一颗器件交换多组信息时，锁与换速只做一遍；
 *       4. bsp_iic 的完成回调在中断里只做三件事：写结果、置完成标志、
 *          唤醒还在这笔传输上等着的任务；
 *       5. 超时后置放弃标志再中止，迟到的完成中断不会再唤醒任何人。
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
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "bsp_iic.h"

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
 * @brief 总线运行块
 * @note owner 同时表示两件事：谁持有总线（会话期间），以及这一笔完成时该
 *       唤醒谁。frame_dev 让"会话里换了设备"变成可查的错误码，而不是悄悄
 *       沿用上一台设备的时钟。
 */
typedef struct {
    TaskHandle_t volatile     owner;     /* 持有总线并等待本次完成的任务 */
    const dev_iic_dev_t      *frame_dev; /* 当前会话对应的从机，空闲为 NULL */
    bsp_iic_wait_t            wait;      /* 交给芯片层的接收方：任务 + 设备 */
    volatile dev_iic_status_t result;    /* 本笔结果，由中断写入 */
    volatile uint8_t          done;      /* 1: 本笔已完成（中断置位） */
    volatile uint8_t          abandoned; /* 1: 调用者已放弃，不再唤醒 */
    volatile uint8_t          initialized; /* 1: 已完成初始化 */
} dev_iic_rt_t;

/**
 * @brief IIC 总线对象
 * @note 布局只在本文件，头文件拿到的只有不透明标签。
 */
struct dev_iic {
    const bsp_iic_t *bsp;  /* 芯片侧总线句柄 */
    osMutexId_t     *lock; /* 指向 CubeMX 生成的互斥量句柄 */
    dev_iic_rt_t    *rt;   /* 运行块 */
};

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
/** @brief I2C1 的运行块 */
static dev_iic_rt_t dev_iic1_rt;

/** @brief I2C1 总线对象；Init 一次登记一条 */
const dev_iic_t dev_iic1 = {
    .bsp  = &bsp_iic1,
    .lock = &IIC_LOCKHandle,
    .rt   = &dev_iic1_rt,
};

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
 * @param ctx    本次传输的从机设备句柄
 * @param status 本次结果
 * @retval 无
 * @note 先写结果与完成标志、再读 owner 与放弃标志：任务被唤醒后看到的
 *       一定是已经落盘的结果。已放弃的一笔只留结果，不再唤醒任何人。
 *       结果与 owner 都在总线运行块上：一次持锁只有一台从机在传，从设备
 *       句柄一步就能定位到那条总线。
 */
static void Dev_IIC_OnDone(const bsp_iic_wait_t *wait, bsp_iic_status_t status)
{
    const dev_iic_dev_t *dev = (const dev_iic_dev_t *)wait->ctx;
    dev_iic_rt_t *rt = dev->bus->rt;
    TaskHandle_t owner;
    BaseType_t woken = pdFALSE;

    rt->result = Dev_IIC_Map(status);
    rt->done   = 1U;

    owner = (TaskHandle_t)wait->task;
    if (owner != NULL && rt->abandoned == 0U) {
        vTaskNotifyGiveFromISR(owner, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/**
 * @brief 在当前已持有的总线上搬一笔：启动、等完成、超时中止
 * @param dev       设备句柄，帧必须已由本任务、为这台设备开好
 * @param msg       传输命令
 * @param budget_ms 本笔的时间预算（ms），已按默认值折算
 * @retval DEV_IIC_OK: 成功
 * @retval DEV_IIC_ERR_TIMEOUT: 搬运超时（已中止并复位）
 * @retval 其他: 芯片层返回码翻译后的失败原因
 *
 * @note 不加锁、不换速、不判初始化：那些是帧开始时的职责。先清残留令牌
 *       再启动，避免上一笔迟到的通知把这一笔误判成已完成。阻塞档在启动
 *       函数里已经搬完，此时没有通知可等。
 */
static dev_iic_status_t Dev_IIC_Push(const dev_iic_dev_t *dev,
                                     const bsp_iic_msg_t *msg,
                                     uint32_t budget_ms)
{
    dev_iic_rt_t *rt = dev->bus->rt;
    bsp_iic_status_t st;
    TickType_t ticks = pdMS_TO_TICKS(budget_ms);

    rt->result    = DEV_IIC_ERR_HAL;
    rt->done      = 0U;
    rt->abandoned = 0U;
    (void)ulTaskNotifyTake(pdTRUE, 0U); /* 清掉上一笔残留的令牌 */

    /* 接收方打包成一份描述交给芯片层：任务用于唤醒，上下文用于定位实例 */
    rt->wait.task = xTaskGetCurrentTaskHandle();
    rt->wait.ctx  = (void *)dev;

    st = BSP_IIC_StartTransfer(dev->bus->bsp, msg, budget_ms, Dev_IIC_OnDone,
                               &rt->wait);
    if (st != BSP_IIC_OK) {
        return Dev_IIC_Map(st);
    }

    /* 阻塞方式在启动函数里已经搬完、没有回调可等；异步方式才睡等通知。 */
    if (BSP_IIC_IsBusy(dev->bus->bsp) == 0U) {
        return (rt->done != 0U) ? rt->result : DEV_IIC_OK;
    }

    if (ulTaskNotifyTake(pdTRUE, ticks) == 0U) {
        rt->abandoned = 1U;
        (void)BSP_IIC_Abort(dev->bus->bsp);
        return DEV_IIC_ERR_TIMEOUT;
    }

    return rt->result;
}

/**
 * @brief 执行一笔传输：会话内直接搬，会话外自带开关
 * @param dev        设备句柄
 * @param msg        传输命令
 * @param timeout_ms 等锁与本笔搬运各自的预算（ms），0 表示用默认值
 * @retval DEV_IIC_OK: 成功
 * @retval 其他: 失败原因
 *
 * @note 进入前已校验句柄：四个包装函数都先读过 dev->addr，所以这里不再重复
 *       判 dev；msg 是包装函数栈上的结构体，取其地址也不可能为空。
 *       两种走法：调用者已用 Dev_IIC_Select() 开了会话，就直接 Push，不再
 *       动锁与速率；否则自带 Select → Push → Deselect，任何出口都不漏锁。
 *       会话里换成别的设备属于误用：那次会话的速率是按前一台设的，这里
 *       直接拒绝，不悄悄改总线时钟。
 */
static dev_iic_status_t Dev_IIC_Run(const dev_iic_dev_t *dev,
                                    const bsp_iic_msg_t *msg,
                                    uint32_t timeout_ms)
{
    dev_iic_rt_t *rt;
    dev_iic_status_t st;
    uint32_t budget_ms;

    if (dev->bus == NULL || msg->data == NULL || msg->len == 0U) {
        return DEV_IIC_ERR_PARAM;
    }

    rt = dev->bus->rt;
    if (rt->initialized == 0U) {
        return DEV_IIC_ERR_STATE;
    }
    /* 调度器没跑起来时任务通知与互斥量都不可用，先挡住。 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return DEV_IIC_ERR_STATE;
    }

    budget_ms = (timeout_ms != 0U) ? timeout_ms : DEV_IIC_DEFAULT_TIMEOUT_MS;

    /* 会话内：锁与速率已由 Select 备好，直接搬 */
    if (rt->owner == xTaskGetCurrentTaskHandle()) {
        return (rt->frame_dev == dev) ? Dev_IIC_Push(dev, msg, budget_ms)
                                      : DEV_IIC_ERR_STATE;
    }

    st = Dev_IIC_Select(dev, timeout_ms);
    if (st != DEV_IIC_OK) {
        return st;
    }

    st = Dev_IIC_Push(dev, msg, budget_ms);
    Dev_IIC_Deselect(dev);

    return st;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化一路 IIC 总线：检查总线、互斥量并探测 BSP 就绪
 * @param bus 总线句柄
 * @retval DEV_IIC_OK: 总线就绪；已初始化的总线直接返回
 * @retval DEV_IIC_ERR_STATE: 缺总线句柄、互斥量尚未创建，或 bsp_iic 未初始化
 *
 * @note 探测走 BSP_IIC_SetSpeed(bus, 0)：目标值 0 表示回落到初始化登记的
 *       时钟，未初始化时直接返回 ERR_STATE，所以这个调用是零副作用的
 *       就绪探针，不需要去读 BSP 句柄的内部字段。每条总线调一次。
 */
dev_iic_status_t Dev_IIC_Init(const dev_iic_t *bus)
{
    bsp_iic_status_t st;
    dev_iic_rt_t *rt = bus->rt;

    if (rt->initialized != 0U) {
        return DEV_IIC_OK;
    }
    if (bus->bsp == NULL || bus->lock == NULL || *bus->lock == NULL) {
        return DEV_IIC_ERR_STATE;
    }

    st = BSP_IIC_SetSpeed(bus->bsp, 0U);
    if (st != BSP_IIC_OK) {
        return Dev_IIC_Map(st);
    }

    rt->frame_dev   = NULL;
    rt->owner       = NULL;
    rt->result      = DEV_IIC_ERR_HAL;
    rt->done        = 0U;
    rt->abandoned   = 0U;
    rt->initialized = 1U;

    return DEV_IIC_OK;
}

/**
 * @brief 取得总线所有权并切换到本设备的总线时钟（会话开始）
 * @param dev        设备句柄，不能为 NULL
 * @param timeout_ms 排队等锁的预算（ms），0 表示用默认 50 ms
 * @retval DEV_IIC_OK: 已持有总线，总线时钟已切到本设备
 * @retval DEV_IIC_ERR_PARAM: 句柄或总线为空
 * @retval DEV_IIC_ERR_STATE: 未初始化、调度器未运行，或本任务已在会话中
 * @retval DEV_IIC_ERR_TIMEOUT: 等锁超时
 * @retval 其他: 换速失败
 *
 * @note 换速改的是整条总线的 SCL，不是这颗器件自己的时钟：上一笔可能是
 *       另一颗器件、另一个速率，所以每次开口前都要把总线恢复到本设备的
 *       速率。换速点在拿到锁之后、任何数据传输之前，此时总线空闲。
 *       与 Deselect() 成对；一次会话里可以夹任意多笔读写。
 */
dev_iic_status_t Dev_IIC_Select(const dev_iic_dev_t *dev, uint32_t timeout_ms)
{
    const dev_iic_t *bus;
    dev_iic_rt_t *rt;
    bsp_iic_status_t st;
    TaskHandle_t self;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return DEV_IIC_ERR_PARAM;
    }
    bus = dev->bus;
    rt  = bus->rt;

    if (rt->initialized == 0U) {
        return DEV_IIC_ERR_STATE;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return DEV_IIC_ERR_STATE;
    }

    self = xTaskGetCurrentTaskHandle();
    /* 同一任务重复开帧是调用错误；别的任务持帧则照常排队等锁 */
    if (rt->owner == self) {
        return DEV_IIC_ERR_STATE;
    }
    if (timeout_ms == 0U) {
        timeout_ms = DEV_IIC_DEFAULT_TIMEOUT_MS;
    }
    if (osMutexAcquire(*bus->lock, pdMS_TO_TICKS(timeout_ms)) != osOK) {
        return DEV_IIC_ERR_TIMEOUT;
    }

    st = BSP_IIC_SetSpeed(bus->bsp, dev->speed_hz);
    if (st != BSP_IIC_OK) {
        (void)osMutexRelease(*bus->lock);
        return Dev_IIC_Map(st);
    }

    /* 锁到手之后才登记：两步之间不会有别的任务插进来 */
    rt->frame_dev = dev;
    rt->owner     = self;

    return DEV_IIC_OK;
}

/**
 * @brief 归还总线（会话结束）
 * @param dev 设备句柄，不能为 NULL
 * @retval 无
 *
 * @note 与 Select() 配对。只对"本任务、本设备"开的会话生效：任务不是持有者、
 *       或 dev 不是本次会话选中的那台，都直接返回，避免放掉别人的锁。
 */
void Dev_IIC_Deselect(const dev_iic_dev_t *dev)
{
    const dev_iic_t *bus;
    dev_iic_rt_t *rt;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return;
    }
    bus = dev->bus;
    rt  = bus->rt;

    if ((rt->owner != xTaskGetCurrentTaskHandle()) || (rt->frame_dev != dev)) {
        return;
    }

    rt->frame_dev = NULL;
    rt->owner     = NULL;
    (void)osMutexRelease(*bus->lock);
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
