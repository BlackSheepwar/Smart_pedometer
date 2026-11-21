/**
 * @file bsp_spi.h
 * @brief SPI 硬件驱动层：只做芯片级字节搬运与速率切换，不等待、不使用任何 OS
 * @author Ricardo
 * @date 2026-11-20
 *
 * @note 本层只回答三个问题：这条总线怎么初始化、SCK 怎么降到某个频率、
 *       一段字节怎么搬出去或收回来。片选时序、总线独占、整帧语义、
 *       多设备各要各的速率，全部在设备层的 Dev_SPI（dev_spi.h）里；
 *       本层不 include 任何设备头，也不知道自己正在给谁搬字节。
 *
 *       HAL 调用、完成回调、分频表、错误恢复都是芯片知识，留在本层；
 *       哪台设备占着总线、它的片选该不该低，与芯片无关，留在 Dev_SPI。
 *
 *       搬运方式按能力分档：0 无功能 / 1 阻塞 / 2 中断 / 3 DMA。两路的实际
 *       能力由 BSP_SPI_Init() 从 HAL 句柄探测（方向允许不允许、有没有挂
 *       DMA 流），结果记在运行期的 rt 里，句柄本身不写能力值——能力是
 *       CubeMX 的配置结果，不是靠人手抄进代码的常量。
 *       可以用的那几档对外契约相同：BSP_SPI_StartTransfer() 启动即返回
 *       BSP_SPI_OK，完成或出错时在中断里回调 done(ctx, status)，也可以用
 *       BSP_SPI_IsBusy() 查是否还在进行；阻塞档在函数里同步搬完，返回即结束，
 *       done 不会被调用。
 *
 *       本层不等待、不通知、不持锁，因此不依赖 FreeRTOS，裸机也能直接用。
 *       回调运行在中断上下文，必须 ISR 安全（不能拿互斥量、不能睡）。
 *       超时与重试策略属于使用方：等待超时后调用 BSP_SPI_Abort() 中止，
 *       Abort 会清掉回调登记，迟到的完成回调不会再惊动任何人。
 *
 *       某一路探测为"无功能"时，该方向的传输直接返回 ERR_MODE（接收只认
 *       2LINES，1LINE 与 RXONLY 都不能收）。本层只服务 Master，配成 Slave
 *       会在初始化时被拒。
 *
 *       速率按请求值切换：BSP_SPI_SetRate() 求"不超过请求值"的分频并写入
 *       外设。改 BR 必须关掉 SPE，因此只能在总线空闲、片选全高时调用——
 *       这个时刻由 Dev_SPI 的 Select 保证。请求 0 表示回落到 CubeMX 配的
 *       预分频（是"回落"不是"不动"，见 bsp_spi.c 的说明）。
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
 * 方向能力取值
 *============================================================================*/
/** @brief 该方向无功能：没接线，或 HAL 的配置不允许这个方向 */
#define BSP_SPI_CAP_NONE     0U

/** @brief 只能阻塞搬运：调用返回时已搬完，不占用中断 */
#define BSP_SPI_CAP_BLOCKING 1U

/** @brief 可用外设中断：每字节一次中断，收尾时才回调 */
#define BSP_SPI_CAP_IT       2U

/** @brief 可用 DMA：启动后由 DMA 中断收尾，CPU 不参与搬字节 */
#define BSP_SPI_CAP_DMA      3U

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief BSP_SPI 返回码
 */
typedef enum {
    BSP_SPI_OK          = 0,  /* 成功 */
    BSP_SPI_ERR_PARAM   = -1, /* 参数非法 */
    BSP_SPI_ERR_STATE   = -2, /* 未初始化 / 外设状态不对 */
    BSP_SPI_ERR_TIMEOUT = -3, /* 阻塞方式传输超时 */
    BSP_SPI_ERR_MODE    = -4, /* 外设当前配置不支持该方向 */
    BSP_SPI_ERR_BUS     = -5, /* 总线错误：MODF / OVR / CRC */
    /* -6 空号：数值即契约，重排会把调用者手里的旧比较结果静默改掉 */
    BSP_SPI_ERR_HAL     = -7, /* HAL 启动失败或传输中出错 */
} bsp_spi_status_t;

