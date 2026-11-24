/**
 * @file dev_spi.c
 * @brief SPI 虚拟设备层：互斥量排队、片选时序、按设备换速、帧所有权
 * @author Ricardo
 * @date 2026-11-20
 *
 * @note 设计模型：
 *       1. 一条总线一把互斥量。先抢锁，抢不到就在锁上排队；锁到手之后
 *          按设备切速率、拉片选、登记帧主，整个帧期间不放开——
 *          同时保证了另外两件事：别的设备插不进来，别的任务也抢不到锁。
 *       2. 锁与片选必须同生命周期。帧内再抢锁会在同一任务上二次 acquire
 *          非递归互斥量，等满 timeout 后 ERR_TIMEOUT、整帧作废；
 *          frame_owner + frame_dev 把这类误用变成 ERR_STATE。
 *       3. 本层只认"设备要什么速率"，怎么求出分频是 BSP 的事；错误码
 *          在这里翻译一次，上层只认 DEV_SPI_*。
 *       4. 运行块（帧主、帧设备、就绪标志）放在内部 rt 对象里，
 *          总线句柄本身 const，与框架的配置型句柄约定一致。
 *
 *       OS 对象归属：总线互斥量由 .ioc 配置、freertos.c 生成并创建，本文件
 *       只从 osal.h 取它的地址（extern 收在 User/OSAL）。
 *       Dev_SPI_Init() 必须放在 USER CODE BEGIN RTOS_MUTEX，那里句柄才建好。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stddef.h>
#include <stdint.h>
#include "dev_spi.h"
// 功能包含
#include "osal.h"
#include "bsp_spi.h"
#include "bsp_gpio.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/** @brief 单次调用默认超时（ms），排队与传输各自受限 */
#define DEV_SPI_DEFAULT_TIMEOUT_MS  50U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 总线的运行块（内部类型，不进公共头文件）
 * @note 帧主与帧设备成对存在：两者都为 NULL 表示总线空闲，只有本任务 +
 *       本设备同时匹配才算"这一帧是我开的"。
 */
typedef struct {
    TaskHandle_t volatile frame_owner; /* 当前持有帧事务的任务，空闲为 NULL */
    const dev_spi_dev_t  *frame_dev;   /* 当前帧对应的设备，空闲为 NULL */
    bsp_spi_wait_t        wait;        /* 交给芯片层的接收方：任务 + 设备 */
    volatile dev_spi_status_t result;  /* 最近一次传输结果，由中断写入 */
    volatile uint8_t      done;        /* 1: 中断已把 result 写好 */
    volatile uint8_t      abandoned;   /* 1: 本次等待已放弃（超时） */
    volatile uint8_t      initialized; /* 1: 已完成初始化 */
} dev_spi_rt_t;

/**
 * @brief SPI 总线对象
 * @note 指向 BSP 的总线句柄与 freertos.c 生成的互斥量；两者都在 .rodata，
 *       可变状态在 rt 里。布局只在本文件，头文件拿到的只有不透明标签。
 */
struct dev_spi {
    const bsp_spi_t *bsp;  /* BSP 总线：速率与字节搬运由它完成 */
    osMutexId_t     *lock; /* 指向 freertos.c 生成的互斥量句柄 */
    dev_spi_rt_t    *rt;   /* 运行块，定义在本文件内 */
};

/**
 * @brief SPI 设备对象
 * @note 只存总线、片选、极性、速率；不存 SCK/MOSI。const 进 .rodata。
 *
 *       sck_hz 是该器件能接受的 SCK 上限，同一条总线上各设备各写各的；
 *       本层在片选拉低之前换挡，调用方完全无感。0 表示回到 CubeMX 里配的
 *       预分频，不必给每个设备都补这个字段。
 */
struct dev_spi_dev {
    const dev_spi_t  *bus;             /* 所属总线，不能为 NULL */
    const bsp_gpio_t *cs;              /* 片选引脚句柄，NULL 表示不用片选 */
    uint8_t           cs_active_level; /* 1: 高电平有效；0: 低电平有效 */
    uint32_t          sck_hz;          /* SCK 上限（Hz），0 = 用默认配置 */
};

