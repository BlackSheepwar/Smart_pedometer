/**
 * @file bsp_iic.c
 * @brief IIC 主机驱动层：初始化 / 换速 / 启动一次搬运，不等待、不持锁
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 设计模型：
 *       1. 每路总线一个常量句柄（hal 指针 + 运行块地址），运行块
 *          （是否就绪、忙、当前时钟、完成回调、生效搬运方式）全在运行块里；
 *       2. 搬运分三级：DMA / 中断 / 阻塞。初始化时从 HAL 句柄探测这条总线
 *          实际能做到哪一级（看收发 DMA 流挂没挂），每笔按方向取该方向的
 *          能力值；启动失败时向下退一级重试；
 *       3. 异步两级（DMA / 中断）只"启动"：登记 done 后立刻返回，不持锁、
 *          不睡等；阻塞级同步搬完再返回，不经过完成回调；
 *       4. 完成/错误中断只做三件事：摘掉登记、清忙标志、调用 done；
 *       5. 超时判定与复位由调用者发起：BSP_IIC_Abort() 先把 HAL 状态机
 *          强制拉回 READY，再 DeInit/Init，否则外设停在 BUSY 时 DeInit
 *          会直接返回 HAL_BUSY、MspDeInit 不执行，越复位越坏。
 *
 *       外设中断必须开：DMA 只搬字节，一次传输的 ADDR / BTF / STOPF 收尾
 *       仍由 I2Cx 的 EV 中断推进，出错走 ER 中断；中断方式更是全靠 EV 中断
 *       推进状态机。只开 DMA 流中断、不开外设中断的话，每笔传输都会永远
 *       等不到完成。
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
 * @brief IIC 总线运行块
 * @note 每次初始化和每笔搬运都会读写，所以不能放进 const 句柄。
 */
typedef struct {
    bsp_iic_done_t volatile done;      /* 本笔完成回调，空闲为 NULL */
    const bsp_iic_wait_t *volatile wait; /* 接收方描述，与 done 同生共死 */
    volatile uint8_t        busy;      /* 1: 有搬运未收尾 */
    volatile uint8_t        initialized; /* 1: 已完成初始化 */
    uint8_t                 tx_mode;   /* 发送方向探测出的搬运方式 */
    uint8_t                 rx_mode;   /* 接收方向探测出的搬运方式 */
    uint32_t                clk_now;   /* 当前生效的总线时钟（Hz） */
    uint32_t                clk_init;  /* 初始化时登记的总线时钟（Hz） */
} bsp_iic_rt_t;

/**
 * @brief IIC 总线对象
 * @note  hal 是 void *，避免公共头文件引入 HAL 类型；rt 指向运行块。
 */
struct bsp_iic {
    void            *hal; /* HAL I2C 句柄（I2C_HandleTypeDef *） */
    bsp_iic_rt_t    *rt;  /* 运行块 */
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
 * @brief 探测发送方向实际能做到的搬运方式
 * @param hi2c HAL 句柄
 * @retval BSP_IIC_CAP_DMA: 发送 DMA 流已挂上
 * @retval BSP_IIC_CAP_IT: 没有发送 DMA 流，走事件中断
 * @note 阻塞方式任何情况下都可用，所以不作为探测结果返回。
 */
static uint8_t BSP_IIC_ActualTx(const I2C_HandleTypeDef *hi2c)
{
    return (hi2c->hdmatx != NULL) ? (uint8_t)BSP_IIC_CAP_DMA
                                  : (uint8_t)BSP_IIC_CAP_IT;
}

/**
 * @brief 探测接收方向实际能做到的搬运方式
 * @param hi2c HAL 句柄
 * @retval BSP_IIC_CAP_DMA: 接收 DMA 流已挂上
 * @retval BSP_IIC_CAP_IT: 没有接收 DMA 流，走事件中断
 */
static uint8_t BSP_IIC_ActualRx(const I2C_HandleTypeDef *hi2c)
{
    return (hi2c->hdmarx != NULL) ? (uint8_t)BSP_IIC_CAP_DMA
                                  : (uint8_t)BSP_IIC_CAP_IT;
}

/**
 * @brief 把 HAL 的结果翻译成 BSP 返回码
 * @param hi2c   HAL 句柄，用于读取 ErrorCode
 * @param status HAL 返回值
 * @retval BSP_IIC_OK: 成功
 * @retval BSP_IIC_ERR_TIMEOUT: HAL 报超时
 * @retval BSP_IIC_ERR_STATE: HAL 报忙
 * @retval BSP_IIC_ERR_NACK: 从机 NACK
 * @retval BSP_IIC_ERR_BUS: 总线错误
 * @retval BSP_IIC_ERR_HAL: 其余 HAL 错误
 */
static bsp_iic_status_t BSP_IIC_MapHal(const I2C_HandleTypeDef *hi2c,
                                       HAL_StatusTypeDef status)
{
    if (status == HAL_OK) {
        return BSP_IIC_OK;
    }
    if (status == HAL_TIMEOUT) {
        return BSP_IIC_ERR_TIMEOUT;
    }
    if (status == HAL_BUSY) {
        return BSP_IIC_ERR_STATE;
    }
    if ((hi2c->ErrorCode & HAL_I2C_ERROR_AF) != 0U) {
        return BSP_IIC_ERR_NACK;
    }
    if ((hi2c->ErrorCode & (HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_ARLO |
                            HAL_I2C_ERROR_OVR)) != 0U) {
        return BSP_IIC_ERR_BUS;
    }
    return BSP_IIC_ERR_HAL;
}

/**
 * @brief 阻塞方式启动一笔：调用返回时已经搬完（或已失败）
 * @param bus        总线对象
 * @param msg        传输命令
 * @param timeout_ms 超时（ms），直接交给 HAL
 * @retval HAL_OK: 已搬完
 * @retval 其他: HAL 失败或外设报超时
 * @note 7 bit 地址在这里左移成 HAL 需要的 8 bit 形式；MEM 模式一次调用
 *       完成子地址写 + repeated START。本函数不经过任何完成回调。
 */
static HAL_StatusTypeDef BSP_IIC_RunBlocking(const bsp_iic_t *bus,
                                             const bsp_iic_msg_t *msg,
                                             uint32_t timeout_ms)
{
    I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)bus->hal;
    uint16_t dev_addr = (uint16_t)(msg->dev_addr << 1);