/**
 * @brief SPI 总线句柄（不透明类型，成员定义在 bsp_spi.c）
 * @note 一个总线句柄对一个 HAL 外设，句柄登记的是"用哪个外设、输入时钟
 *       多少"这类配置；收发两路各能做到哪一档不写在这里，由
 *       BSP_SPI_Init() 从 HAL 句柄探测后记进运行期对象——能力是 CubeMX
 *       的配置结果，抄一遍就成了两份会漂移的事实。
 *       句柄 const 进 .rodata；运行期状态（忙标志、回调登记、当前分频、
 *       两路探测结果）放在 .c 的内部对象里，头文件不暴露。
 *       输入时钟是句柄的配置项，不是调用时现取：它是板级事实，填进句柄后
 *       分频计算只剩一次除法，调用路径上不碰时钟树的查询接口。
 */
typedef struct bsp_spi bsp_spi_t;

/**
 * @brief 一次异步传输的完成回调
 * @param ctx    启动时原样传回的上下文指针
 * @param status 传输结果，取值同 bsp_spi_status_t
 * @retval 无
 * @note 在中断里被调用，实现必须 ISR 安全：只能做"记结果 + 唤醒等待者"
 *       这类 O(1) 动作，不能拿互斥量、不能阻塞、不能调用非 FromISR 版本
 *       的 FreeRTOS API。调用 BSP_SPI_Abort() 之后本回调不会被再调用；
 *       阻塞方式下本回调永远不会被调用。
 */
typedef void (*bsp_spi_done_t)(void *ctx, bsp_spi_status_t status);

/*==============================================================================
 * 外部变量
 *============================================================================*/
/**
 * @brief 驱动管理的 SPI 总线：SPI1
 * @note 设备层的 Dev_SPI 按总线句柄调用本层；谁挂在这条总线上由设备层登记。
 */
extern const bsp_spi_t bsp_spi1;

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化 SPI 驱动：探测两路的实际搬运能力，记进运行期对象
 * @retval BSP_SPI_OK: 初始化成功（已初始化的总线会被跳过）
 * @retval BSP_SPI_ERR_PARAM: 句柄漏填输入时钟（pclk_hz 为 0）
 * @retval BSP_SPI_ERR_STATE: 缺 HAL 句柄、外设非 READY、Mode 不是 Master，
 *         或数据宽度不是 8 bit
 * @note 必须在 MX_SPIx_Init() 之后调用，且不能在中断里调用。总线互斥量
 *       不属于本层：它的创建与检查在 Dev_SPI_Init() 里。
 *       探测看两件事：方向允不允许这一路（2LINES / 1LINE / RXONLY），以及
 *       这一路有没有挂 DMA 流。结果只记运行期，句柄不写能力值。
 */
bsp_spi_status_t BSP_SPI_Init(void);

/**
 * @brief 把总线 SCK 切换到"不超过请求值"的档位
 * @param bus    总线句柄，不能为 NULL
 * @param sck_hz 请求的 SCK 上限（Hz），0 表示回落到 CubeMX 配的预分频
 * @retval BSP_SPI_OK: 已生效（与当前档位相同时是零开销的空操作）
 * @retval BSP_SPI_ERR_PARAM: 总线句柄为空，或句柄漏填输入时钟
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化
 * @retval BSP_SPI_ERR_HAL: HAL_SPI_Init 失败，外设可能停在 RESET
 * @note 改分频必须关掉 SPE，只能在总线空闲、片选全高时调用；调用者负责
 *       保证此刻没有传输在进行（Dev_SPI 在拉片选之前调本函数）。
 *       分频用句柄里的输入时钟算，不在本函数里读 RCC：时钟树变了只改
 *       句柄那一行常量，不散落在调用路径里。
 *       请求 0 会退回 CubeMX 的档位，而不是保持当前值——否则一台低速设备
 *       改过速率之后，没有速率要求的设备会继续被拖着跑。
 */