/*==============================================================================
 * 内部变量
 *============================================================================*/
/** @brief SPI1 的运行块，仅本文件可见 */
static dev_spi_rt_t dev_spi1_rt = {
    .frame_owner = NULL,
    .frame_dev   = NULL,
    .initialized = 0U,
};

/** @brief SPI1 总线句柄：配置只有"用哪条 BSP 总线、哪把锁" */
const dev_spi_t dev_spi1 = {
    .bsp  = &bsp_spi1,
    .lock = &SPI1_LOCKHandle,
    .rt   = &dev_spi1_rt,
};

/**
 * @brief 显示设备对象：SPI1 + 低有效片选
 * @note sck_hz 填器件手册给出的 SCK 上限，不是"板子能跑多快"的经验值：
 *       超速的典型现象是初始化失败、全黑或满屏花点，从代码上完全看不出原因。
 */
const dev_spi_dev_t dev_spi_dev_lcd = {
    .bus             = &dev_spi1,
    .cs              = &bsp_gpio_pa8,
    .cs_active_level = 0U,
    .sck_hz          = 21000000U,
};

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把 BSP 返回码翻译成本层返回码
 * @param status BSP 层的返回码
 * @retval 同义的 DEV_SPI 返回码
 * @note 一一对应，不降级、不合并：上层需要区分"总线错误"与"HAL 错误"，
 *       合并会让现场排查丢掉"是器件收错还是外设没起来"这个分界。
 */
static dev_spi_status_t Dev_SPI_Map(bsp_spi_status_t status)
{
    switch (status) {
    case BSP_SPI_OK:
        return DEV_SPI_OK;
    case BSP_SPI_ERR_PARAM:
        return DEV_SPI_ERR_PARAM;
    case BSP_SPI_ERR_STATE:
        return DEV_SPI_ERR_STATE;
    case BSP_SPI_ERR_TIMEOUT:
        return DEV_SPI_ERR_TIMEOUT;
    case BSP_SPI_ERR_MODE:
        return DEV_SPI_ERR_MODE;
    case BSP_SPI_ERR_BUS:
        return DEV_SPI_ERR_BUS;
    default:
        return DEV_SPI_ERR_HAL;
    }
}

/**
 * @brief BSP 的完成回调：落盘结果并唤醒开着帧的任务
 * @param ctx    本次传输的设备句柄
 * @param status BSP 层的结果码
 * @retval 无
 * @note 在中断上下文执行，只做两件 O(1) 的事：写结果、唤醒等待者，
 *       唤醒必须用 FromISR 版本。结果与放弃标志在总线运行块上：一次持锁
 *       只有一台设备在传，所以从设备句柄一步就能定位到那条总线。
 *       frame_owner 为 NULL 说明这一帧已经 Deselect，没人等这次结果；
 *       abandoned 为 1 说明等待方已超时放弃，此时只落盘不唤醒，避免把
 *       令牌留给下一次传输。
 */