    if (msg->mode == (uint8_t)BSP_IIC_MODE_MEM) {
        uint16_t mem_size = (msg->mem_addr_size == 2U) ?
                            I2C_MEMADD_SIZE_16BIT : I2C_MEMADD_SIZE_8BIT;

        if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
            return HAL_I2C_Mem_Write(hi2c, dev_addr, msg->mem_addr, mem_size,
                                     msg->data, msg->len, timeout_ms);
        }
        return HAL_I2C_Mem_Read(hi2c, dev_addr, msg->mem_addr, mem_size,
                                msg->data, msg->len, timeout_ms);
    }

    if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
        return HAL_I2C_Master_Transmit(hi2c, dev_addr, msg->data, msg->len,
                                       timeout_ms);
    }
    return HAL_I2C_Master_Receive(hi2c, dev_addr, msg->data, msg->len,
                                  timeout_ms);
}

/**
 * @brief 中断方式启动一笔：交给 I2C 事件中断推进，完成点只在回调
 * @param bus 总线对象
 * @param msg 传输命令
 * @retval HAL_OK: 已启动
 * @retval 其他: HAL 拒绝启动
 * @note 中断方式下每一个字节都由 I2Cx 的 EV 中断推进，数据量越大中断次数
 *       越多；只适合命令、寄存器这类短传输。
 */
static HAL_StatusTypeDef BSP_IIC_StartIt(const bsp_iic_t *bus,
                                         const bsp_iic_msg_t *msg)
{
    I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)bus->hal;
    uint16_t dev_addr = (uint16_t)(msg->dev_addr << 1);

    if (msg->mode == (uint8_t)BSP_IIC_MODE_MEM) {
        uint16_t mem_size = (msg->mem_addr_size == 2U) ?
                            I2C_MEMADD_SIZE_16BIT : I2C_MEMADD_SIZE_8BIT;

        if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
            return HAL_I2C_Mem_Write_IT(hi2c, dev_addr, msg->mem_addr,
                                        mem_size, msg->data, msg->len);
        }
        return HAL_I2C_Mem_Read_IT(hi2c, dev_addr, msg->mem_addr,
                                   mem_size, msg->data, msg->len);
    }

    if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
        return HAL_I2C_Master_Transmit_IT(hi2c, dev_addr, msg->data,
                                          msg->len);
    }
    return HAL_I2C_Master_Receive_IT(hi2c, dev_addr, msg->data, msg->len);
}

/**
 * @brief DMA 方式启动一笔：字节由 DMA 流搬，完成点只在回调
 * @param bus 总线对象
 * @param msg 传输命令
 * @retval HAL_OK: 已启动
 * @retval 其他: HAL 拒绝启动（DMA 通道没配、外设忙等）
 * @note DMA 只搬数据字节，ADDR / BTF / STOPF 仍由 I2Cx 的 EV 中断推进，
 *       所以两个中断都得开。
 * @note 四条 HAL 入口的阻塞语义不一样：RAW 模式挂好 DMA 就返回，地址阶段
 *       在中断里推进；MEM 模式返回前会在调用线程里忙等地址与子地址写完
 *       （上限 I2C_TIMEOUT_FLAG 35 ms）。这个差别属 HAL，不改写它。
 */
