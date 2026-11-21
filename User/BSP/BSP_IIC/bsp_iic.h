/**
 * @file bsp_iic.h
 * @brief IIC 主机驱动层：初始化 / 换速 / 启动一次搬运，不等待、不持锁
 * @author Ricardo
 * @date 2026-11-21
 *
 * @note 本层只回答三个问题：外设怎么初始化、总线时钟怎么改、一次搬运怎么
 *       启动。从机地址怎么来、寄存器子地址多宽、总线归谁用、等多久、超时
 *       怎么处理，全在设备层的 IIC 虚拟外设里；所以本层不含任何 OS 调用，
 *       也不认识任何具体器件。
 *
 *       搬运有三级，初始化时从 HAL 句柄探测出这条总线实际能做到哪一级：
 *         3 = DMA    有收发 DMA 流，启动后靠流中断完成
 *         2 = 中断   没有 DMA 流，靠 I2C 事件中断推进
 *         1 = 阻塞   同步搬完再返回，不经过任何完成回调
 *       每一笔按方向取该方向的能力值，启动失败时向下退一级重试
 *       （DMA → 中断 → 阻塞）。退档只发生在启动失败，不会打断已跑起来的一笔。
 *
 *       调用者收到 done 之前不能改动缓冲区；阻塞方式没有 done，返回即完成。
 */

#ifndef __BSP_IIC_H__
#define __BSP_IIC_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 搬运方式
 *============================================================================*/
/**
 * @brief 搬运方式取值：数值越大越快，退档时逐级递减
 * @note 这几个值只描述"这条总线用哪种方式搬"，不是调用者能选的档位：
 *       实际生效值由初始化时从 HAL 句柄探测出来的能力决定。
 */
#define BSP_IIC_CAP_NONE      0U /* 无能两（IIC 总有阻塞方式，保留此值对齐命名） */
#define BSP_IIC_CAP_BLOCKING  1U /* 阻塞：同步搬完再返回 */
#define BSP_IIC_CAP_IT        2U /* 中断：I2C 事件中断推进，完成回调在中断里 */
#define BSP_IIC_CAP_DMA       3U /* DMA：DMA 流搬运，完成回调在流中断里 */

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 传输方向
 */
typedef enum {
    BSP_IIC_DIR_TX = 0, /* 主机发送 */
    BSP_IIC_DIR_RX = 1, /* 主机接收 */
} bsp_iic_dir_t;

/**
 * @brief 传输模式
 */
typedef enum {
    BSP_IIC_MODE_RAW = 0, /* 直接收发，没有寄存器子地址 */
    BSP_IIC_MODE_MEM = 1, /* 带寄存器子地址，repeated START 由 HAL 完成 */
} bsp_iic_mode_t;

/**
 * @brief BSP_IIC 返回码
 * @note 数值即契约：已发布的数值不再重排。
 */
typedef enum {
    BSP_IIC_OK          = 0,  /* 成功；异步方式下表示已交给外设 */
    BSP_IIC_ERR_PARAM   = -1, /* 参数非法 */
    BSP_IIC_ERR_STATE   = -2, /* 未初始化，或上一笔搬运还没收尾 */
    BSP_IIC_ERR_TIMEOUT = -3, /* 外设报告超时：阻塞方式超时，或异步方式收到超时错误 */
    BSP_IIC_ERR_NACK    = -4, /* 从机 NACK */
    BSP_IIC_ERR_BUS     = -5, /* 总线错误（BERR / ARLO / OVR） */
    BSP_IIC_ERR_HAL     = -6, /* HAL 启动失败或其他 HAL 错误 */
} bsp_iic_status_t;

/**
 * @brief 一次传输命令
 * @note 只描述这一笔怎么搬，不描述"等多久"：等待属于调用者。
 */
typedef struct {
    uint16_t dev_addr;      /* 7 bit 从机地址，未左移 */
    uint16_t mem_addr;      /* 寄存器子地址，RAW 模式忽略 */
    uint16_t mem_addr_size; /* 子地址字节数 1 或 2，RAW 模式忽略 */
    uint16_t len;           /* 数据字节数，不能为 0 */
    uint8_t  dir;           /* 方向，取 bsp_iic_dir_t */
    uint8_t  mode;          /* 模式，取 bsp_iic_mode_t */
    uint8_t *data;          /* 收发缓冲区，不能为 NULL */
} bsp_iic_msg_t;

/**
 * @brief IIC 总线句柄（不透明类型，成员定义在 bsp_iic.c）
 * @note  句柄本身是常量，运行块定义在 bsp_iic.c 里，所以实例可以
 *        进 .rodata。
 */
typedef struct bsp_iic bsp_iic_t;

/**
 * @brief 搬运完成回调
 * @param ctx    登记时传入的上下文，原样回传
 * @param status 本次搬运结果，取 bsp_iic_status_t
 * @note 在中断上下文调用（中断 / DMA 两种方式）；回调返回后调用者才可以
 *       把总线交给下一笔。阻塞方式不调用本回调，结果由启动函数的返回值给出。
 */