static void Dev_SPI_OnDone(const bsp_spi_wait_t *wait, bsp_spi_status_t status)
{
    const dev_spi_dev_t *dev = (const dev_spi_dev_t *)wait->ctx;
    dev_spi_rt_t *rt    = dev->bus->rt;
    TaskHandle_t  owner = (TaskHandle_t)wait->task;
    BaseType_t    woken = pdFALSE;

    rt->result = Dev_SPI_Map(status);
    rt->done   = 1U;

    if ((owner != NULL) && (rt->abandoned == 0U)) {
        vTaskNotifyGiveFromISR(owner, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/**
 * @brief 在当前已持有的帧上搬一段字节：启动、等完成、超时中止
 * @param dev       设备句柄，帧必须已由本任务、为这台设备开好
 * @param tx        发送缓冲区，NULL 表示本次不发
 * @param rx        接收缓冲区，NULL 表示本次不收
 * @param len       传输字节数
 * @param budget_ms 本次搬运的时间预算（ms），已按默认值折算
 * @retval DEV_SPI_OK: 传输成功
 * @retval DEV_SPI_ERR_TIMEOUT: 传输超时（已中止本次传输）
 * @retval 其他: 芯片层返回码翻译后的失败原因
 *
 * @note 不抢锁、不换速、不碰片选：那些是帧开始时的职责。清残留令牌必须
 *       在启动之前——上一笔超时放弃后，迟到的那次唤醒会把令牌留在任务的
 *       通知值里，不清就会把本次等待误判成"已完成"。
 */
static dev_spi_status_t Dev_SPI_Push(const dev_spi_dev_t *dev,
                                     const uint8_t *tx, uint8_t *rx,
                                     uint16_t len, uint32_t budget_ms)
{
    const dev_spi_t *bus = dev->bus;
    dev_spi_rt_t    *rt  = bus->rt;
    bsp_spi_status_t bsp;

    rt->done      = 0U;
    rt->abandoned = 0U;
    rt->result    = DEV_SPI_ERR_STATE;
    (void)ulTaskNotifyTake(pdTRUE, 0U);

    /* 接收方打包成一份描述交给芯片层：任务用于唤醒，上下文用于定位实例 */
    rt->wait.task = xTaskGetCurrentTaskHandle();
    rt->wait.ctx  = (void *)dev;

    bsp = BSP_SPI_StartTransfer(bus->bsp, tx, rx, len, budget_ms,
                                Dev_SPI_OnDone, &rt->wait);
    if (bsp != BSP_SPI_OK) {
        return Dev_SPI_Map(bsp);
    }

    /* 没有 DMA 时 BSP 已在启动里搬完；DMA 也可能在查询之前就收尾了。
       这两种情况都不必再等，直接取结果。 */
    if (BSP_SPI_IsBusy(bus->bsp) == 0U) {
        return (rt->done != 0U) ? rt->result : DEV_SPI_OK;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(budget_ms)) == 0U) {
        /* 醒了但令牌是超时给的：撤回等待登记再中止，迟到回调不会再来打扰 */
        rt->abandoned = 1U;
        (void)BSP_SPI_Abort(bus->bsp);
        return DEV_SPI_ERR_TIMEOUT;
    }

    return rt->result;
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化一路 SPI 总线：检查互斥量与 BSP 是否就绪
 * @param bus 总线句柄
 * @retval DEV_SPI_OK: 初始化成功；已初始化的总线直接返回
 * @retval DEV_SPI_ERR_STATE: BSP 未初始化，或总线互斥量句柄仍为 NULL
 *
 * @note 在 BSP_SPI_Init() 与 CubeMX 的互斥量创建之后调用，即 USER CODE
 *       BEGIN RTOS_MUTEX 段内。每条总线调一次，多路总线各调各的；
 *       本层不再持有总线表，"登记几条"由工程决定。不能在中断里调用。
 */
dev_spi_status_t Dev_SPI_Init(const dev_spi_t *bus)
{
    dev_spi_rt_t    *rt = bus->rt;
    bsp_spi_status_t bsp;

    if (rt->initialized != 0U) {
        return DEV_SPI_OK;
    }
    if (bus->bsp == NULL || bus->lock == NULL || *bus->lock == NULL) {
        return DEV_SPI_ERR_STATE;
    }

    /* 用"回落到 CubeMX 档位"确认 BSP 已初始化：此刻还没有人改过速率，
       所以这是一次零开销的查询；没初始化会返回 BSP_SPI_ERR_STATE。
       直接读 BSP 的总线内部状态是越层，本层不该知道它有没有就绪标志 */
    bsp = BSP_SPI_SetRate(bus->bsp, 0U);
    if (bsp != BSP_SPI_OK) {
        return Dev_SPI_Map(bsp);
    }

    rt->frame_owner = NULL;
    rt->frame_dev   = NULL;
    rt->result      = DEV_SPI_ERR_STATE;
    rt->done        = 0U;
    rt->abandoned   = 0U;
    rt->initialized = 1U;

    return DEV_SPI_OK;
}

/**
 * @brief 取得总线所有权并拉有效片选（帧开始）
 * @param dev        设备句柄，不能为 NULL
 * @param timeout_ms 排队等锁的预算，0 表示用默认 50 ms
 * @retval DEV_SPI_OK: 已持有总线，片选已拉有效
 * @retval DEV_SPI_ERR_PARAM: 句柄或总线为空
 * @retval DEV_SPI_ERR_STATE: 未初始化、调度器未运行，或本任务已经在帧中
 * @retval DEV_SPI_ERR_TIMEOUT: 等锁超时，片选没有被拉低
 * @retval DEV_SPI_ERR_HAL: 速率切换失败，片选没有被拉低
 * @note 电平按 cs_active_level 换算。抢锁成功才拉片选，不存在"CS 已低、
 *       总线还在别人手里"的窗口。本函数同时完成速率切换：sck_hz 非 0 时
 *       按"不超过该值"算出分频并写入外设，片选拉低之前生效；sck_hz 为 0
 *       则回落到 CubeMX 配的预分频，因此同一总线上可以同时挂两种速率的
 *       器件而调用方无感。
 */
dev_spi_status_t Dev_SPI_Select(const dev_spi_dev_t *dev,
                                uint32_t timeout_ms)
{
    const dev_spi_t *bus;
    dev_spi_rt_t    *rt;
    dev_spi_status_t status;
    TaskHandle_t     self;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return DEV_SPI_ERR_PARAM;
    }
    bus = dev->bus;
    rt  = bus->rt;

    if (rt->initialized == 0U) {
        return DEV_SPI_ERR_STATE;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return DEV_SPI_ERR_STATE;
    }
    /* 同一任务重复开帧是调用错误；别的任务正持帧则照常排队等锁 */
    self = xTaskGetCurrentTaskHandle();
    if (rt->frame_owner == self) {
        return DEV_SPI_ERR_STATE;
    }

    if (timeout_ms == 0U) {
        timeout_ms = DEV_SPI_DEFAULT_TIMEOUT_MS;
    }
    if (osMutexAcquire(*bus->lock, pdMS_TO_TICKS(timeout_ms)) != osOK) {
        return DEV_SPI_ERR_TIMEOUT;
    }

    /* 速率切换点在片选拉低之前：此时总线空闲、片选全高，正是改 BR 的合法
       时机。失败时片选还没动、帧也没登记，把锁还回去即可，不能走 Deselect
       ——那里会校验 frame_dev，此刻它还是上一帧的残留值 */
    status = Dev_SPI_Map(BSP_SPI_SetRate(bus->bsp, dev->sck_hz));
    if (status != DEV_SPI_OK) {
        (void)osMutexRelease(*bus->lock);
        return status;
    }

    /* 锁到手之后才登记所有权、拉片选，两步之间不会有别的任务插进来 */
    rt->frame_owner = self;
    rt->frame_dev   = dev;
    if (dev->cs != NULL) {
        BSP_GPIO_Write(dev->cs, dev->cs_active_level);
    }
    return DEV_SPI_OK;
}

/**
 * @brief 放无效片选并归还总线（帧结束）
 * @param dev 设备句柄，不能为 NULL
 * @retval 无
 * @note 与 Select() 配对。只对"本任务、本设备"开的帧生效：任务不是帧主、
 *       或 dev 不是本帧选中的那台，都直接返回，避免放掉别人的锁或抬错片选。
 *       必须校验设备：否则 Select(dev1) 后误调 Deselect(dev2) 会放锁而
 *       dev1 的 CS 仍低，接下来用 dev2 的传输会被 dev1 一并收走。
 */
void Dev_SPI_Deselect(const dev_spi_dev_t *dev)
{
    const dev_spi_t *bus;
    dev_spi_rt_t    *rt;
    TaskHandle_t     owner;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return;
    }
    bus = dev->bus;
    rt  = bus->rt;

    /* 调度器未运行时 xTaskGetCurrentTaskHandle() 返回 NULL，会和空闲时的
       frame_owner 撞上，所以先把 NULL 挡掉，避免误放一把没持有的锁。 */
    owner = xTaskGetCurrentTaskHandle();
    if ((owner == NULL) || (rt->frame_owner != owner) ||
        (rt->frame_dev != dev)) {
        return;
    }

    if (dev->cs != NULL) {
        BSP_GPIO_Write(dev->cs, (dev->cs_active_level == 0U) ? 1U : 0U);
    }
    rt->frame_dev   = NULL;
    rt->frame_owner = NULL;
    (void)osMutexRelease(*bus->lock);
}

/**
 * @brief 提交一次传输：会话内直接搬，会话外自带开关
 * @param dev        设备句柄，不能为 NULL
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 等锁与本次搬运各自的预算（毫秒），0 表示用默认 50 ms
 * @retval DEV_SPI_OK: 传输成功
 * @retval DEV_SPI_ERR_PARAM: 句柄非法，或两个缓冲区都为 NULL
 * @retval DEV_SPI_ERR_STATE: 驱动未初始化、调度器未运行，或会话里换了设备
 * @retval DEV_SPI_ERR_TIMEOUT: 排队超时，或本次传输超时（已中止）
 * @retval DEV_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @retval DEV_SPI_ERR_BUS: 总线错误（MODF / OVR / CRC）
 * @retval DEV_SPI_ERR_HAL: 速率切换失败，或启动 / DMA / 阻塞传输出错
 *
 * @note 唯一传输入口，两种走法：调用者已用 Dev_SPI_Select() 开了帧，就直接
 *       搬，不再抢锁、换速、动片选；否则自带 Select → 搬 → Deselect，成功
 *       失败都把片选与总线还回去。一帧要分多段发时先 Select，再连调本函数。
 *       会话里换成别的设备属于误用：那次帧的速率与片选是按前一台设的，
 *       这里直接拒绝，不悄悄改总线。
 *       等待与超时策略在本层：BSP 只负责"启动 + 完成回调"，本层登记回调、
 *       睡等任务通知、超时后调 BSP_SPI_Abort()。BSP 因此不依赖任何 OS。
 *       方向由 tx / rx 推出：只发 / 只收 / 全双工 / ERR_PARAM。不得在中断调用。
 */
dev_spi_status_t Dev_SPI_Transfer(const dev_spi_dev_t *dev,
                                  const uint8_t *tx, uint8_t *rx,
                                  uint16_t len, uint32_t timeout_ms)
{
    const dev_spi_t *bus;
    dev_spi_rt_t    *rt;
    dev_spi_status_t status;
    uint32_t budget_ms;

    if ((dev == NULL) || (dev->bus == NULL)) {
        return DEV_SPI_ERR_PARAM;
    }
    bus = dev->bus;
    rt  = bus->rt;

    if ((rt->initialized == 0U) ||
        (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)) {
        return DEV_SPI_ERR_STATE;
    }

    budget_ms = (timeout_ms == 0U) ? DEV_SPI_DEFAULT_TIMEOUT_MS : timeout_ms;

    /* 会话内：锁、速率与片选已由 Select 备好，直接搬 */
    if (rt->frame_owner == xTaskGetCurrentTaskHandle()) {
        return (rt->frame_dev == dev) ? Dev_SPI_Push(dev, tx, rx, len, budget_ms)
                                      : DEV_SPI_ERR_STATE;
    }

    /* ---------- 会话外：抢总线 + 拉片选，搬完无论成败都还回去 ---------- */
    status = Dev_SPI_Select(dev, timeout_ms);
    if (status != DEV_SPI_OK) {
        return status;
    }

    status = Dev_SPI_Push(dev, tx, rx, len, budget_ms);
    Dev_SPI_Deselect(dev);

    return status;
}
