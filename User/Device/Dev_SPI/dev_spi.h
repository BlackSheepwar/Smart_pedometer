/**
 * @file dev_spi.h
 * @brief SPI 虚拟设备层：总线独占、片选时序、多设备各要各的速率
 * @author Ricardo
 * @date 2026-11-20
 *
 * @note 本层把"一条 SPI 总线怎么被多台器件共用"收在一处，向上提供设备视角：
 *       选设备、搬一帧、放设备。它不知道芯片型号（HAL 与分频表在 bsp_spi），
 *       也不知道业务（画什么、读什么在更上层）。
 *
 *       与 bsp_spi 的分工按职责划：
 *         bsp_spi  记住这条总线是哪个 HAL 外设、怎么降速、怎么搬字节——
 *                  全是芯片知识，换 MCU 只改它；
 *         dev_spi  记住总线上挂着哪几台器件、各要多快、谁现在占着它——
 *                  与芯片无关，换 MCU 一个字都不用改。
 *       两条职责分开之后，"移植只改 BSP、且 BSP 保持极简"才能同时成立。
 *
 *       两种用法，同一个入口 Dev_SPI_Transfer()：
 *         单段  直接调，一次调用走完 选→发→放；
 *         多段  先 Select()，再连调 N 次 Transfer()，最后 Deselect()。
 *               锁与片选同一生命周期，整帧独占总线；命令与参数不能跨帧的
 *               器件必须用这种，否则别的设备能在帧中间插进来，字节会被本
 *               设备误收。
 *
 *       一次事务同步完成：抢互斥量排队 → 按设备切速率 → 拉片选 →
 *       启动传输并等完成 → 抬片选 → 放锁。其中"启动"在 bsp_spi（只登记
 *       完成回调后返回），"等待"在本层（任务通知 + 超时后 Abort），
 *       所以 bsp_spi 不含任何 OS 调用，换 RTOS 或改裸机这一层不用动。
 *       互斥量由 CubeMX 的 .ioc 生成，本层只 extern 其句柄地址，
 *       不创建任何 OS 对象。
 *       公共头文件不暴露任何 bsp_ 类型，HAL 句柄只存在于 bsp_spi.c。
 */

#ifndef __DEV_SPI_H__
#define __DEV_SPI_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief Dev_SPI 返回码
 * @note 取值与 BSP_SPI_* 一一对应，是为了让本层只做一次翻译，不改变上层
 *       对错误类别的判断习惯；上层只认 DEV_SPI_*。
 */
typedef enum {
    DEV_SPI_OK          = 0,  /* 成功 */
    DEV_SPI_ERR_PARAM   = -1, /* 参数非法 */
    DEV_SPI_ERR_STATE   = -2, /* 未初始化 / 调度器未跑 / 帧所有权不对 */
    DEV_SPI_ERR_TIMEOUT = -3, /* 排队等锁超时，或传输超时 */
    DEV_SPI_ERR_MODE    = -4, /* 外设当前配置不支持该方向 */
    DEV_SPI_ERR_BUS     = -5, /* 总线错误：MODF / OVR / CRC */
    DEV_SPI_ERR_HAL     = -6, /* 速率切换或 HAL 启动失败 */
} dev_spi_status_t;

/**
 * @brief SPI 总线句柄（不透明类型，成员定义在 dev_spi.c）
 * @note 只描述"用哪条 BSP 总线、哪把互斥量"，不含任何引脚。
 */
typedef struct dev_spi dev_spi_t;

/**
 * @brief SPI 设备句柄（不透明类型，成员定义在 dev_spi.c）
 * @note 描述"挂在哪条总线上、用哪根片选、片选什么极性、SCK 要多快"，
 *       不含引脚号（片选引脚由 BSP_GPIO 的句柄携带）。
 */
typedef struct dev_spi_dev dev_spi_dev_t;

/*==============================================================================
 * 外部变量
 *============================================================================*/
/**
 * @brief SPI1 总线：BSP 总线句柄 + 互斥量句柄 + 运行块
 * @note Init 一次登记一条总线；同一条总线上的设备由 dev_spi_dev_* 携带。
 */
extern const dev_spi_t dev_spi1;

/**
 * @brief 挂在 SPI1 上的显示面板：片选引脚、片选极性、SCK 速率
 * @note 每多一台挂在同一条总线上的设备，在 .c 的设备表里登记一份并在本处声明。
 */
extern const dev_spi_dev_t dev_spi_dev_lcd;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化一路 SPI 总线：检查总线互斥量与 BSP 是否就绪
 * @param bus 总线句柄
 * @retval DEV_SPI_OK: 初始化成功；已初始化的总线直接返回
 * @retval DEV_SPI_ERR_STATE: BSP 未初始化，或总线互斥量句柄仍为 NULL
 *
 * @note 必须在 BSP_SPI_Init() 之后、且互斥量已由 freertos.c 建好之后调用
 *       （CubeMX 生成的 USER CODE BEGIN RTOS_MUTEX 段内）。每条总线调一次，
 *       多路总线各调各的。不能在中断里调用。
 */
dev_spi_status_t Dev_SPI_Init(const dev_spi_t *bus);

/**
 * @brief 取得总线所有权并拉有效片选（帧开始）
 * @param dev        设备句柄，不能为 NULL
 * @param timeout_ms 排队等锁的预算（毫秒），0 表示用默认 50 ms
 * @retval DEV_SPI_OK: 已持有总线，片选已拉有效
 * @retval DEV_SPI_ERR_PARAM: 句柄或总线为空
 * @retval DEV_SPI_ERR_STATE: 未初始化、调度器未运行，或本任务已经在帧中
 * @retval DEV_SPI_ERR_TIMEOUT: 等锁超时；此时片选没有被拉低
 * @retval DEV_SPI_ERR_HAL: 速率切换失败；此时片选没有被拉低
 * @note 电平按 cs_active_level 换算。抢锁成功才拉片选，不存在"CS 已低、
 *       总线还在别人手里"的窗口。与 Deselect() 成对，帧内只调
 *       Transfer()（它检测到帧已开就不再抢锁）。失败后不要调 Deselect。
 *       会阻塞，不得在中断中调用。
 *       本函数同时完成速率切换：句柄的 sck_hz 非 0 时按"不超过该值"算出
 *       分频并写入外设，片选拉低之前生效；sck_hz 为 0 则回落到 CubeMX
 *       配的预分频。
 */
dev_spi_status_t Dev_SPI_Select(const dev_spi_dev_t *dev,
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
void Dev_SPI_Deselect(const dev_spi_dev_t *dev);

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
 * @note 唯一传输入口，两种走法：已用 Dev_SPI_Select() 开了帧就直接搬，不再
 *       抢锁、换速、动片选；否则自带 Select → 搬 → Deselect，成功失败都把
 *       片选与总线还回去。一帧要分多段发时先 Select，再连调本函数。
 *       会话里换成别的设备返回 ERR_STATE，不会悄悄改总线速率。
 *       方向由 tx / rx 推出：只发 / 只收 / 全双工 / ERR_PARAM。不得在中断调用。
 */
dev_spi_status_t Dev_SPI_Transfer(const dev_spi_dev_t *dev,
                                  const uint8_t *tx, uint8_t *rx,
                                  uint16_t len, uint32_t timeout_ms);

#endif /* __DEV_SPI_H__ */
