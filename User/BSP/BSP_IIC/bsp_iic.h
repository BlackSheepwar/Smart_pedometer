/**
 * @file bsp_iic.h
 * @brief IIC 主机驱动层：只启动一次搬运，不等待、不持锁
 * @author Ricardo
 * @date 2026-10-27
 *
 * @note 本层只回答三个问题：外设怎么初始化、总线时钟怎么改、一次搬运怎么
 *       启动。从机地址怎么来、寄存器子地址多宽、总线归谁用、等多久、超时
 *       怎么处理，全在设备层的 IIC 虚拟外设里；所以本层不含任何 OS 调用，
 *       也不认识任何具体器件。
 *
 *       搬运固定走 DMA：启动调用登记完成回调后立即返回，完成或出错时在
 *       中断上下文里调用 done。调用者收到 done 之前不能改动缓冲区。
 */

#ifndef __BSP_IIC_H__
#define __BSP_IIC_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

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
 */
typedef enum {
    BSP_IIC_OK          = 0,  /* 成功（异步启动时表示已交给外设） */
    BSP_IIC_ERR_PARAM   = -1, /* 参数非法 */
    BSP_IIC_ERR_STATE   = -2, /* 未初始化，或上一笔搬运还没收尾 */
    BSP_IIC_ERR_TIMEOUT = -3, /* 外设报告超时错误 */
    BSP_IIC_ERR_NACK    = -4, /* 从机 NACK */
    BSP_IIC_ERR_BUS     = -5, /* 总线错误（BERR / ARLO / OVR） */
    /* -6 空号：数值即契约，重排会把调用者手里的旧比较结果静默改掉 */
    BSP_IIC_ERR_HAL     = -7, /* HAL 启动失败或其他 HAL 错误 */
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
 * @note  句柄本身是常量，运行期状态存在 .c 里的运行时块中，所以实例可以
 *        进 .rodata。
 */
typedef struct bsp_iic bsp_iic_t;

/**
 * @brief 搬运完成回调
 * @param ctx    登记时传入的上下文，原样回传
 * @param status 本次搬运结果，取 bsp_iic_status_t
 * @note 在中断上下文调用；回调返回后调用者才可以把总线交给下一笔。
 *       回调里不要再启动同一路总线的搬运：此时总线仍被登记为忙。
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
 * @brief 初始化 IIC 驱动：逐路检查 HAL 句柄与外设状态
 * @retval BSP_IIC_OK: 全部实例就绪（已就绪的实例会被跳过）
 * @retval BSP_IIC_ERR_STATE: 某一路缺 HAL 句柄，或外设不在 READY
 * @note 必须在 MX_I2Cx_Init() 之后调用；本函数不碰任何 OS 对象，所以
 *       放在初始化链的 BSP 段即可。新增一路 IIC 只需在总线表追加实例。
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
 * @brief 启动一次搬运并立即返回
 * @param bus  总线句柄，不能为 NULL
 * @param msg  传输命令，不能为 NULL；data 在 done 之前必须保持有效
 * @param done 完成回调，可以为 NULL（只能靠 IsBusy 轮询）
 * @param ctx  回调上下文，原样回传
 * @retval BSP_IIC_OK: 已交给外设，完成点只在 done
 * @retval BSP_IIC_ERR_PARAM: 地址、方向、模式、长度或缓冲区非法
 * @retval BSP_IIC_ERR_STATE: 未初始化，或上一笔还没收尾
 * @retval BSP_IIC_ERR_HAL: HAL 拒绝启动（启动失败时本函数已复位外设）
 * @note 不能在中断里调用；同一路总线同时只能有一笔在飞。
 */
bsp_iic_status_t BSP_IIC_StartTransfer(const bsp_iic_t *bus,
                                       const bsp_iic_msg_t *msg,
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