static HAL_StatusTypeDef BSP_IIC_StartDma(const bsp_iic_t *bus,
                                          const bsp_iic_msg_t *msg)
{
    I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)bus->hal;
    uint16_t dev_addr = (uint16_t)(msg->dev_addr << 1);

    if (msg->mode == (uint8_t)BSP_IIC_MODE_MEM) {
        uint16_t mem_size = (msg->mem_addr_size == 2U) ?
                            I2C_MEMADD_SIZE_16BIT : I2C_MEMADD_SIZE_8BIT;

        if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
            return HAL_I2C_Mem_Write_DMA(hi2c, dev_addr, msg->mem_addr,
                                         mem_size, msg->data, msg->len);
        }
        return HAL_I2C_Mem_Read_DMA(hi2c, dev_addr, msg->mem_addr,
                                    mem_size, msg->data, msg->len);
    }

    if (msg->dir == (uint8_t)BSP_IIC_DIR_TX) {
        return HAL_I2C_Master_Transmit_DMA(hi2c, dev_addr, msg->data,
                                           msg->len);
    }
    return HAL_I2C_Master_Receive_DMA(hi2c, dev_addr, msg->data, msg->len);
}

/**
 * @brief 按一种搬运方式启动一笔：分派到阻塞 / 中断 / DMA 三个搬运函数
 * @param bus        总线对象
 * @param msg        传输命令
 * @param mode       搬运方式，取 BSP_IIC_CAP_BLOCKING / _IT / _DMA
 * @param timeout_ms 仅阻塞方式使用的超时
 * @param done       异步方式的完成回调，阻塞方式忽略
 * @param wait       接收方描述，原样传给 done
 * @retval BSP_IIC_OK: 异步方式已启动，或阻塞方式已搬完
 * @retval 其他: 启动失败，已复位外设，调用者可以退档重试
 * @note 只有异步两档需要登记 done/wait/busy；阻塞档在函数里就搬完了，
 *       登记了反而会让调用者误以为还要等通知。异步档启动失败时先复位
 *       再返回，所以不会留下"忙"的假状态。
 */
static bsp_iic_status_t BSP_IIC_StartOnce(const bsp_iic_t *bus,
                                          const bsp_iic_msg_t *msg,
                                          uint8_t mode, uint32_t timeout_ms,
                                          bsp_iic_done_t done,
                                          const bsp_iic_wait_t *wait)
{
    bsp_iic_rt_t *rt = bus->rt;
    I2C_HandleTypeDef *hi2c = (I2C_HandleTypeDef *)bus->hal;
    HAL_StatusTypeDef status;

    if (mode == (uint8_t)BSP_IIC_CAP_BLOCKING) {
        status = BSP_IIC_RunBlocking(bus, msg, timeout_ms);
        if (status != HAL_OK) {
            (void)BSP_IIC_Abort(bus);
            return BSP_IIC_MapHal(hi2c, status);
        }
        return BSP_IIC_OK;
    }

    rt->done = done;
    rt->wait = wait;
    rt->busy = 1U;

    if (mode == (uint8_t)BSP_IIC_CAP_IT) {
        status = BSP_IIC_StartIt(bus, msg);
    } else {
        status = BSP_IIC_StartDma(bus, msg);
    }
    if (status != HAL_OK) {
        (void)BSP_IIC_Abort(bus);
        return BSP_IIC_ERR_HAL;
    }
    return BSP_IIC_OK;
}

/**
 * @brief 初始化时探测两路搬运能力并存入运行块
 * @param bus  总线对象
 * @param hi2c HAL 句柄
 * @retval 无
 * @note 只在 Init 里调用一次；探测结果不会随运行期变化，除非重新初始化。
 */
static void BSP_IIC_DetectModes(const bsp_iic_t *bus,
                                const I2C_HandleTypeDef *hi2c)
{
    bus->rt->tx_mode = BSP_IIC_ActualTx(hi2c);
    bus->rt->rx_mode = BSP_IIC_ActualRx(hi2c);
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
    bsp_iic_done_t        done = rt->done;
    const bsp_iic_wait_t *wait = rt->wait;

    rt->done = NULL;
    rt->wait = NULL;
    rt->busy = 0U;

    if (done != NULL) {
        done(wait, status);
    }
}

/*==============================================================================
 * 接口实现
 *============================================================================*/