typedef void (*bsp_iic_done_t)(void *ctx, bsp_iic_status_t status);

/*==============================================================================
 * 外部变量
 *============================================================================*/
extern const bsp_iic_t bsp_iic1; /* I2C1 总线实例；新增一路 IIC 时在这里加一行 */

/*==============================================================================
 * 函数声明
 *============================================================================*/
/**
 * @brief 初始化 IIC 驱动：逐路检查 HAL 句柄与外设状态，并探测搬运能力
 * @retval BSP_IIC_OK: 全部实例就绪（已就绪的实例会被跳过）
 * @retval BSP_IIC_ERR_STATE: 某一路缺 HAL 句柄，或外设不在 READY
 * @note 必须在 MX_I2Cx_Init() 之后调用；本函数不碰任何 OS 对象。
 *       能力探测只看收发 DMA 流有没有挂上：挂了就是 DMA 级，否则中断级；
 *       阻塞级任何情况下都可用。
 */
bsp_iic_status_t BSP_IIC_Init(void);

/**
 * @brief 设置总线时钟
 * @param bus 总线句柄，不能为 NULL
 * @param hz  目标时钟（Hz）；传 0 表示回到初始化时登记的值
 * @retval BSP_IIC_OK: 已生效（或本来就等于目标值）
 * @retval BSP_IIC_ERR_PARAM: 句柄为空，或目标值为 0 且没有可回落的初值
 * @retval BSP_IIC_ERR_STATE: 未初始化，或上一笔搬运还没收尾
 * @retval BSP_IIC_ERR_HAL: HAL 拒绝重新初始化
 * @note 只能在总线空闲时调用：时钟是整条总线共用的，换时钟会改写外设的
 *       时序寄存器，进行中的传输会丢失。
 */
bsp_iic_status_t BSP_IIC_SetSpeed(const bsp_iic_t *bus, uint32_t hz);

/**
 * @brief 启动一次搬运：阻塞方式同步搬完，中断 / DMA 方式登记回调后返回
 * @param bus        总线句柄，不能为 NULL
 * @param msg        传输命令，不能为 NULL；data 在 done 之前必须保持有效
 * @param timeout_ms 仅阻塞方式使用的超时（ms）；中断 / DMA 方式忽略，
 *                   由调用者自己计时并在超时后调 BSP_IIC_Abort()
 * @param done       完成回调（中断 / DMA 方式用），可以为 NULL
 * @param ctx        回调上下文，原样回传
 * @retval BSP_IIC_OK: 阻塞方式已搬完；异步方式已交给外设（完成点只在 done）
 * @retval BSP_IIC_ERR_PARAM: 地址、方向、模式、长度或缓冲区非法
 * @retval BSP_IIC_ERR_STATE: 未初始化，或上一笔还没收尾
 * @retval BSP_IIC_ERR_TIMEOUT: 阻塞方式等外设超时
 * @retval BSP_IIC_ERR_NACK / BSP_IIC_ERR_BUS / BSP_IIC_ERR_HAL: 阻塞方式下
 *         外设报告的错误
 * @note 不能在中断里调用；同一路总线同时只能有一笔在飞。
 *       启动失败时会向下退一级重试，退到阻塞级仍失败才返回错误；退档前
 *       已复位外设，所以调用者可以直接发起下一笔。
 * @note 中断 / DMA 方式的"已启动"从本函数返回时算起。RAW 模式返回时地址
 *       阶段还没走完，由外设事件中断推进；MEM 模式返回时地址阶段已经走完
 *       （子地址已写到总线上），这段时间花在调用线程里，上限由端口实现决
 *       定，敏感任务不要在这里发起 MEM 传输。
 */
bsp_iic_status_t BSP_IIC_StartTransfer(const bsp_iic_t *bus,
                                       const bsp_iic_msg_t *msg,
                                       uint32_t timeout_ms,
                                       bsp_iic_done_t done, void *ctx);

/**
 * @brief 查询总线是否还有搬运未收尾
 * @param bus 总线句柄
 * @retval 1: 忙
 * @retval 0: 空闲（句柄为空也返回 0）
 */
uint8_t BSP_IIC_IsBusy(const bsp_iic_t *bus);

/**
 * @brief 中止当前搬运并把外设复位到可用状态
 * @param bus 总线句柄
 * @retval BSP_IIC_OK: 已复位
 * @retval BSP_IIC_ERR_PARAM: 句柄为空
 * @retval BSP_IIC_ERR_STATE: 未初始化
 * @retval BSP_IIC_ERR_HAL: 复位后 HAL 重新初始化失败
 * @note 中止后不会再调用这一笔登记的 done；调用者自己决定这一笔算失败。
 *       这是超时路径唯一能把外设从 BUSY 拉回来的手段。
 */
bsp_iic_status_t BSP_IIC_Abort(const bsp_iic_t *bus);

#endif /* __BSP_IIC_H__ */
