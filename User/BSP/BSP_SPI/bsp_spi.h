/**
 * @file bsp_spi.h
 * @brief SPI 硬件驱动层：设备句柄统一入口，互斥量排队 + 完成唤醒
 * @author Ricardo
 * @date 2026-10-29
 *
 * @note 对外只有一级句柄：bsp_spi_dev_t 描述"挂在哪条总线上、用哪根片选"，
 *       所有传输都从设备句柄进入；总线对象是 BSP 内部实现，不对外暴露——
 *       同一条总线挂多个设备时，每个设备各持一份自己的设备句柄。
 *       设备级函数自动拉/放 CS，帧内入口只管搬字节。
 *
 *       两种用法，按事务长度选：
 *         单段  BSP_SPI_Dev_Transfer()，一次调用走完 Select→写→Deselect；
 *         多段  Select() + TransferLocked() × N + Deselect()，锁与片选同生命
 *               周期，整帧独占总线。命令与参数不能跨帧的器件必须用这种，
 *               否则别的设备能在帧中间插进来，字节会被本设备误收。
 *
 *       一次事务同步完成：抢互斥量排队 → 启动传输 → 等完成唤醒 → 返回。
 *       配了 DMA 走 DMA + 任务通知，没配自动退回 HAL 阻塞接口。
 *       方向能力由 CubeMX 的 Direction 决定，不支持的组合返回 ERR_MODE；
 *       本层只服务 Master，配成 Slave 会在 BSP_SPI_Init() 被拒。
 *
 *       速率按设备配置：设备句柄里的 sck_hz 是该器件能接受的 SCK 上限，
 *       Select() 时按"不超过它"换算出分频，在片选拉低之前切换完成。
 *       于是同一条总线上可以同时挂两种速率不同的器件——
 *       两者引用同一个总线句柄、各写各的 sck_hz 即可，调用方完全无感。
 *       sck_hz 填 0 表示回落到 CubeMX 里配的预分频——注意是"回落"而不是
 *       "不改"：总线上只要有设备改过速率，填 0 的设备下一次 Select 就会把
 *       外设恢复到 CubeMX 的档位，避免被上一个设备的低速拖着走。
 *       公共头文件不暴露 HAL 类型，HAL 句柄只存在于 .c。
 */

#ifndef __BSP_SPI_H__
#define __BSP_SPI_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief BSP_SPI 返回码
 */
typedef enum {
    BSP_SPI_OK          = 0,  /* 成功 */
    BSP_SPI_ERR_PARAM   = -1, /* 参数非法 */
    BSP_SPI_ERR_STATE   = -2, /* 未初始化 / 调度器未跑 / 外设状态不对 / 已在帧中 */
    BSP_SPI_ERR_TIMEOUT = -3, /* 排队等锁超时，或传输超时 */
    BSP_SPI_ERR_MODE    = -4, /* 外设当前配置不支持该方向 */
    BSP_SPI_ERR_BUS     = -5, /* 总线错误：MODF / OVR / CRC */
    BSP_SPI_ERR_HAL     = -7, /* HAL 启动失败或 DMA 错误 */
} bsp_spi_status_t;

/**
 * @brief SPI 设备句柄（不透明类型，成员定义在 bsp_spi.c）
 * @note 只描述"挂在哪条总线上、用哪根片选、什么极性"，不含引脚号。
 */
typedef struct bsp_spi_dev bsp_spi_dev_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/**
 * @brief SPI1 上的两个设备：各持一根片选、共用 SCK/MOSI/MISO
 * @note 每个设备各写各的 sck_hz，本层在片选拉低之前自动换挡，
 *       调用方不需要关心上一条总线上一次跑的是哪台。
 */
extern const bsp_spi_dev_t bsp_spi1_dev_lcd;
extern const bsp_spi_dev_t bsp_spi1_dev_touch;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化 SPI 驱动：轮询总线表，登记句柄与方向能力
 * @retval BSP_SPI_OK: 初始化成功（已初始化的总线会被跳过）
 * @retval BSP_SPI_ERR_STATE: 缺 HAL 句柄或互斥量、外设非 READY、Mode 不是
 *         Master，或数据宽度不是 8 bit
 * @note 必须在 USER CODE BEGIN RTOS_MUTEX 内调用（CubeMX 刚建完总线互斥量），
 *       且 MX_SPIx_Init() 已执行过。不能在中断里调用。
 */
bsp_spi_status_t BSP_SPI_Init(void);