/**
 * @brief 初始化 IIC 驱动：逐路登记 HAL 句柄、外设状态与初始时钟，并探测搬运能力
 * @retval BSP_IIC_OK: 全部实例就绪（已就绪的实例会被跳过）
 * @retval BSP_IIC_ERR_STATE: 某一路缺 HAL 句柄，或外设不在 READY
 * @note 必须在 MX_I2Cx_Init() 之后调用；不碰任何 OS 对象。能力探测只看
 *       收发 DMA 流挂没挂：挂了是 DMA 级，否则中断级；阻塞级永远可用。
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
        rt->wait       = NULL;
        rt->busy       = 0U;
        rt->clk_now    = hi2c->Init.ClockSpeed;
        rt->clk_init   = hi2c->Init.ClockSpeed;
        BSP_IIC_DetectModes(bus, hi2c);
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
 * @brief 启动一次搬运：阻塞方式同步搬完，中断 / DMA 方式登记回调后返回
 * @param bus        总线句柄，不能为 NULL
 * @param msg        传输命令，不能为 NULL；data 在 done 之前必须保持有效
 * @param timeout_ms 仅阻塞方式使用的超时（ms）；异步方式忽略
 * @param done       完成回调（中断 / DMA 方式用），可以为 NULL
 * @param wait       接收方描述，原样传给 done，原样回传
 * @retval BSP_IIC_OK: 阻塞方式已搬完；异步方式已交给外设（完成点只在 done）
 * @retval BSP_IIC_ERR_PARAM: 地址、方向、模式、长度或缓冲区非法
 * @retval BSP_IIC_ERR_STATE: 未初始化，或上一笔还没收尾
 * @retval 其他: 退到阻塞级仍失败时，返回该级的具体错误
 * @note 启动失败时向下退一级重试：DMA 起不来改中断，中断起不来改阻塞；
 *       退档前已复位外设，所以调用者可以直接发起下一笔。
 * @note 异步方式的"已启动"从本函数返回算起。MEM 模式里 HAL 会在返回前
 *       忙等地址阶段，这段 CPU 记在调用者头上，不是中断里搬的；本端口的
 *       上限取 I2C_TIMEOUT_FLAG（35 ms）。
 */
bsp_iic_status_t BSP_IIC_StartTransfer(const bsp_iic_t *bus,
                                       const bsp_iic_msg_t *msg,
                                       uint32_t timeout_ms,
                                       bsp_iic_done_t done,
                                       const bsp_iic_wait_t *wait)
{
    bsp_iic_rt_t *rt;
    uint8_t mode;
    bsp_iic_status_t status;

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

    mode = (msg->dir == (uint8_t)BSP_IIC_DIR_TX) ? rt->tx_mode : rt->rx_mode;
    if (mode < (uint8_t)BSP_IIC_CAP_BLOCKING) {
        mode = (uint8_t)BSP_IIC_CAP_BLOCKING;
    } else if (mode > (uint8_t)BSP_IIC_CAP_DMA) {
        mode = (uint8_t)BSP_IIC_CAP_DMA;
    }

    for (;;) {
        status = BSP_IIC_StartOnce(bus, msg, mode, timeout_ms, done, wait);
        if (status == BSP_IIC_OK) {
            return BSP_IIC_OK;
        }
        if (mode <= (uint8_t)BSP_IIC_CAP_BLOCKING) {
            return status;
        }
        mode--;
    }
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
    rt->wait = NULL;
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
 * 事件推进状态机）与 DMA 流的中断（字节搬完）。中断方式只用前者，DMA 方式
 * 两者都要用，HAL 把两条路径收在同一组完成回调里，所以下面四个回调既可能
 * 从外设中断进，也可能从 DMA 中断进。错误回调的来源是 I2Cx 的 ER 中断，
 * 或 DMA 流的错误中断。阻塞方式不经过这里，结果由启动函数返回。
 * 这里覆盖的是 HAL 的 __weak 回调，不是 IRQ 入口本身。
 *============================================================================*/
/**
 * @brief 主机发送完成回调
 * @param hi2c HAL IIC 句柄
 * @note 强定义覆盖 HAL 的 __weak 版本；RAW 发送路径的完成点。中断与 DMA
 *       方式共用本回调，阻塞方式不经过。
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
 * @note 强定义覆盖 HAL 的 __weak 版本；RAW 接收路径的完成点。中断与 DMA
 *       方式共用本回调，阻塞方式不经过。
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
 *       两条路径的回调不能互相替代，HAL 按传输类型分别调用；中断与 DMA
 *       方式共用本回调，阻塞方式不经过。
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
 * @note 强定义覆盖 HAL 的 __weak 版本；MEM 读路径的完成点。中断与 DMA
 *       方式共用本回调，阻塞方式不经过。
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