bsp_spi_status_t BSP_SPI_SetRate(const bsp_spi_t *bus, uint32_t sck_hz);

/**
 * @brief 启动一段字节的搬运：中断 / DMA 异步返回，阻塞方式同步搬完
 * @param bus        总线句柄，不能为 NULL
 * @param tx         发送缓冲区，NULL 表示本次不发
 * @param rx         接收缓冲区，NULL 表示本次不收
 * @param len        传输字节数，不能为 0
 * @param timeout_ms 超时预算（毫秒），0 表示用默认 50 ms；只对阻塞方式生效
 * @param done       异步完成回调，可为 NULL（此时只能轮询 IsBusy）
 * @param ctx        回调上下文，原样传回 done
 * @retval BSP_SPI_OK: 异步方式已启动（尚未完成）；阻塞方式已搬完且成功
 * @retval BSP_SPI_ERR_PARAM: 句柄或长度非法，或两个缓冲区都为 NULL
 * @retval BSP_SPI_ERR_STATE: 驱动未初始化，或上一笔异步传输还没收尾
 * @retval BSP_SPI_ERR_MODE: 外设当前方向不支持本次传输
 * @retval BSP_SPI_ERR_TIMEOUT: 阻塞方式传输超时
 * @retval BSP_SPI_ERR_HAL: 各档位都启动失败，或阻塞方式传输出错
 * @retval BSP_SPI_ERR_BUS: 阻塞方式遇到总线错误
 * @note 调用者必须已持有总线、已拉低目标设备的片选、已把速率切好；
 *       这三件事都在 Dev_SPI 里做。方向由 tx / rx 推出：只发 / 只收 /
 *       全双工 / ERR_PARAM。异步方式返回 OK 不等于搬完，要靠 done 或
 *       IsBusy() 才知道结束；阻塞方式返回 OK 即为搬完。
 *       生效档位由 Init 探测出的两路能力决定，全双工取两路中较低的一档；
 *       该档启动失败就向下退档重试，直到阻塞档（DMA → 中断 → 阻塞）。
 *       某一档一旦启动成功，本次传输就固定在该档，不会中途换。
 *       退到阻塞档时同步搬完，done 不会被调用。
 *       不得在中断里调用。
 */
bsp_spi_status_t BSP_SPI_StartTransfer(const bsp_spi_t *bus,
                                       const uint8_t *tx, uint8_t *rx,
                                       uint16_t len, uint32_t timeout_ms,
                                       bsp_spi_done_t done, void *ctx);

/**
 * @brief 查询总线上是否还有未完成的传输
 * @param bus 总线句柄，不能为 NULL
 * @retval 1: 异步传输还在进行
 * @retval 0: 空闲，或已由回调收尾；句柄为空时也返回 0
 * @note 启用了 done 回调的调用者通常不需要它；用于"没给回调、只想轮询"
 *       的用法。阻塞方式启动之后这里恒为 0。
 */
uint8_t BSP_SPI_IsBusy(const bsp_spi_t *bus);

/**
 * @brief 中止当前传输并让外设回到 READY，清掉回调登记
 * @param bus 总线句柄，不能为 NULL
 * @retval BSP_SPI_OK: 已回到空闲
 * @retval BSP_SPI_ERR_PARAM: 总线句柄为空
 * @retval BSP_SPI_ERR_HAL: HAL_SPI_Abort 失败，已用 DeInit/Init 硬复位一次
 * @note 只允许在任务上下文调用。中止自身失败（传输已跑飞）时用
 *       DeInit/Init 重新初始化一次，保证总线能交给下一个调用者。
 *       返回后 done 已被清空，迟到的完成/错误回调不会再通知任何人。
 */
bsp_spi_status_t BSP_SPI_Abort(const bsp_spi_t *bus);

#endif /* __BSP_SPI_H__ */