/**
 * @brief 在设备所属的已持有总线上搬一段字节（不抢锁、不碰片选）
 * @param dev        设备句柄，不能为 NULL，且必须是 Select() 选中的那台
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 本次传输的超时预算（毫秒），0 表示用默认 50 ms
 * @retval BSP_SPI_OK: 传输成功
 * @retval BSP_SPI_ERR_PARAM: 句柄或长度非法，或两个缓冲区都为 NULL
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化、调度器未运行，或当前帧不是
 *         本任务、本设备开的
 * @retval BSP_SPI_ERR_TIMEOUT: 传输超时（不存在排队，只有传输）
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @retval BSP_SPI_ERR_BUS: 总线错误
 * @retval BSP_SPI_ERR_HAL: HAL 启动失败或 DMA 错误
 * @note 只给帧内分段用：调用前必须 BSP_SPI_Dev_Select() 同一个设备，与
 *       Deselect() 成对。总线由 dev->bus 取得，调用者不用单独持有总线句柄。
 *       内部校验 frame_owner 与 frame_dev，不是"本任务 + 本设备"就返回
 *       ERR_STATE，把"忘了开帧""开了别的设备的帧""跨任务误用"变成可查的
 *       错误码。
 *       方向由 tx / rx 推出：只发 / 只收 / 全双工 / ERR_PARAM。不得在中断调用。
 */
bsp_spi_status_t BSP_SPI_TransferLocked(const bsp_spi_dev_t *dev,
                                        const uint8_t *tx, uint8_t *rx,
                                        uint16_t len, uint32_t timeout_ms);

/**
 * @brief 取得总线所有权并拉有效片选（帧开始）
 * @param dev        设备句柄，不能为 NULL
 * @param timeout_ms 排队等锁的预算（毫秒），0 表示用默认 50 ms
 * @retval BSP_SPI_OK: 已持有总线，片选已拉有效
 * @retval BSP_SPI_ERR_PARAM: 句柄或总线为空
 * @retval BSP_SPI_ERR_STATE: 未初始化、调度器未运行，或本任务已经在帧中
 * @retval BSP_SPI_ERR_TIMEOUT: 等锁超时；此时片选没有被拉低
 * @retval BSP_SPI_ERR_HAL: 速率切换失败；此时片选没有被拉低
 * @note 电平按 cs_active_level 换算。抢锁成功才拉片选，不存在"CS 已低、
 *       总线还在别人手里"的窗口。与 Deselect() 成对，帧内只能调
 *       TransferLocked()。失败后不要调 Deselect。会阻塞，不得在中断中调用。
 *       本函数同时完成速率切换：句柄的 sck_hz 非 0 时按"不超过该值"算出
 *       分频并写入外设，片选拉低之前生效。因此同一总线上的两个器件可以
 *       各用各的速率，调用方不需要为此做任何事；sck_hz 为 0 则回落到
 *       CubeMX 配的预分频。
 */
bsp_spi_status_t BSP_SPI_Dev_Select(const bsp_spi_dev_t *dev,
                                    uint32_t timeout_ms);

/**
 * @brief 放无效片选并归还总线（帧结束）
 * @param dev 设备句柄，不能为 NULL
 * @retval 无
 * @note 与 Select() 配对。只对"本任务、本设备"开的帧生效：任务不是帧主、
 *       或 dev 不是本帧选中的那台，都直接返回，避免放掉别人的锁或抬错片选。
 *       必须校验设备：否则 Select(dev1) 后误调 Deselect(dev2) 会放锁而
 *       dev1 的 CS 仍低，接下来用 dev2 的传输会被 dev1 一并收走。
 */
void BSP_SPI_Dev_Deselect(const bsp_spi_dev_t *dev);

/**
 * @brief 在指定设备上提交一次传输，自动拉放片选
 * @param dev        设备句柄，不能为 NULL
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 超时预算（毫秒），0 表示用默认 50 ms
 * @retval BSP_SPI_OK: 传输成功
 * @retval BSP_SPI_ERR_PARAM: 句柄或长度非法，或两个缓冲区都为 NULL
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化、调度器未运行，或本任务已在帧中
 * @retval BSP_SPI_ERR_TIMEOUT: 排队或传输超时
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @retval BSP_SPI_ERR_BUS: 总线错误
 * @retval BSP_SPI_ERR_HAL: HAL 错误
 * @note 一次完整帧事务：Select → TransferLocked → Deselect，成功失败都
 *       把片选与总线还回去。方向推断规则与 TransferLocked() 一致。
 *       一帧要分多段发时别用本函数，改用三个底层函数自己拼。
 */
bsp_spi_status_t BSP_SPI_Dev_Transfer(const bsp_spi_dev_t *dev,
                                      const uint8_t *tx, uint8_t *rx,
                                      uint16_t len, uint32_t timeout_ms);

#endif /* __BSP_SPI_H__ */
